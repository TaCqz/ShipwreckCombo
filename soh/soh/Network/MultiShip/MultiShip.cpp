#include "MultiShip.h"

#ifdef ENABLE_MULTISHIP

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "soh/ShipUtils.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
// randomizerTypes.h (not randomizerEnums.h) — it's #pragma once guarded, so it
// provides RandomizerGet without re-running the unguarded X-macro enum header
// (which would redefine every rando enum if it's already been pulled in).
#include "soh/Enhancements/randomizer/randomizerTypes.h"
#include "soh/Enhancements/randomizer/randomizerEnumStrings.h"
#include "soh/Enhancements/randomizer/randomizer.h"  // Rando::Context, ItemLocation
#include "soh/Enhancements/randomizer/Traps.h"
#include "soh/Enhancements/custom-message/CustomMessageTypes.h"

extern "C" {
extern SaveContext gSaveContext;
}

// The seed the connected MultiShip server sent this session: the per-RSK settings,
// this world's placements (check -> item) + per-check owner world, the player names,
// our world index, and the seed value. Populated from the network thread (parse only,
// no Context access); consumed on the main thread (file creation + the grant handler),
// so it's mutex-guarded.
struct MultiShipSeed {
    bool valid = false;
    int world = -1;
    uint32_t seed = 0;
    std::vector<uint8_t> settings;
    std::vector<std::pair<int, int>> placements;  // (check, item)
    std::unordered_map<int, int> ownerOf;         // check -> owner world
    std::vector<std::string> players;
};
static std::mutex gMultiShipSeedMutex;
static MultiShipSeed gMultiShipSeed;

// Accessors used by randomizer.cpp (file creation) and hook_handlers.cpp (grant
// routing). All thread-safe copies / lookups.
std::vector<uint8_t> MultiShip_GetServerSettings() {
    std::lock_guard<std::mutex> lk(gMultiShipSeedMutex);
    return gMultiShipSeed.settings;
}
std::vector<std::pair<int, int>> MultiShip_GetServerPlacements() {
    std::lock_guard<std::mutex> lk(gMultiShipSeedMutex);
    return gMultiShipSeed.placements;
}
int MultiShip_GetMyWorld() {
    std::lock_guard<std::mutex> lk(gMultiShipSeedMutex);
    return gMultiShipSeed.world;
}
// Owner world of the item at `check` (-1 if unknown). Used to tell own-world items
// (granted locally) from cross-world ones (sent to another player via the server).
int MultiShip_GetCheckOwner(int check) {
    std::lock_guard<std::mutex> lk(gMultiShipSeedMutex);
    auto it = gMultiShipSeed.ownerOf.find(check);
    return it == gMultiShipSeed.ownerOf.end() ? -1 : it->second;
}
std::string MultiShip_GetPlayerName(int world) {
    std::lock_guard<std::mutex> lk(gMultiShipSeedMutex);
    if (world < 0 || world >= (int)gMultiShipSeed.players.size()) return "";
    return gMultiShipSeed.players[world];
}

// Pending server item deliveries. The network thread only enqueues; the main thread
// (OnGameFrameUpdate) hands them out one at a time, and only while the player can
// actually receive an item — so nothing is delivered during loading / the spawn and
// the get-item animations don't overwrite each other.
struct PendingDelivery {
    uint32_t seq = 0;
    std::string command;        // "give_item randomizer <id>"
    bool isIceTrap = false;
    std::string iceTrapModel;   // RG_* name, optional
    std::string iceTrapText;    // optional
};
static std::mutex gDeliveryMutex;
static std::deque<PendingDelivery> gDeliveryQueue;

