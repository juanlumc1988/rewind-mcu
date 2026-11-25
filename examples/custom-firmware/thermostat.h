// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// A firmware that is not the one in firmware/: a thermostat. It reads a
// temperature sensor, switches a heater with a little hysteresis, and lets a
// button nudge the set point from an interrupt.
//
// This is the part that runs on the device, written the way device code is
// written here -- C++98, no heap, every register access through the shim -- and
// it is the same source on the part and on the workstation. Nothing in it knows
// about replay except that its state is declared (hal_state_region) so a
// recording can begin mid-run.

#ifndef THERMOSTAT_H
#define THERMOSTAT_H

#include "rewind/rw_types.h"

namespace thermostat {

using rwd::u32;

// Registers, as the (invented) part defines them.
const u32 kTempReg   = 0x50000000u;   // RO  temperature, tenths of a degree
const u32 kHeaterReg = 0x50000004u;   // WO  1 = heater on
const u32 kButtonReg = 0x50000008u;   // RO  reading acknowledges the button

const u32 kIrqButton = 4u;

struct State {
    u32 setpoint;    // tenths of a degree
    u32 heater;      // is it on
    u32 switches;    // times the heater has changed state
    u32 hottest;     // highest reading seen
    u32 presses;     // button presses handled
};

const u32 kDefaultSetpoint = 200;     // 20.0 degrees
const u32 kHysteresis      = 4;       // 0.4 degrees either side

void reset();
void install();                       // handlers and the state region; after hal_reset()
void run(u32 iterations);             // the main loop, resumable
const State& state();
void restore(const State& s);

} // namespace thermostat

#endif // THERMOSTAT_H
