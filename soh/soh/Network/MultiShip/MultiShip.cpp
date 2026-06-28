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
// F-040: feed the native check-detection / give-item-replacement pipeline (registered for
// MultiShip in hook_handlers.cpp) from the F-035 seed store by populating the otherwise-empty
// randomizer Context with our world's placements. ITEM FLOW ONLY — no settings are applied.
#include "soh/Enhancements/randomizer/SeedContext.h"  // Rando::Context
#include "soh/Enhancements/randomizer/item_location.h"  // Rando::ItemLocation, RCSHOW_COLLECTED
#include "soh/Enhancements/randomizer/item_override.h"   // Rando::ItemOverride (ice-trap disguise)
#include "soh/Enhancements/randomizer/Traps.h"           // Rando::Traps::GetTrapTrickModel / GetTrapName
#include "soh/Enhancements/randomizer/static_data.h"     // Rando::StaticData
#include "soh/SaveManager.h"                            // SaveSection, SECTION_ID_MULTISHIP
#include <atomic>
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
// Translates a received rando item that is a vanilla equipment upgrade for us (strength →
// UPG_STRENGTH) into that vanilla upgrade, so it shows in the equipment subscreen + takes effect.
// Defined in hook_handlers.cpp (which has the game-side macros/functions). No-op for other items.
extern "C" void Randomizer_MultiShipApplyVanillaUpgrade(int modIndex, int getItemId);
// Grants the Link's Pocket starting dungeon reward (F-041), reusing the proven rando StartingItemGive
// path. Defined in soh/Enhancements/randomizer/savefile.cpp. The reward RandomizerGet comes from the
// RC_LINKS_POCKET placement; this side owns the once-per-save guard (MultiShip_GrantStartingReward).
extern "C" void Randomizer_MultiShipGiveStartingReward(int rgItem);
// Bakes the one-time area-access world-state flags (open forest Mido block, Door of Time, Kakariko
// gate, Gerudo carpenters) from the live Rando::Context settings (F-043). Defined in savefile.cpp;
// idempotent + "open"-only so it is safe to re-run. We call it after applying the synced settings.
extern "C" void Randomizer_ApplyAreaAccessWorldState();
// Configures Ganon's Trials from the synced RSK_TRIAL_COUNT (F-043b): marks that many trials
// required + bakes the COMPLETED flag for the skipped ones. Defined in savefile.cpp; idempotent.
extern "C" void Randomizer_MultiShipApplyTrials();
// Applies the one-time starting state from the synced "Logic" settings (F-044): adult age + Master
// Sword, full wallets, Skip Child Zelda letter + lullaby + flags, completed masks. Defined in
// savefile.cpp; the caller copies the synced settings to the Context first + owns the once guard.
extern "C" void Randomizer_MultiShipApplyStartState();
// Applies the Skip Child Stealth entrance remap (F-044) from the live Context. Defined in
// randomizer_entrance.c (MultiShip never runs Entrance_Init, so it isn't patched there). Idempotent;
// reads RSK_SKIP_CHILD_STEALTH, so we call it after copying the synced settings to the Context.
extern "C" void Randomizer_MultiShipApplySkipChildStealth();

// --- F-040 cross-world item flow ---------------------------------------------------
// Set on the network thread when a full seed is (re)received; consumed on the main thread
// (the only place it's safe to touch the rando Context / fire GameInteractor hooks).
static std::atomic<bool> gNeedsContextApply{ false };

// Our world index in the loaded seed, or -1 if no seed. Linked by the rando item pipeline
// (hook_handlers.cpp) — plain C++ linkage, declared there with a matching forward decl.
int MultiShip_GetMyWorld(void) {
    return MultiShipSeed::Snapshot().worldId;
}

// The player/world name for world index `world`, or "" if out of range. Used by the get-item
// textbox reword (ItemMessages.cpp BuildCustomItemMessage) to show "<Player>'s <item>".
std::string MultiShip_GetPlayerName(int world) {
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    if (world < 0 || world >= (int)d.players.size()) {
        return std::string();
    }
    return d.players[world];
}