// True only when Link is in-game and ready to receive an item (defined in
// hook_handlers.cpp, which has player access).
extern "C" bool Randomizer_PlayerCanReceiveItem(void);

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
    // server can attribute any message — e.g. which player collected an item —
    // regardless of packet ordering or reconnects.
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

    // Request a full check re-report on the main thread (we can't safely read the
    // rando Context from this network thread). This catches the server up on any
    // checks collected while we were disconnected — the server dedupes, so only the
    // genuinely-new ones route items.
    mNeedsCheckResync.store(true);
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

        // The server sends our world's seed: settings, placements (+ owner per check),
        // player names, seed value. We only CACHE it here (no Context access from this
        // network thread); the main thread applies it at file creation and the grant
        // handler reads the owner map. No result packet is expected.
        if (packetType == "placements") {
            MultiShipSeed seed;
            seed.valid = true;
            if (payload.contains("world") && payload["world"].is_number_integer())
                seed.world = payload["world"].get<int>();
            if (payload.contains("seed") && payload["seed"].is_number_unsigned())
                seed.seed = payload["seed"].get<uint32_t>();
            if (payload.contains("settings") && payload["settings"].is_array())
                for (const auto& s : payload["settings"])
                    if (s.is_number_integer()) seed.settings.push_back((uint8_t)s.get<int>());
            if (payload.contains("players") && payload["players"].is_array())
                for (const auto& n : payload["players"])
                    if (n.is_string()) seed.players.push_back(n.get<std::string>());
            if (payload.contains("placements") && payload["placements"].is_array()) {
                for (const auto& p : payload["placements"]) {
                    if (!p.contains("check") || !p.contains("item") ||
                        !p["check"].is_number_integer() || !p["item"].is_number_integer())
                        continue;
                    const int check = p["check"].get<int>();
                    seed.placements.emplace_back(check, p["item"].get<int>());
                    if (p.contains("owner") && p["owner"].is_number_integer())
                        seed.ownerOf[check] = p["owner"].get<int>();
                }
            }
            {
                std::lock_guard<std::mutex> lk(gMultiShipSeedMutex);
                gMultiShipSeed = std::move(seed);
            }
            SPDLOG_INFO("[MultiShip] Cached server seed (world {}, {} placements)",
                        gMultiShipSeed.world, gMultiShipSeed.placements.size());
            return;
        }

        // Only command packets are handled beyond this point; ignore anything else.
        if (packetType != "command") {
            return;
        }

        if (!payload.contains("command") || !payload["command"].is_string()) {
            SPDLOG_ERROR("[MultiShip] Received command payload without a command");
            SendJsonToRemote(response);
            return;
        }

        std::string command = payload["command"].get<std::string>();
        bool cmdIsIceTrap = false;
        std::string cmdIceTrapModel, cmdIceTrapText;

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
            if (tokens.size() >= 3 && tokens[0] == "give_item" && tokens[1] == "randomizer") {
                std::optional<RandomizerGet> rg = StringToEnum<RandomizerGet>(tokens[2]);
                if (rg.has_value()) {
                    tokens[2] = std::to_string(static_cast<int>(*rg));
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
                    // Already a numeric id.
                    rg = static_cast<RandomizerGet>(std::stoi(tokens[2]));
                }

                // For an ice trap, capture the server-provided disguise model/text.
                // They're applied right before the item is actually granted (now for a
                // manual send, or at drain time for a queued multiworld delivery), so a
                // queued trap doesn't clobber an earlier one. Both are optional.
                if (rg.has_value() && *rg == RG_ICE_TRAP) {
                    cmdIsIceTrap = true;
                    if (payload.contains("iceTrapModel") && payload["iceTrapModel"].is_string()) {
                        cmdIceTrapModel = payload["iceTrapModel"].get<std::string>();
                    }
                    if (payload.contains("iceTrapText") && payload["iceTrapText"].is_string()) {
                        cmdIceTrapText = payload["iceTrapText"].get<std::string>();
                    }
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
        if (isMultiShipItem) {
            PendingDelivery d;
            d.seq = payload["seq"].get<uint32_t>();
            d.command = command;
            d.isIceTrap = cmdIsIceTrap;
            d.iceTrapModel = cmdIceTrapModel;
            d.iceTrapText = cmdIceTrapText;
            {
                std::lock_guard<std::mutex> lk(gDeliveryMutex);
                gDeliveryQueue.push_back(std::move(d));
            }
            response["status"] = "success";
            SendJsonToRemote(response);
            return;
        }

        // Manual command (e.g. the GUI "Send Item"): dispatch immediately. Apply the
        // ice-trap disguise first, since it's consumed by the give path.
        if (cmdIsIceTrap) {
            if (!cmdIceTrapModel.empty()) {
                if (std::optional<RandomizerGet> m = StringToEnum<RandomizerGet>(cmdIceTrapModel)) {
                    Rando::Traps::SetNextIceTrapModel(*m);
                }
            }
            if (!cmdIceTrapText.empty()) {
                Rando::Traps::SetNextIceTrapText(cmdIceTrapText);
            }
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
    // created in the MultiShip gamemode are reported — other quests (vanilla,
    // rando, boss rush, ...) are irrelevant to a MultiShip session.
    //
    // NOTE: OnLoadGame fires from the file-select gamestate, before Play_Init
    // runs, so gPlayState is still NULL and GameInteractor::IsSaveLoaded() would
    // return false here. We must NOT gate on it or the packet is never sent. The
    // selected file's quest is already populated in gSaveContext at this point.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadGame>([&](int32_t fileNum) {
        if (!isConnected || gSaveContext.ship.quest.id != QUEST_MULTISHIP)
            return;

        SendOnLoadGame();
    });

    // Full check re-report after a (re)connect — the main-thread counterpart of the
    // OnConnected flag. Re-reports every already-collected check so the server learns
    // about anything collected while we were disconnected (it dedupes, so only new
    // ones route items). This runs on the main thread, where reading the rando
    // Context is safe. The per-frame cost is just one atomic load until the flag is
    // set, so it's negligible.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>([&]() {
        // --- Deliver queued server items, one per ready frame ----------------------
        // Hand out at most one queued item, and only while Link can actually receive
        // one (not during loading / the spawn cutscene, and not while another get-item
        // animation is playing). This is what stops items being delivered before the
        // player is in-game and stops rapid deliveries from overwriting each other.
        if (isConnected && gSaveContext.ship.quest.id == QUEST_MULTISHIP &&
            Randomizer_PlayerCanReceiveItem()) {
            PendingDelivery d;
            bool have = false;
            {
                std::lock_guard<std::mutex> lk(gDeliveryMutex);
                if (!gDeliveryQueue.empty()) {
                    d = gDeliveryQueue.front();
                    have = true;
                }
            }
            if (have) {
                // Skip an already-applied re-send; otherwise grant it and advance the
                // persisted high-water mark (saved atomically with the inventory).
                if (d.seq >= gSaveContext.ship.multishipReceivedSeq) {
                    if (d.isIceTrap) {
                        if (!d.iceTrapModel.empty()) {
                            if (std::optional<RandomizerGet> m = StringToEnum<RandomizerGet>(d.iceTrapModel)) {
                                Rando::Traps::SetNextIceTrapModel(*m);
                            }
                        }
                        if (!d.iceTrapText.empty()) {
                            Rando::Traps::SetNextIceTrapText(d.iceTrapText);
                        }
                    }
                    std::reinterpret_pointer_cast<Ship::ConsoleWindow>(
                        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"))
                        ->Dispatch(d.command);
                    gSaveContext.ship.multishipReceivedSeq = d.seq + 1;
                }
                std::lock_guard<std::mutex> lk(gDeliveryMutex);
                if (!gDeliveryQueue.empty()) {
                    gDeliveryQueue.pop_front();
                }
            }
        }

        // --- Full check re-report after a (re)connect ------------------------------
        if (!mNeedsCheckResync.load())
            return;
        if (!isConnected || gSaveContext.ship.quest.id != QUEST_MULTISHIP) {
            mNeedsCheckResync.store(false);  // request no longer relevant
            return;
        }
        if (!GameInteractor::IsSaveLoaded())
            return;  // not in-game yet; keep the flag and retry next frame

        mNeedsCheckResync.store(false);
        auto ctx = Rando::Context::GetInstance();
        if (ctx == nullptr)
            return;
        int reported = 0;
        for (int rc = 0; rc < RC_MAX; rc++) {
            auto loc = ctx->GetItemLocation(static_cast<RandomizerCheck>(rc));
            if (loc != nullptr && loc->HasObtained()) {
                nlohmann::json payload;
                payload["id"] = ShipUtils::Random(0, UINT32_MAX);
                payload["type"] = "hook";
                payload["hook"]["type"] = "OnCheckCollected";
                payload["hook"]["check"] = rc;
                SendJsonToRemote(payload);
                ++reported;
            }
        }
        SPDLOG_INFO("[MultiShip] Re-reported {} collected checks after (re)connect", reported);
    });

    // Collecting a randomizer check (a location). This is what the server routes:
    // it looks up who owns the item at this location and delivers it to them.
    // OnRandoSetCheckStatus hands us the RandomizerCheck directly. We report on
    // both COLLECTED (just collected) and SAVED (already-obtained, surfaced on
    // load) — the server dedupes via its session log, so re-reporting is harmless
    // and rebuilds the server's routing state if its .session was lost.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnRandoSetCheckStatus>(
        [&](RandomizerCheck rc, RandomizerCheckStatus status) {
            if (!isConnected || gSaveContext.ship.quest.id != QUEST_MULTISHIP)
                return;
            if (status != RCSHOW_COLLECTED && status != RCSHOW_SAVED)
                return;

            nlohmann::json payload;
            payload["id"] = ShipUtils::Random(0, UINT32_MAX);
            payload["type"] = "hook";
            payload["hook"]["type"] = "OnCheckCollected";
            payload["hook"]["check"] = static_cast<int>(rc);
            SendJsonToRemote(payload);
        });

    // Receiving an item.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnItemReceive>([&](GetItemEntry itemEntry) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        nlohmann::json payload;
        payload["id"] = ShipUtils::Random(0, UINT32_MAX);
        payload["type"] = "hook";
        payload["hook"]["type"] = "OnItemReceive";
        payload["hook"]["tableId"] = itemEntry.tableId;
        payload["hook"]["getItemId"] = itemEntry.getItemId;
        SendJsonToRemote(payload);
    });

    // Defeating a boss. OnBossDefeat is already filtered to boss enemies only,
    // so no manual category filtering is needed here.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnBossDefeat>([&](void* refActor) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        Actor* actor = (Actor*)refActor;
        nlohmann::json payload;
        payload["id"] = ShipUtils::Random(0, UINT32_MAX);
        payload["type"] = "hook";
        payload["hook"]["type"] = "OnBossDefeat";
        payload["hook"]["actorId"] = actor->id;
        payload["hook"]["params"] = actor->params;
        SendJsonToRemote(payload);
    });

    // Dying and getting damaged both surface through the health-change hook.
    // This fires after the health value has been updated, so we can inspect the
    // resulting health to distinguish a death from non-lethal damage.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayerHealthChange>([&](int16_t amount) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        nlohmann::json payload;
        payload["id"] = ShipUtils::Random(0, UINT32_MAX);
        payload["type"] = "hook";
        if (gSaveContext.health <= 0) {
            payload["hook"]["type"] = "OnPlayerDeath";
        } else if (amount < 0) {
            payload["hook"]["type"] = "OnPlayerDamage";
            payload["hook"]["amount"] = amount;
        } else {
            return;
        }
        SendJsonToRemote(payload);
    });

    // Show the textbox for a server-sent ice trap. SoH only registers the
    // TEXT_RANDOMIZER_CUSTOM_ITEM handler for randomizer seeds (IS_RANDO), so in a
    // MultiShip game the ice trap textbox would otherwise fall back to its raw id.
    // We register our own: when a server-provided text is pending, build the
    // message from it. No pending text means the textbox isn't ours, so we leave
    // it untouched.
    GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnOpenText>(
        TEXT_RANDOMIZER_CUSTOM_ITEM, [&](uint16_t* textId, bool* loadFromMessageTable) {
            if (!isConnected) {
                return;
            }
            std::optional<std::string> text = Rando::Traps::TakeNextIceTrapText();
            if (!text.has_value()) {
                return;
            }
            CustomMessage msg(*text, *text, *text, { QM_BLUE, QM_BLUE, QM_BLUE });
            msg.AutoFormat();
            *loadFromMessageTable = false;
            msg.LoadIntoFont();
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
