#include "MultiShip.h"

#ifdef ENABLE_MULTISHIP

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include "soh/ShipUtils.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
// randomizerTypes.h (not randomizerEnums.h) — it's #pragma once guarded, so it
// provides RandomizerGet without re-running the unguarded X-macro enum header
// (which would redefine every rando enum if it's already been pulled in).
#include "soh/Enhancements/randomizer/randomizerTypes.h"
#include "soh/Enhancements/randomizer/randomizerEnumStrings.h"
#include "soh/Enhancements/custom-message/CustomMessageTypes.h"  // TEXT_RANDOMIZER_CUSTOM_ITEM
#include "MultiShipSeed.h"

extern "C" {
extern SaveContext gSaveContext;
}

// SoH's get-item textbox builder for randomizer items (defined in
// Enhancements/randomizer/Messages/ItemMessages.cpp). It reads the item being
// received from the player and builds a "You found X!" message inline from the
// static item catalog — no generated seed / live rando Context needed. SoH only
// auto-registers it for IS_RANDO seeds; a MultiShip game is NOT IS_RANDO, so we
// register it ourselves (below) for delivered items. Not declared in a header.
void BuildItemMessage(uint16_t* textId, bool* loadFromMessageTable);

// Pending server item deliveries. The network thread only enqueues; the main thread
// (OnGameFrameUpdate) hands them out one at a time, and only while the player can
// actually receive an item — so nothing is delivered during loading / the spawn and
// the get-item animations don't overwrite each other.
struct PendingDelivery {
    // A tracked delivery is part of the server's crash-safe stream: it carries a real
    // `seq`, is deduped against the persisted high-water mark, and advances it on grant.
    // An untracked one is a manual GUI "Send Item" (no seq): it still drains one-at-a-time
    // through the idle-gate + grant confirmation so it isn't lost mid-animation, but it
    // never touches multishipReceivedSeq.
    bool tracked = true;
    uint32_t seq = 0;
    std::string command;  // "give_item randomizer <id>"
    int rgId = -1;        // the RandomizerGet id this give hands out (for OnItemReceive match)
    // The (modIndex, getItemId) the give will actually grant, resolved ONCE on the first
    // delivery attempt (pre-give inventory) so progressive items match correctly on receipt.
    // -1 until resolved.
    int expectModIndex = -1;
    int expectGetItemId = -1;
};
static std::mutex gDeliveryMutex;
static std::deque<PendingDelivery> gDeliveryQueue;

// True when Link is in-game and able to START a get-item (defined in hook_handlers.cpp,
// which has player access). Does NOT check getItemId, so the drain can re-attempt a give
// that staged but stranded; an in-progress get-item is still excluded.
extern "C" bool Randomizer_PlayerCanReceiveItem(void);
// Resolves the item a queued give hands out to (modIndex, getItemId) so the drain can
// match it on OnItemReceive. Must be called BEFORE the give (pre-give inventory) for
// progressive items to resolve to the tier that will actually be received.
extern "C" void Randomizer_ResolveGive(int rgId, int* outModIndex, int* outGetItemId);
// Diagnostic: logs why the player currently can't receive an item (stall debugging).
extern "C" void Randomizer_LogReceiveBlockReason(void);

void MultiShip::Connect() {
    // The "Connect" menu button toggles the connection. The underlying Network
    // base class handles the actual TCP connection (and auto-reconnect) on its
    // own thread; we just feed it the configured host/port.
    if (isEnabled) {
        SPDLOG_INFO("[MultiShip] Disconnecting from server");
        Network::Disable();
        return;
    }

    std::string host = CVarGetString(CVAR_REMOTE_MULTISHIP("Host"), "127.0.0.1");
    uint16_t port = CVarGetInteger(CVAR_REMOTE_MULTISHIP("Port"), 43384);
    SPDLOG_INFO("[MultiShip] Connecting to {}:{}", host, port);
    Network::Enable(host.c_str(), port);
}

