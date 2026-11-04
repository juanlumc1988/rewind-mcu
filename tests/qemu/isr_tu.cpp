// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The macro under test, in a translation unit of its own so that run.sh can
// build it with the target side's strict flags (-std=c++98 -pedantic-errors):
// RWD_CORTEX_M_ISR is public API for firmware that is held to those rules.

#include "rewind/cortex_m.h"

RWD_CORTEX_M_ISR(IRQ0_Handler, 1)
