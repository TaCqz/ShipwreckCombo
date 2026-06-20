#ifndef RANDOSAVEFILE_H
#define RANDOSAVEFILE_H

#ifdef __cplusplus
extern "C" {
#endif

void Randomizer_InitSaveFile();

// Bakes the one-time world-state event/scene flags for the "area access" / open-world
// settings (open forest, Door of Time, Kakariko gate, Gerudo fortress). Called from
// Randomizer_InitSaveFile at creation, and re-runnable on its own: MultiShip re-runs
// it once the authoritative server settings are known, since those baked flags are
// otherwise computed only at file creation and the live-context reapply doesn't touch
// them. Every write is idempotent and "open"-only, so it is safe to call repeatedly
// and after the player is already in-game.
void Randomizer_ApplyAreaAccessWorldState();

#ifdef ENABLE_MULTISHIP
// Sets up a brand-new MultiShip file as a default-settings randomizer world
// (full local generation for a guaranteed-valid Context, then Randomizer_InitSaveFile).
// The server overrides placements on connect; the local fill is otherwise unused.
void Randomizer_InitMultiShipSaveFile();
#endif

#ifdef __cplusplus
}
#endif

#endif