void MultiShip::SendJsonToRemote(nlohmann::json packet) {
    // Attach the player's name to every packet (not just the handshake) so the
    // server can attribute any message regardless of packet ordering or reconnects.
    packet["userName"] = CVarGetString(CVAR_REMOTE_MULTISHIP("UserName"), "");
    Network::SendJsonToRemote(packet);
}

void MultiShip::SendOnLoadGame() {
    // Reports the currently loaded MultiShip file to the server. Shared by the
    // OnLoadGame hook (fresh load while connected) and OnConnected (connecting
    // while a MultiShip file is already loaded).
    nlohmann::json payload;
    payload["id"] = ShipUtils::Random(0, UINT32_MAX);
    payload["type"] = "hook";
    payload["hook"]["type"] = "OnLoadGame";
    payload["hook"]["fileNum"] = gSaveContext.fileNum;
    // Quest/mode of the loaded file (Quest enum: 0 Normal, 1 Master, 2 Rando,
    // 3 Boss Rush, 4 MultiShip). Sourced from the loaded save, not the carousel.
    payload["hook"]["questId"] = gSaveContext.ship.quest.id;
    // Crash-safe catch-up: tell the server the highest delivery seq we've applied
    // (persisted atomically with our inventory). The server re-sends everything
    // past it, re-granting exactly what a crash lost — nothing already applied.
    payload["hook"]["receivedSeq"] = gSaveContext.ship.multishipReceivedSeq;
    SPDLOG_INFO("[MultiShip] Sending OnLoadGame (fileNum {}, questId {}, receivedSeq {})",
                gSaveContext.fileNum, gSaveContext.ship.quest.id, gSaveContext.ship.multishipReceivedSeq);
    SendJsonToRemote(payload);
}

// --- File-select menu C accessors (used by z_file_choose.c) ------------------------
// "Start save" is enabled only when connected, the chosen user name is one of the seed's
// players, and a full seed has been received (so there is something to persist).
extern "C" bool MultiShip_CanStartSave(void) {
    if (MultiShip::Instance == nullptr || !MultiShip::Instance->isConnected) {
        return false;
    }
    std::string name = CVarGetString(CVAR_REMOTE_MULTISHIP("UserName"), "");
    return MultiShipSeed::IsNameValid(name) && MultiShipSeed::IsReady();
}

// One-line status for the file-select MultiShip menu. Static buffer (the menu draws it
// once per frame on the main thread, so this is safe).
extern "C" const char* MultiShip_FileSelectStatus(void) {
    static std::string status;
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    const bool connected = MultiShip::Instance != nullptr && MultiShip::Instance->isConnected;
    if (d.ready && d.worldId >= 0 && d.worldId < (int)d.players.size()) {
        status = "Seed loaded for " + d.players[d.worldId];
    } else if (d.ready) {
        status = "Seed loaded";
    } else if (connected) {
        status = "Connected - use Start Multiworld Save in the Network menu";
    } else {
        status = "Not connected - press Connect";
    }
    return status.c_str();
}

void MultiShip::RequestStartMultiworldSave() {
    // Triggered by the 'Start Multiworld Save' menu button. Ask the server for the seed
    // matching the configured user name; it validates the name, locks that world to us,
    // and replies with the full v3 SeedData (handled in OnIncomingJson). The button is
    // greyed unless connected + valid name, but we re-check here defensively.
    std::string name = CVarGetString(CVAR_REMOTE_MULTISHIP("UserName"), "");
    if (!isConnected) {
        MultiShipSeed::SetStatus("Not connected");
        return;
    }
    if (name.empty()) {
        MultiShipSeed::SetStatus("Enter a user name first");
        return;
    }
    nlohmann::json payload;
    payload["id"] = ShipUtils::Random(0, UINT32_MAX);
    payload["type"] = "start_multiworld_save";
    payload["playerName"] = name;
    SPDLOG_INFO("[MultiShip] Requesting 'Start Multiworld Save' for '{}'", name);
    MultiShipSeed::SetStatus("Requesting seed for '" + name + "'...");
    SendJsonToRemote(payload);
}

