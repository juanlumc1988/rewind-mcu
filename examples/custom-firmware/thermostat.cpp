// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "thermostat.h"

#include "rewind/hal.h"

namespace thermostat {
namespace {

State g_state;

void button_isr() {
    (void)rwd::mmio_read32(kButtonReg);        // acknowledge
    g_state.setpoint += 5;                     // half a degree warmer
    ++g_state.presses;
}

} // namespace

void reset() {
    g_state.setpoint = kDefaultSetpoint;
    g_state.heater   = 0;
    g_state.switches = 0;
    g_state.hottest  = 0;
    g_state.presses  = 0;
}

void install() {
    rwd::hal_set_isr(kIrqButton, &button_isr);
    // The whole of the firmware's state, so a recording can start mid-run.
    rwd::hal_state_region(0, &g_state, sizeof(g_state));
}

void run(u32 iterations) {
    for (u32 i = 0; i < iterations; ++i) {
        const u32 t = rwd::mmio_read32(kTempReg);
        if (t > g_state.hottest) {
            g_state.hottest = t;
        }

        if (g_state.heater == 0 && t + kHysteresis < g_state.setpoint) {
            g_state.heater = 1;
            ++g_state.switches;
            rwd::mmio_write32(kHeaterReg, 1);
        } else if (g_state.heater != 0 && t > g_state.setpoint + kHysteresis) {
            g_state.heater = 0;
            ++g_state.switches;
            rwd::mmio_write32(kHeaterReg, 0);
        }
    }
}

const State& state() {
    return g_state;
}

void restore(const State& s) {
    g_state = s;
}

} // namespace thermostat