// The world that OWNS the item at our-world location `check` (a RandomizerCheck), or -1 if
// the seed has no placement for it. When we collect a check, `check` is a location in our
// own world, so only our-world placements are considered.
int MultiShip_GetCheckOwner(int check) {
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    if (!d.ready || d.worldId < 0) {
        return -1;
    }
    for (const auto& pl : d.placements) {
        if (pl.locWorld == d.worldId && pl.loc == check) {
            return pl.ownerWorld;
        }
    }
    return -1;
}

// F-043: make the in-game world match the seed's "area access" settings (open forest, Kakariko
// gate, Door of Time, Zora's Fountain, sleeping waterfall, Jabu-Jabu, fortress carpenters).
//
// Two mechanisms, both fed from the synced F-035 settings store:
//   (1) Several effects are LIVE reads of the randomizer Context at actor-init / VanillaBehavior
//       time (King Zora, the waterfall/Jabu actor-update hooks, the forest Mido/exit-boy VBs, the
//       Door-of-Time eligibility VB). The native Context is otherwise left empty in a MultiShip
//       game (F-040 only fills placements), so we copy the server's settings into it here — the
//       VBs/actor hooks (gated for IS_MULTISHIP in hook_handlers / timesaver_hook_handlers) then
//       read the correct values, exactly like standard rando.
//   (2) The rest are ONE-TIME event/scene flags baked at save init (forest Mido flags, the
//       Door-of-Time-open flag, the Kakariko-gate inf flag, the freed carpenters). Those are NOT
//       re-derived by the live-settings copy, so we re-bake them via the shared, idempotent,
//       "open"-only Randomizer_ApplyAreaAccessWorldState (extracted from Randomizer_InitSaveFile).
//
// Called from MultiShip_ApplyPlacementsToContext, which runs both at file load (OnLoadGame) and
// when a seed arrives live mid-game (OnGameFrameUpdate). So whether the player creates-then-connects
// or connects-then-creates, the world is corrected — at load and again the moment settings arrive.
// The flag effects show up on the next scene load; the live-read effects self-heal the same way
// (the forest exit-boy has its own per-frame re-check in z_en_ko.c since the forest can't re-init).
// Main thread only (touches gRandoContext + gSaveContext).
// Copy the synced honored settings into the live Context so the in-game reads — the VBs / actor
// hooks, the area-access bake, the trial setup, and the F-044 save-init appliers — see the values
// the seed was generated under. This is the minimal set those reads touch; we deliberately do NOT
// copy the rest of the settings: F-040 keeps the Context settings-empty and drives item flow purely
// from placements, and several other registered handlers (flag-set, item queue) branch on settings,
// so leaving them at 0 preserves the established item-flow behavior. (The carpenter bake's
// Gerudo-card branch reads RSK_SHUFFLE_GERUDO_MEMBERSHIP_CARD, but only under Carpenters=Free, which
// the curated UI can't select.) The Rainbow Bridge keys feed the bridge-eligibility VB;
// RSK_GANONS_TRIALS/RSK_TRIAL_COUNT feed the trial setup; the F-044 keys feed the logic batch.
// Returns the count copied. Main thread only (touches gRandoContext).
static int MultiShip_CopyHonoredSettingsToContext() {
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    auto ctx = Rando::Context::GetInstance();
    if (!d.ready || d.worldId < 0 || ctx == nullptr) {
        return 0;
    }
    static const RandomizerSettingKey kHonoredKeys[] = {
        RSK_FOREST, RSK_KAK_GATE, RSK_DOOR_OF_TIME, RSK_ZORAS_FOUNTAIN,
        RSK_SLEEPING_WATERFALL, RSK_JABU_OPEN, RSK_GERUDO_FORTRESS,
        RSK_RAINBOW_BRIDGE, RSK_RAINBOW_BRIDGE_STONE_COUNT, RSK_RAINBOW_BRIDGE_MEDALLION_COUNT,
        RSK_GANONS_TRIALS, RSK_TRIAL_COUNT,
        // F-044 — Tab 1 section 1.1 "Logic": Selected Starting Age (the resolved age the client
        // applies), Mask Quest (mask-shop borrow logic), Skip Child Stealth (entrance remap) and
        // Skip Epona Race (logic that lets Epona's Song summon her); Full Wallets + Skip Child Zelda
        // drive one-time save-init grants/flags but are carried here too for a self-consistent
        // Context, and Starting Age ships collapsed alongside Selected.
        RSK_STARTING_AGE, RSK_SELECTED_STARTING_AGE, RSK_FULL_WALLETS, RSK_SKIP_CHILD_ZELDA,
        RSK_MASK_QUEST, RSK_SKIP_CHILD_STEALTH, RSK_SKIP_EPONA_RACE,
    };
    int copied = 0;
    for (const auto& s : d.settings) {
        for (RandomizerSettingKey k : kHonoredKeys) {
            if (s.key == (int)k) {
                ctx->GetOption(k).Set(static_cast<uint8_t>(s.value));
                ++copied;
                break;
            }
        }
    }
    return copied;
}

