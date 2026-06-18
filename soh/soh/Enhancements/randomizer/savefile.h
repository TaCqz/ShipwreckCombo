#ifndef RANDOSAVEFILE_H
#define RANDOSAVEFILE_H

#ifdef __cplusplus
extern "C" {
#endif

void Randomizer_InitSaveFile();

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
