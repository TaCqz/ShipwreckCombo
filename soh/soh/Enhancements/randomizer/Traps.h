#pragma once

#ifndef __cplusplus
#error This header should not be used in C files
#endif

#include "soh/Enhancements/custom-message/CustomMessageManager.h"
#include "soh/Enhancements/randomizer/randomizerTypes.h"
#include "soh/Enhancements/custom-message/text.h"
#include "libultraship/libultra/types.h"

namespace Rando {
namespace Traps {
Text GetTrapName(uint16_t id, uint64_t* state = nullptr);
// True if `id` (a RandomizerGet / GetItemID) has trick names defined, i.e. it's a valid ice-trap
// disguise model. GetTrapName asserts on ids without names, so a caller building its own disguise
// model pool (e.g. MultiShip) must filter with this first. Initializes the trick-name table.
bool HasTrapName(uint16_t id);
RandomizerGet GetTrapTrickModel(uint64_t* state = nullptr);
bool ShouldJunkItemBeTrap();
void BuildIceTrapMessage(CustomMessage& msg, GetItemEntry getItemEntry);
} // namespace Traps
} // namespace Rando