static void MultiShip_ApplyAreaAccessWorldState() {
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    if (!d.ready || d.worldId < 0 || Rando::Context::GetInstance() == nullptr) {
        return;
    }
    int copied = MultiShip_CopyHonoredSettingsToContext();
    // Re-bake the one-time world-state flags (forest/DoT/Kakariko/carpenters) from those settings,
    // then set up Ganon's Trials (required count + skipped-trial barriers) from RSK_TRIAL_COUNT.
    Randomizer_ApplyAreaAccessWorldState();
    Randomizer_MultiShipApplyTrials();
    // F-044: apply the Skip Child Stealth entrance remap (a live world-state read, like area access,
    // re-applied each load — NOT a one-time save grant). The other F-044 logic-batch settings either
    // bake at save init (Randomizer_MultiShipApplyStartState) or are read live from the Context by
    // their own actors/logic (Mask Quest borrow, Skip Epona summon).
    Randomizer_MultiShipApplySkipChildStealth();
    SPDLOG_INFO("[MultiShip] Applied {} area-access settings to Context + re-baked world state", copied);
}

// Populate the (otherwise empty) randomizer Context with our world's placements so the
// native check-detection + give-item-replacement pipeline suppresses the vanilla item and
// hands out the placed one. ITEM FLOW ONLY: no settings / FinalizeSettings — IsLocationShuffled
// only needs SetPlacedItem and GetFinalGIEntry resolves the entry from it. Also restores the
// COLLECTED status for already-collected checks (persisted) so the suppression VBs despawn
// them — they are never granted/reported twice. Idempotent; main thread only.
static void MultiShip_ApplyPlacementsToContext() {
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    if (!d.ready || d.worldId < 0) {
        return;
    }
    auto ctx = Rando::Context::GetInstance();
    if (ctx == nullptr) {
        return;
    }
    int placed = 0;
    for (const auto& pl : d.placements) {
        if (pl.locWorld != d.worldId) {
            continue;  // only locations in our own world are collected locally
        }
        Rando::ItemLocation* loc = ctx->GetItemLocation(static_cast<RandomizerCheck>(pl.loc));
        if (loc == nullptr) {
            continue;
        }
        loc->SetPlacedItem(static_cast<RandomizerGet>(pl.item));
        ++placed;
    }

    // Ice-trap disguise (F-040), mirroring the randomizer's Context::CreateItemOverrides: each
    // ice trap appears as a random other item with a troll name. Base rando builds the disguise
    // model pool (possibleIceTrapModels) during generation, which never runs in MultiShip — so we
    // seed it from our world's real placed items (excluding ice traps), exactly the set base rando
    // draws from. Then, per ice trap, pick a deterministic (per seed+check, so it's stable across
    // reloads) disguise model + trick name and store an override; GetFinalGIEntry swaps the draw to
    // the model and the get-item textbox uses the trick name.
    ctx->possibleIceTrapModels.clear();
    for (const auto& pl : d.placements) {
        if (pl.locWorld != d.worldId) {
            continue;
        }
        RandomizerGet rg = static_cast<RandomizerGet>(pl.item);
        // Only models with a trick-name entry are valid disguises (GetTrapName asserts otherwise).
        if (rg != RG_ICE_TRAP && rg != RG_NONE && Rando::Traps::HasTrapName(static_cast<uint16_t>(rg))) {
            ctx->possibleIceTrapModels.insert(rg);
        }
    }
    if (!ctx->possibleIceTrapModels.empty()) {
        for (const auto& pl : d.placements) {
            if (pl.locWorld != d.worldId || static_cast<RandomizerGet>(pl.item) != RG_ICE_TRAP) {
                continue;
            }
            RandomizerCheck rc = static_cast<RandomizerCheck>(pl.loc);
            // splitmix-style state keyed on (seed, check) — deterministic across reloads + non-zero.
            uint64_t state = d.seed ^ (static_cast<uint64_t>(pl.loc) * 0x9E3779B97F4A7C15ULL + 0x165667B19E3779F9ULL);
            if (state == 0) {
                state = 0x9E3779B97F4A7C15ULL;
            }
            // Pick directly from our named-only model set (NOT GetTrapTrickModel, whose reroll
            // special-cases can yield an unnamed model and trip GetTrapName's assert).
            RandomizerGet model = ShipUtils::RandomElementFromSet(ctx->possibleIceTrapModels, &state);
            Rando::ItemOverride ov(rc, model);
            ov.SetTrickName(Rando::Traps::GetTrapName(static_cast<uint16_t>(model), &state));
            ctx->iceTrapModels[rc] = model;
            ctx->overrides[rc] = ov;
        }
    }

    // Restore already-collected checks so the suppression VBs see HasObtained() and despawn
    // them. This re-fires OnRandoSetCheckStatus, but the collected set was restored first
    // (LoadMultiship runs before OnLoadGame), so the report hook treats them as already-handled.
    std::vector<int> collected = MultiShipSeed::GetCollected();
    for (int rc : collected) {
        Rando::ItemLocation* loc = ctx->GetItemLocation(static_cast<RandomizerCheck>(rc));
        if (loc != nullptr) {
            loc->SetCheckStatus(RCSHOW_COLLECTED);
        }
    }
    SPDLOG_INFO("[MultiShip] Applied {} world-{} placements to Context ({} already collected)", placed,
                d.worldId, static_cast<int>(collected.size()));

    // F-043: now that the Context exists, also apply the synced area-access settings + re-bake the
    // matching world-state flags so the game world matches the seed (open forest, fountain, etc.).
    MultiShip_ApplyAreaAccessWorldState();
}

