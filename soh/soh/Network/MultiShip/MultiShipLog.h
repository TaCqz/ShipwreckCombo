// Dedicated MultiShip debug logging (C++).
//
// MULTISHIP_LOG(fmt, ...) formats with fmt and appends one line to a dedicated "multiship.log"
// file in the app's "logs" folder (the same place as SoH's own log) plus the normal SoH log under
// a "[MultiShip]" tag — so MultiShip behaviour is easy to follow without sifting the whole console.
// It is:
//   - COMPILE-gated: when ENABLE_MULTISHIP is not defined the macro is a no-op, so it costs nothing
//     and can never affect a vanilla / standard-rando build.
//   - RUNTIME-gated: the backing MultiShip_Log() early-outs unless the current save is a MultiShip
//     game (IS_MULTISHIP), so even an ENABLE_MULTISHIP build stays silent during a vanilla/rando game.
//
// MultiShip_Log is defined (extern "C") in Network/MultiShip/MultiShip.cpp. This header is C++-only
// (it is the convenience path for the C++ randomizer hooks); a C caller would declare MultiShip_Log
// itself.
#pragma once

#ifdef ENABLE_MULTISHIP
#include <spdlog/fmt/fmt.h>
extern "C" void MultiShip_Log(const char* msg);
#define MULTISHIP_LOG(...) MultiShip_Log(::fmt::format(__VA_ARGS__).c_str())
#else
#define MULTISHIP_LOG(...) ((void)0)
#endif
