#ifndef NETWORK_MULTISHIP_HINTS_H
#define NETWORK_MULTISHIP_HINTS_H
#ifdef ENABLE_MULTISHIP
#ifdef __cplusplus

// F-051 — MultiShip static hints.
//
// SoH's "Static Hints" (the fixed hint-givers: ToT altar, Ganondorf, Sheik, Dampé's diary,
// Mido, Saria, Biggoron, …) are rendered by builders in Messages/StaticHints.cpp that read
// RAND_GET_HINT(rh) from the randomizer Context. In standard rando those hint objects are
// created by the engine's per-seed generator (3drando/hints.cpp CreateStaticHints), which
// never runs for MultiShip. This module rebuilds the in-scope static-hint objects from the
// synced multiworld placement store instead, CROSS-WORLD aware (an item routed to the other
// player is labelled "<Player>'s <item>", matching the F-037 foreign-item style), and stores
// them in the Context so the unchanged StaticHints.cpp builders render them.
//
// Called from MultiShip_ApplyPlacementsToContext (MultiShip.cpp) after our world's placements
// and the honored settings are in the Context. Idempotent (Context::AddHint overwrites).

#include "soh/Enhancements/custom-message/CustomMessageManager.h"  // CustomMessage
#include "soh/Enhancements/randomizer/randomizerTypes.h"           // RandomizerCheck

// Populate the Context's static-hint objects for the 17 in-scope hint-givers from the store.
void MultiShip_BuildStaticHints();

// Owner-aware display name of the item physically placed at our-world check `check` (used by
// "forward" hints — the ones that reveal the item at a fixed reward check). A foreign item is
// labelled "<Player>'s <item>" (F-037 style). Falls back to the check's vanilla item when the
// check isn't in the store. Shared with the Gold-Skulltula reward hint in StaticHints.cpp.
CustomMessage MultiShip_HintItemName(RandomizerCheck check);

#endif // __cplusplus
#endif // ENABLE_MULTISHIP
#endif // NETWORK_MULTISHIP_HINTS_H