// F-041: grant this world's starting dungeon reward, placed by the generator at RC_LINKS_POCKET
// (a medallion or spiritual stone). Granted ONCE when the MultiShip save is created/initialized —
// NOT via the F-040 collect-check flow — and idempotent across reloads via the persisted collected
// set. Main thread only. Runs even while disconnected: a starting item is local.
//
// `persist`: when nonzero, persists with a full base save itself (the OnLoadGame fallback path,
// where nothing else saves right after). When zero, the CALLER persists — used at file creation
// (z_sram.c Sram_InitSave), which runs its own Save_SaveFile immediately after, so the reward must
// land in gSaveContext BEFORE that creation save so the file-select slot metadata shows it from the
// start (InitMeta reads gSaveContext.inventory.questItems). A self-save there would be redundant.
extern "C" void MultiShip_GrantStartingReward(int persist) {
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    if (!d.ready || d.worldId < 0) {
        return;
    }
    // Once-only guard: the collected set is persisted in the multiship save section and restored
    // (by LoadMultiship) before this runs, so a reload never re-grants or duplicates the reward.
    if (MultiShipSeed::IsCollected(static_cast<int>(RC_LINKS_POCKET))) {
        return;
    }
    int rewardRg = RG_NONE;
    for (const auto& pl : d.placements) {
        if (pl.locWorld == d.worldId && pl.loc == static_cast<int>(RC_LINKS_POCKET)) {
            rewardRg = pl.item;
            break;
        }
    }
    if (rewardRg <= RG_NONE) {
        return;  // no Link's Pocket reward in this seed (e.g. an older seed) — nothing to grant.
    }
    Randomizer_MultiShipGiveStartingReward(rewardRg);
    // Record it as collected so it isn't granted again. The OnRandoSetCheckStatus hook ignores
    // RC_LINKS_POCKET, so this is the only marker.
    MultiShipSeed::MarkCollected(static_cast<int>(RC_LINKS_POCKET));
    if (persist) {
        // Persist with a FULL base save, not just the multiship section: the granted reward lives in
        // the base inventory/quest section while the once-only marker lives in the multiship section,
        // and the two MUST reach disk together. A bare SaveSection(multiship) would write the marker
        // while the base section on disk stayed stale (no reward) — and a later foreign-check
        // SaveSection(multiship) could lock that in, losing the reward on reload. A base save writes
        // every saveWithBase section (base + multiship) from one gSaveContext snapshot, keeping them
        // consistent. (At file creation persist is 0: the caller's Save_SaveFile does this.)
        SaveManager::Instance->SaveFile(gSaveContext.fileNum);
    }
    SPDLOG_INFO("[MultiShip] Granted starting dungeon reward (RG {}) at Link's Pocket for world {}", rewardRg,
                d.worldId);
}

