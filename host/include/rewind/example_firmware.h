// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The example firmware (firmware/rx_pump.h) as a Firmware: all four variants,
// and the build ids that name them. This is also the smallest worked case of
// what a firmware has to provide -- see firmware.h.

#ifndef REWIND_EXAMPLE_FIRMWARE_H
#define REWIND_EXAMPLE_FIRMWARE_H

#include "fw/rx_pump.h"
#include "rewind/firmware.h"

namespace rwhost {

class ExampleFirmware : public Firmware {
public:
    explicit ExampleFirmware(fw::Variant variant) : variant_(variant) {}

    fw::Variant variant() const { return variant_; }

    const char* name() const override;
    void reset() override;
    void install() override;
    void run(u32 iterations) override;
    std::size_t state_size() const override;
    void save_state(u8* out) const override;
    void restore_state(const u8* in) override;
    u64  state_hash() const override;
    void describe_state(const u8* state, std::vector<StateField>* out) const override;

    // The state, as the struct it is.
    static bool decode(const u8* state, std::size_t len, fw::State* out);
    static void describe(const fw::State& state, std::vector<StateField>* out);

private:
    fw::Variant variant_;
};

// The firmware a build id names, or null if it is not one of the example's.
std::unique_ptr<Firmware> make_example_firmware(u64 build_id);

} // namespace rwhost

#endif // REWIND_EXAMPLE_FIRMWARE_H
