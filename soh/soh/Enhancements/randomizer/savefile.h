#ifndef RANDOSAVEFILE_H
#define RANDOSAVEFILE_H

#ifdef __cplusplus
extern "C" {
#endif

void Randomizer_InitSaveFile();

#ifdef ENABLE_MULTISHIP
// F-041: grant the Link's Pocket starting dungeon reward (RandomizerGet `rgItem`) in a MultiShip
// save. Caller owns the once-per-save guard; see soh/Network/MultiShip/MultiShip.cpp.
void Randomizer_MultiShipGiveStartingReward(int rgItem);
#endif

#ifdef __cplusplus
}
#endif

#endif
