#ifndef NETWORK_MULTISHIP_SEED_H
#define NETWORK_MULTISHIP_SEED_H
#ifdef ENABLE_MULTISHIP

// MultiShipSeed — the client-side store for the multiworld seed handed out by the
// MultiShip server (F-035 Part B).
//
// The server sends the FULL v3 SeedData over the wire, byte-identical to the
// .multiship file (see the sibling MultiShip repo: src/rando/SeedFile.{h,cpp} +
// docs/multiship-wire-v3.md). This module base64-decodes that blob and deserializes
// the v3 byte layout into a structured, mutex-guarded store so the client can:
//   - validate the player's chosen world name before claiming (from the seed-info
//     player list), greying out 'Start Multiworld Save' otherwise,
//   - persist the placements / owners / settings into the QUEST_MULTISHIP save (via
//     SaveManager's "multiship" section) so they survive reloads, and
//   - later (F-040) show the correct item + "belongs to <player>" label at each check.
//
// SCOPE (Part B): receive + persist only. No live cross-world delivery, no game-
// behavior setting application yet.

// C-callable readiness gate. Declared in OTRGlobals.h for the C file-creation code
// (z_sram.c / z_file_choose.c); defined in MultiShipSeed.cpp. Mirrors
// Randomizer_IsSeedGenerated(): true once a full v3 seed has been received or loaded.

#ifdef __cplusplus
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace MultiShipSeed {

// One routed placement, exactly as the v3 contract carries it. `loc`/`item` are the
// raw RandomizerCheck / RandomizerGet enum values (kept as ints so this header pulls
// in no randomizer enum dependency); `ownerWorld` is the world that receives the item.
struct Placement {
    int loc = 0;
    int locWorld = 0;
    int item = 0;
    int ownerWorld = 0;
};

// One curated setting: RandomizerSettingKey enum value -> base SoH option index.
struct Setting {
    int key = 0;
    int value = 0;
};

// The deserialized v3 seed + this client's place in it. A copy is returned by
// Snapshot() (for saving) and pushed by LoadFromSnapshot() (on load).
struct Data {
    bool ready = false;        // a full v3 seed has been received or loaded
    int worldId = -1;          // this client's assigned world (index into players)
    uint64_t seed = 0;         // numeric RNG seed
    std::string seedId;        // human-readable id
    std::vector<std::string> players;
    std::vector<Placement> placements;
    std::vector<Setting> settings;
};

// --- Seed-info (non-locking) — learned from the server's multiworld_seed_info push.
// Used only to validate the chosen name before claiming a world.
void SetKnownPlayers(const std::string& seedId, const std::vector<std::string>& players);
std::vector<std::string> GetKnownPlayers();
// True if `name` is one of the seed's world names (case-sensitive exact match).
bool IsNameValid(const std::string& name);

// --- Full seed (granted) -----------------------------------------------------------
// Decode base64 -> parse the v3 byte layout -> populate the store + mark ready. Returns
// false (and fills `err`) on malformed input; the store is left unchanged on failure.
// `worldId` is the server-assigned world index from the multiworld_seed envelope.
bool DeserializeV3FromBase64(const std::string& base64, int worldId, std::string& err);

// True once a full v3 seed is present (received this session or loaded from a save).
bool IsReady();

// Thread-safe copy of the full store (for SaveManager serialization / inspection).
Data Snapshot();
// Replace the store from a loaded save (SaveManager "multiship" section) and mark ready.
void LoadFromSnapshot(const Data& data);
// Forget everything (e.g. leaving a multiworld file). Leaves knownPlayers intact only
// if still connected — callers decide; this clears the full-seed half.
void Clear();

// --- Request lifecycle status (for the 'Start Multiworld Save' menu) ---------------
// A short human-readable line describing the last request outcome ("Requesting…",
// "Seed received (world 1: Player 1)", "Denied: unknown_name", …). Thread-safe.
void SetStatus(const std::string& status);
std::string GetStatus();

} // namespace MultiShipSeed
#endif // __cplusplus

#endif // ENABLE_MULTISHIP
#endif // NETWORK_MULTISHIP_SEED_H