void MultiShip::OnConnected() {
    // Announce ourselves so the server has something to display immediately.
    // The user name is added to every packet by SendJsonToRemote().
    nlohmann::json payload;
    payload["id"] = ShipUtils::Random(0, UINT32_MAX);
    payload["type"] = "hook";
    payload["hook"]["type"] = "OnConnected";
    SendJsonToRemote(payload);

    // If we connect while a MultiShip file is already loaded (the common case:
    // the player opens the in-game menu and connects mid-game), report it right
    // away — the OnLoadGame hook only fires on a fresh load. The game hooks are
    // registered once at boot on the main thread (see the RegisterShipInitFunc
    // at the bottom of this file); they must NOT be registered from this network
    // thread, as GameInteractor's hook registry is only safe to mutate on the
    // main thread.
    if (GameInteractor::IsSaveLoaded() && gSaveContext.ship.quest.id == QUEST_MULTISHIP) {
        SendOnLoadGame();
    }
}

void MultiShip::OnIncomingJson(nlohmann::json payload) {
    SPDLOG_INFO("[MultiShip] Received payload: \n{}", payload.dump());

    // Mirrors Sail: a {"type":"command","command":"..."} packet runs that command
    // through the SoH console, exactly as if it had been typed in-game. A result
    // is sent back so the server can see whether it succeeded.
    nlohmann::json response;
    response["type"] = "result";
    response["status"] = "failure";
    if (payload.contains("id")) {
        response["id"] = payload["id"];
    }

    try {
        if (!payload.contains("type") || !payload["type"].is_string()) {
            SPDLOG_ERROR("[MultiShip] Received payload without a type");
            SendJsonToRemote(response);
            return;
        }

        const std::string packetType = payload["type"].get<std::string>();

        // MultiShip seed handshake (F-035 Part B). The server pushes the seed's world
        // names on connect, and replies to a 'Start Multiworld Save' request with the
        // full v3 SeedData (or a denial). These carry no command and need no response.
        if (packetType == "multiworld_seed_info") {
            std::string seedId = payload.value("seedId", std::string());
            std::vector<std::string> players;
            if (payload.contains("players") && payload["players"].is_array()) {
                for (const auto& p : payload["players"]) {
                    if (p.is_string()) {
                        players.push_back(p.get<std::string>());
                    }
                }
            }
            MultiShipSeed::SetKnownPlayers(seedId, players);
            SPDLOG_INFO("[MultiShip] Seed info: {} world name(s) in seed {}", players.size(), seedId);
            return;
        }
        if (packetType == "multiworld_seed") {
            int worldId = payload.value("worldId", -1);
            std::string data = payload.value("data", std::string());
            std::string err;
            if (!data.empty() && MultiShipSeed::DeserializeV3FromBase64(data, worldId, err)) {
                std::string who = payload.value("playerName", std::string());
                MultiShipSeed::SetStatus("Seed received (world " + std::to_string(worldId + 1) +
                                         (who.empty() ? "" : ": " + who) + ")");
            } else {
                MultiShipSeed::SetStatus("Seed receive failed: " + (err.empty() ? "empty data" : err));
                SPDLOG_ERROR("[MultiShip] Failed to deserialize seed: {}", err);
            }
            return;
        }
        if (packetType == "multiworld_seed_denied") {
            std::string reason = payload.value("reason", std::string("denied"));
            MultiShipSeed::SetStatus("Denied: " + reason);
            SPDLOG_WARN("[MultiShip] 'Start Multiworld Save' denied: {}", reason);
            return;
        }

        // Only command packets are handled beyond this point. Anything else is accepted
        // and ignored without a response, so an unexpected packet never crashes.
        if (packetType != "command") {
            return;
        }

        if (!payload.contains("command") || !payload["command"].is_string()) {
            SPDLOG_ERROR("[MultiShip] Received command payload without a command");
            SendJsonToRemote(response);
            return;
        }

        std::string command = payload["command"].get<std::string>();
        bool cmdIsGive = false;  // a give_item command (the only kind routed through the item queue)
        int cmdRgId = -1;        // the resolved RandomizerGet id of a give (for OnItemReceive match)

        // The "give_item randomizer <item>" console command expects a numeric
        // RandomizerGet id, but commands arrive with the enum NAME (e.g.
        // RG_KOKIRI_SWORD). Translate the name to its id before dispatching so the
        // command works (and doesn't hit give_item's unguarded std::stoi).
        {
            std::istringstream iss(command);
            std::vector<std::string> tokens;
            for (std::string tok; iss >> tok;) {
                tokens.push_back(tok);
            }
            cmdIsGive = !tokens.empty() && tokens[0] == "give_item";
            if (tokens.size() >= 3 && tokens[0] == "give_item" && tokens[1] == "randomizer") {
                std::optional<RandomizerGet> rg = StringToEnum<RandomizerGet>(tokens[2]);
                if (rg.has_value()) {
                    cmdRgId = static_cast<int>(*rg);
                    tokens[2] = std::to_string(cmdRgId);
                    command.clear();
                    for (size_t i = 0; i < tokens.size(); ++i) {
                        if (i != 0) {
                            command += ' ';
                        }
                        command += tokens[i];
                    }
                } else if (tokens[2].find_first_not_of("0123456789") != std::string::npos) {
                    // Not a known RandomizerGet name and not a plain numeric id.
                    SPDLOG_ERROR("[MultiShip] Unknown RandomizerGet item: {}", tokens[2]);
                    SendJsonToRemote(response);
                    return;
                } else {
                    // Already a numeric id; dispatch as-is.
                    cmdRgId = std::stoi(tokens[2]);
                }
            }
        }

        // MultiShip crash-safe delivery: a server-routed item carries a monotonic
        // `seq` and the `multiship` flag. Rather than grant it here on the network
        // thread (which would deliver during loading and overwrite the previous
        // get-item before its animation finishes), QUEUE it. The main thread drains
        // the queue one item at a time, only while the player can receive, and
        // advances the persisted high-water mark per actual grant.
        const bool isMultiShipItem = payload.value("multiship", false) && payload.contains("seq") &&
                                     payload["seq"].is_number_unsigned();
        // A manual GUI "Send Item" arrives as a plain give_item command (no `multiship`
        // flag / `seq`). It still gives an item, so it must NOT be dispatched here on
        // the network thread during loading / mid-animation — that's exactly the drop
        // the queue exists to prevent. Route it through the SAME queue, but untracked:
        // the main thread drains it through the idle-gate + grant confirmation so it's
        // never lost, while it stays out of the crash-safe seq stream (no dedup, never
        // advances multishipReceivedSeq). Non-give commands (e.g. the Teleport button's
        // `entrance <hex>`) fall through to immediate dispatch below.
        if (isMultiShipItem || cmdIsGive) {
            PendingDelivery d;
            d.tracked = isMultiShipItem;
            if (isMultiShipItem) {
                d.seq = payload["seq"].get<uint32_t>();
            }
            d.command = command;
            d.rgId = cmdRgId;
            {
                std::lock_guard<std::mutex> lk(gDeliveryMutex);
                gDeliveryQueue.push_back(std::move(d));
            }
            SPDLOG_INFO("[MultiShip] Queued give (tracked={}, seq={}): {}", isMultiShipItem,
                        isMultiShipItem ? payload["seq"].get<uint32_t>() : 0, command);
            response["status"] = "success";
            SendJsonToRemote(response);
            return;
        }

        // Non-give command (e.g. the server's Teleport button: `entrance <hex>`).
        // MultiShip server commands only apply in a MultiShip game, so ignore it unless
        // a QUEST_MULTISHIP file is loaded; otherwise dispatch immediately through the
        // existing SoH console handler.
        if (!GameInteractor::IsSaveLoaded() || gSaveContext.ship.quest.id != QUEST_MULTISHIP) {
            SPDLOG_INFO("[MultiShip] Ignoring command (not in a MultiShip game): {}", command);
            SendJsonToRemote(response);
            return;
        }
        std::reinterpret_pointer_cast<Ship::ConsoleWindow>(
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"))
            ->Dispatch(command);

        response["status"] = "success";
        SendJsonToRemote(response);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[MultiShip] Exception handling command: {}", e.what());
        SendJsonToRemote(response);
    }
}

void MultiShip::RegisterHooks() {
    // Registered ONCE at boot on the main thread (see the RegisterShipInitFunc
    // below). Each body is gated on `isConnected`, so the hooks only do anything
    // while connected to a MultiShip server. We intentionally register here
    // rather than from OnConnected: OnConnected runs on the network thread, and
    // GameInteractor's hook registry is only safe to mutate on the main thread —
    // registering from the network thread left the hooks silently never firing.

    // Loading a save file (entering gameplay from the file select). Only files
    // created in the MultiShip gamemode are reported — other quests are irrelevant.
    //
    // NOTE: OnLoadGame fires from the file-select gamestate, before Play_Init runs,
    // so gPlayState is still NULL and GameInteractor::IsSaveLoaded() would return
    // false here. We must NOT gate on it or the packet is never sent. The selected
    // file's quest is already populated in gSaveContext at this point.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadGame>([&](int32_t fileNum) {
        if (!isConnected || gSaveContext.ship.quest.id != QUEST_MULTISHIP)
            return;
        SendOnLoadGame();
    });

    // Deliver queued server items by RE-ATTEMPTING the front item every frame Link is able
    // to start a get-item — exactly like SoH's own randomizer item delivery
    // (RandomizerOnPlayerUpdateForItemQueueHandler). The give only STAGES player->getItemId;
    // the player's action handlers (later in the same player update) turn it into the actual
    // get-item. A single attempt can stage getItemId yet fail to "take" (it strands), so we
    // re-issue every eligible frame until the item is actually RECEIVED — confirmed by the
    // OnItemReceive hook below, which pops the queue. Registered on OnPlayerUpdate (not
    // OnGameFrameUpdate) so the give lands at the right point in the frame; the readiness
    // gate excludes an in-progress get-item / freeze, so re-attempts never duplicate or
    // interrupt one.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayerUpdate>([&]() {
        if (!isConnected || gSaveContext.ship.quest.id != QUEST_MULTISHIP) {
            return;
        }
        if (!Randomizer_PlayerCanReceiveItem()) {
            // Diagnostic: if something is queued but we can't (re)attempt, log WHY
            // (throttled) so a stall's cause is visible instead of guessed at.
            bool queued;
            {
                std::lock_guard<std::mutex> lk(gDeliveryMutex);
                queued = !gDeliveryQueue.empty();
            }
            if (queued) {
                static uint32_t sBlockLog = 0;
                if ((sBlockLog++ % 120) == 0) {
                    Randomizer_LogReceiveBlockReason();
                }
            }
            return;
        }

        PendingDelivery d;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(gDeliveryMutex);
            if (!gDeliveryQueue.empty()) {
                PendingDelivery& front = gDeliveryQueue.front();
                // Resolve the item this give will grant ONCE, now, against the pre-give
                // inventory — so a progressive item matches the exact tier on receipt.
                if (front.expectModIndex < 0 && front.rgId >= 0) {
                    Randomizer_ResolveGive(front.rgId, &front.expectModIndex, &front.expectGetItemId);
                }
                d = front;
                have = true;
            }
        }
        if (!have) {
            return;
        }

        if (d.tracked && d.seq < gSaveContext.ship.multishipReceivedSeq) {
            // Already-applied re-send (crash catch-up): drop it without re-granting.
            std::lock_guard<std::mutex> lk(gDeliveryMutex);
            if (!gDeliveryQueue.empty()) {
                gDeliveryQueue.pop_front();
            }
            return;
        }

        // (Re-)issue the give for the front item. We do NOT pop here — the OnItemReceive
        // hook below pops once the item is actually received, so a stranded give is retried
        // next frame instead of being lost. The log is throttled so a multi-frame retry
        // doesn't spam.
        {
            static uint32_t sDeliverLog = 0;
            if ((sDeliverLog++ % 20) == 0) {
                SPDLOG_INFO("[MultiShip] Delivering (rg={}, tracked={}, seq={}): {}", d.rgId, d.tracked, d.seq,
                            d.command);
            }
        }
        std::reinterpret_pointer_cast<Ship::ConsoleWindow>(
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"))
            ->Dispatch(d.command);
    });

    // Confirm + pop a delivered item once it's actually received. The drain re-issues the
    // front give every eligible frame; this fires when the get-item completes (for ice
    // traps, when the deferred freeze is applied — ExtraTraps raises OnItemReceive then).
    // Match the received item to the front delivery so an unrelated world pickup can't pop
    // it, and advance the persisted seq only for tracked stream deliveries.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnItemReceive>([&](GetItemEntry itemEntry) {
        if (!isConnected || gSaveContext.ship.quest.id != QUEST_MULTISHIP) {
            return;
        }
        std::lock_guard<std::mutex> lk(gDeliveryMutex);
        if (gDeliveryQueue.empty()) {
            return;
        }
        const PendingDelivery& d = gDeliveryQueue.front();
        // Only pop if this received item is the one our in-flight give resolved to (set on
        // the first delivery attempt). Guards against an unrelated world pickup popping the
        // queue, and against popping before we've even attempted the front item.
        if (d.expectModIndex < 0 || static_cast<int>(itemEntry.modIndex) != d.expectModIndex ||
            static_cast<int>(itemEntry.getItemId) != d.expectGetItemId) {
            return;
        }
        if (d.tracked) {
            gSaveContext.ship.multishipReceivedSeq = d.seq + 1;
        }
        SPDLOG_INFO("[MultiShip] Confirmed received (rg={}, tracked={}, seq={})", d.rgId, d.tracked, d.seq);
        gDeliveryQueue.pop_front();
    });

    // Get-item textbox for delivered randomizer items. Items the rando table stores as
    // plain vanilla (MOD_NONE: Kokiri Sword, tunics, ...) already show their normal
    // "You got X" box. Items stored as MOD_RANDOMIZER (Master Sword, bottles, keys, ...)
    // use TEXT_RANDOMIZER_CUSTOM_ITEM, whose builder SoH only registers for IS_RANDO
    // seeds — so in a (non-rando) MultiShip session they'd pop a blank box. Register the
    // same builder here, gated on being connected. It reads the item from the player and
    // builds the message inline, so it needs no generated seed.
    // TODO: later, prefix the box with the sending player's name.
    GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnOpenText>(
        TEXT_RANDOMIZER_CUSTOM_ITEM, [&](uint16_t* textId, bool* loadFromMessageTable) {
            if (!isConnected || gSaveContext.ship.quest.id != QUEST_MULTISHIP) {
                return;
            }
            BuildItemMessage(textId, loadFromMessageTable);
        });
}

// Register the MultiShip game hooks once, at boot, on the main thread. By this
// point both GameInteractor::Instance and MultiShip::Instance have been created
// (see OTRGlobals init order). The hooks stay registered for the whole session
// and no-op while disconnected.
static RegisterShipInitFunc multiShipInitFunc([]() {
    if (MultiShip::Instance != nullptr) {
        MultiShip::Instance->RegisterHooks();
    }
});

#endif // ENABLE_MULTISHIP
