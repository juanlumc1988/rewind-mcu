// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// What the host tools need to know about the thermostat: how to start it, run it,
// and save and restore its state. This is the whole of the adapter -- see
// rewind/firmware.h for what each piece is for.

#ifndef THERMOSTAT_FIRMWARE_H
#define THERMOSTAT_FIRMWARE_H

#include <cstring>

#include "rewind/firmware.h"
#include "thermostat.h"

namespace thermostat {

// What the trace header's build id says this firmware is. On a real device it is
// something the build generates (a hash of the binary); any 64-bit value will do
// as long as the device and the host agree.
const rwd::u64 kBuildId = 0x7E57A7000000A001ULL;

class Firmware : public rwhost::Firmware {
public:
    const char* name() const override { return "thermostat"; }

    void reset() override { thermostat::reset(); }
    void install() override { thermostat::install(); }
    void run(rwd::u32 iterations) override { thermostat::run(iterations); }

    std::size_t state_size() const override { return sizeof(State); }
    void save_state(rwd::u8* out) const override {
        std::memcpy(out, &thermostat::state(), sizeof(State));
    }
    void restore_state(const rwd::u8* in) override {
        State s;
        std::memcpy(&s, in, sizeof(State));
        thermostat::restore(s);
    }

    // So the viewer and `diff` show variables, not just a hash.
    void describe_state(const rwd::u8* bytes, std::vector<rwhost::StateField>* out) const override {
        State s;
        std::memcpy(&s, bytes, sizeof(State));
        out->clear();
        out->push_back(rwhost::StateField("setpoint", s.setpoint));
        out->push_back(rwhost::StateField("heater", s.heater));
        out->push_back(rwhost::StateField("switches", s.switches));
        out->push_back(rwhost::StateField("hottest", s.hottest));
        out->push_back(rwhost::StateField("presses", s.presses));
    }
};

// Which firmware replays a trace: this one, for its build id.
inline std::unique_ptr<rwhost::Firmware> factory(rwd::u64 build_id) {
    if (build_id != kBuildId) {
        return std::unique_ptr<rwhost::Firmware>();
    }
    return std::unique_ptr<rwhost::Firmware>(new Firmware());
}

} // namespace thermostat

#endif // THERMOSTAT_FIRMWARE_H