// F-044: apply the one-time starting STATE (adult age + Master Sword, full wallets, Skip Child Zelda
// letter/flags, completed masks) from the synced "Logic" settings — the MultiShip analog of the
// per-setting blocks in Randomizer_InitSaveFile, which does not run for a QUEST_MULTISHIP file.
// Granted ONCE per save (the persisted start-state marker), idempotent across reloads, exactly like
// the F-041 reward. We copy the synced settings into the Context first so the savefile reader sees
// them at BOTH file creation (z_sram.c, the Context isn't populated yet there) and load. Local
// grants, so this runs even while disconnected. `persist`: nonzero -> persist with a full base save
// (the OnLoadGame fallback); zero -> the caller's creation Save_SaveFile persists it. Main thread.
extern "C" void MultiShip_ApplyStartState(int persist) {
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    if (!d.ready || d.worldId < 0) {
        return;
    }
    // Once-only guard: the marker is persisted in the multiship section and restored before this
    // runs, so a reload never re-grants the Master Sword / rupees / letter (the flag writes are
    // idempotent anyway, but the item grants are not).
    if (MultiShipSeed::IsStartStateApplied()) {
        return;
    }
    if (Rando::Context::GetInstance() == nullptr) {
        return;
    }
    // Ensure the synced settings are in the Context so Randomizer_GetSettingValue reads them — at
    // file creation nothing else has populated it yet (placements/settings land at OnLoadGame).
    MultiShip_CopyHonoredSettingsToContext();
    Randomizer_MultiShipApplyStartState();
    MultiShipSeed::SetStartStateApplied(true);
    if (persist) {
        // Full base save: the grants live in the base inventory/equip/scene-flag sections while the
        // marker lives in the multiship section, and they must reach disk together (same reasoning
        // as the F-041 reward). At file creation persist is 0 — the caller's Save_SaveFile does it.
        SaveManager::Instance->SaveFile(gSaveContext.fileNum);
    }
    SPDLOG_INFO("[MultiShip] Applied starting state for world {}", d.worldId);
}

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

