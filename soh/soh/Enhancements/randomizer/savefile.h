#ifndef RANDOSAVEFILE_H
#define RANDOSAVEFILE_H

#ifdef __cplusplus
extern "C" {
#endif

void Randomizer_InitSaveFile();

// F-043: bakes the one-time world-state event/scene flags for the "area access" / open-world
// settings (open forest Mido block, Door of Time, Kakariko gate, Gerudo fortress carpenters).
// Called from Randomizer_InitSaveFile at creation, and re-runnable on its own: MultiShip re-runs
// it once the authoritative server settings are known (the create-before-connect race), since
// those baked flags are otherwise computed only at file creation and the live-Context reapply
// doesn't touch them. Every write is idempotent and "open"-only, so it is safe to call
// repeatedly and after the player is already in-game. Reads the live Rando::Context settings via
// Randomizer_GetSettingValue, so the caller must have those settings applied first.
void Randomizer_ApplyAreaAccessWorldState();

#ifdef ENABLE_MULTISHIP
// F-041: grant the Link's Pocket starting dungeon reward (RandomizerGet `rgItem`) in a MultiShip
// save. Caller owns the once-per-save guard; see soh/Network/MultiShip/MultiShip.cpp.
void Randomizer_MultiShipGiveStartingReward(int rgItem);

// F-043 (b): set up Ganon's Trials in a MultiShip save from the synced RSK_TRIAL_COUNT — marks
// that many trials required and bakes the COMPLETED flag for the skipped ones (dispelling their
// barriers). Caller applies the synced settings to the Context first. Idempotent.
void Randomizer_MultiShipApplyTrials();
#endif

#ifdef __cplusplus
}
#endif

#endif
