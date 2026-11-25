// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Putting events into words, for the command line and the viewer alike.

#ifndef REWIND_DESCRIBE_H
#define REWIND_DESCRIBE_H

#include <string>

#include "rewind/trace.h"

namespace rwhost {

using rwd::u8;
using rwd::u32;

// The example firmware's register map, by address; "" for anything else.
const char* register_name(u32 addr);

// "read", "write", "irq", "site", "dma", "stop"; "?" for a type this does not know.
const char* event_kind(u8 type);

// One line, without the cycle count: "read 0x40000000 SYSTICK.CNT -> 0x00000006".
std::string describe_event(const rwd::Event& e);

} // namespace rwhost

#endif // REWIND_DESCRIBE_H
