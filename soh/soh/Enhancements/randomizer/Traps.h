#pragma once

#ifndef __cplusplus
#error This header should not be used in C files
#endif

#include <optional>
#include <string>

#include "soh/Enhancements/custom-message/CustomMessageManager.h"
#include "soh/Enhancements/randomizer/randomizerTypes.h"
#include "soh/Enhancements/custom-message/text.h"
#include "libultraship/libultra/types.h"

namespace Rando {
namespace Traps {
Text GetTrapName(uint16_t id, uint64_t* state = nullptr);
RandomizerGet GetTrapTrickModel(uint64_t* state = nullptr);
bool ShouldJunkItemBeTrap();
void BuildIceTrapMessage(CustomMessage& msg, GetItemEntry getItemEntry);

// One-shot overrides for the NEXT ice trap that is given outside of a normal
// placement (e.g. one sent by the MultiShip server). Each is consumed (cleared)
// the first time it is read; when unset, the normal random fallback is used.
// Thread-safe: the setter runs on the network thread, the text getter on the
// main thread.
void SetNextIceTrapModel(RandomizerGet model);
std::optional<RandomizerGet> TakeNextIceTrapModel();
void SetNextIceTrapText(const std::string& text);
std::optional<std::string> TakeNextIceTrapText();
} // namespace Traps
} // namespace Rando