// F-041: the name of this world's starting dungeon reward (from the Link's Pocket placement), shown
// on the file-select pre-creation screen BEFORE the save is loaded so the player sees which reward
// they start with. Empty until a seed is received. Static buffer (the menu draws it once per frame
// on the main thread, so this is safe).
extern "C" const char* MultiShip_StartingRewardName(void) {
    static std::string name;
    name.clear();
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    if (!d.ready || d.worldId < 0) {
        return name.c_str();
    }
    for (const auto& pl : d.placements) {
        if (pl.locWorld == d.worldId && pl.loc == static_cast<int>(RC_LINKS_POCKET)) {
            if (pl.item > RG_NONE) {
                name = Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(pl.item)).GetName().english;
            }
            break;
        }
    }
    return name.c_str();
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
                // F-040: feed the placements into the rando Context, but only on the main
                // thread (touching Context / firing hooks from this network thread is unsafe).
                // If a MultiShip file is already loaded (reconnect mid-game), the frame hook
                // applies it; otherwise the upcoming file load's OnLoadGame applies it.
                gNeedsContextApply.store(true);
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
        if (gSaveContext.ship.quest.id != QUEST_MULTISHIP)
            return;
        // F-040: feed our world's placements into the rando Context now (main thread, save
        // loaded). LoadMultiship already restored the seed + collected set for an existing
        // file; for a just-created file the seed is in memory from the live receive. Granting
        // own items locally works even while disconnected, so this is NOT gated on isConnected.
        gNeedsContextApply.store(false);
        MultiShip_ApplyPlacementsToContext();
        // F-041: grant the Link's Pocket starting dungeon reward once (guarded by the collected
        // set, which ApplyPlacementsToContext just restored for an existing file). Reward is local,
        // so this runs even while disconnected. New files are already granted at creation
        // (z_sram.c), so this is a fallback (e.g. legacy files); persist=1 so it saves itself.
        MultiShip_GrantStartingReward(1);
        // F-044: apply the one-time starting state (adult age + Master Sword, full wallets, Skip
        // Child Zelda letter/flags, completed masks). Like the reward it's also granted at creation
        // (z_sram.c); persist=1 here is the fallback (the marker makes it a no-op once applied).
        MultiShip_ApplyStartState(1);
        if (isConnected) {
            SendOnLoadGame();
        }
    });

    // F-040: apply placements to the Context when a seed arrives LIVE while a MultiShip file
    // is already loaded (reconnect mid-game) — the network thread can't touch the Context, so
    // it sets a flag that we consume here on the main thread.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>([&]() {
        if (gSaveContext.ship.quest.id != QUEST_MULTISHIP || !GameInteractor::IsSaveLoaded()) {
            return;
        }
        if (gNeedsContextApply.exchange(false)) {
            MultiShip_ApplyPlacementsToContext();
        }
    });

    // F-040: a collected check (RandomizerCheck) reaches RCSHOW_COLLECTED/SAVED — for our own
    // items the native pipeline set it on receipt; for foreign items the hook_handlers intercept
    // set it after suppressing the give. Record it (idempotency, persisted in the multiship
    // section) and, for a FOREIGN item, report the collection to the server exactly once so it
    // can route the item to its owner. Not gated on isConnected for recording (own items can be
    // collected offline); the report itself requires a connection.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnRandoSetCheckStatus>(
        [&](RandomizerCheck rc, RandomizerCheckStatus status) {
            if (gSaveContext.ship.quest.id != QUEST_MULTISHIP) {
                return;
            }
            if (status != RCSHOW_COLLECTED && status != RCSHOW_SAVED) {
                return;
            }
            // F-041: Link's Pocket is a starting item granted once at save init
            // (MultiShip_GrantStartingReward), never collected in the world — exclude it from the
            // cross-world collect flow so it isn't reported or double-processed.
            if (rc == RC_LINKS_POCKET) {
                return;
            }
            const int check = static_cast<int>(rc);
            const int owner = MultiShip_GetCheckOwner(check);
            if (owner < 0) {
                return;  // not one of our seed's placements — ignore vanilla collections
            }
            if (MultiShipSeed::IsCollected(check)) {
                return;  // already handled — grant/report exactly once across reloads/reconnects
            }
            if (owner != MultiShip_GetMyWorld() && isConnected) {
                // Foreign item: report the collection so the server delivers it to its owner.
                nlohmann::json payload;
                payload["id"] = ShipUtils::Random(0, UINT32_MAX);
                payload["type"] = "hook";
                payload["hook"]["type"] = "OnCheckCollected";
                payload["hook"]["check"] = check;
                payload["hook"]["world"] = MultiShip_GetMyWorld();
                SPDLOG_INFO("[MultiShip] Reporting collected foreign check {} (owner world {}, my world {})",
                            check, owner, MultiShip_GetMyWorld());
                SendJsonToRemote(payload);
            }
            MultiShipSeed::MarkCollected(check);
            SaveManager::Instance->SaveSection(gSaveContext.fileNum, SECTION_ID_MULTISHIP, true);
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
        if (gSaveContext.ship.quest.id != QUEST_MULTISHIP) {
            return;
        }
        // Some rando "items" are vanilla equipment upgrades for us (strength → UPG_STRENGTH): the
        // give only sets a RAND_INF flag, so translate it to the vanilla upgrade here so it shows
        // in the equipment subscreen + takes effect. Runs for BOTH locally-collected and
        // server-delivered items, and even while disconnected (own items work offline).
        Randomizer_MultiShipApplyVanillaUpgrade(static_cast<int>(itemEntry.modIndex),
                                                static_cast<int>(itemEntry.getItemId));

        // The remainder is the F-030 crash-safe delivery pop, which only applies to items the
        // server pushed to us — so it needs an active connection + a queued delivery.
        if (!isConnected) {
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
