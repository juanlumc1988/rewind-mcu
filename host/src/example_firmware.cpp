// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/example_firmware.h"

#include <cstdio>
#include <cstring>

#include "rewind/session.h"

namespace rwhost {

// The saved image is the struct as it sits in memory. That is only a faithful
// image if it has no padding to differ between equal states, which is true of
// this layout and checked here rather than assumed.
static_assert(sizeof(fw::State) == fw::kRingSize + 9 * sizeof(rwd::u32),
              "fw::State has padding; save_state would copy it");

const char* ExampleFirmware::name() const {
    switch (variant_) {
        case fw::kFixed:     return "fixed";
        case fw::kUnguarded: return "unguarded";
        case fw::kDma:       return "dma";
        default:             return "buggy";
    }
}

void ExampleFirmware::reset() {
    fw::rx_pump_reset();
}

void ExampleFirmware::install() {
    fw::rx_pump_install();
}

void ExampleFirmware::run(u32 iterations) {
    fw::rx_pump_run(variant_, iterations);
}

std::size_t ExampleFirmware::state_size() const {
    return sizeof(fw::State);
}

void ExampleFirmware::save_state(u8* out) const {
    std::memcpy(out, &fw::rx_pump_state(), sizeof(fw::State));
}

void ExampleFirmware::restore_state(const u8* in) {
    fw::State s = fw::State();
    std::memcpy(&s, in, sizeof(fw::State));
    fw::rx_pump_restore(s);
}

// Not the default: the example's hash is field by field and leaves out the DMA
// counters when they are zero, so that the hash of a run that did not use them
// stayed what it was before they existed.
u64 ExampleFirmware::state_hash() const {
    return fw::rx_pump_state_hash();
}

bool ExampleFirmware::decode(const u8* state, std::size_t len, fw::State* out) {
    if (state == 0 || out == 0 || len != sizeof(fw::State)) {
        return false;
    }
    std::memcpy(out, state, sizeof(fw::State));
    return true;
}

void ExampleFirmware::describe(const fw::State& s, std::vector<StateField>* out) {
    out->clear();
    out->push_back(StateField("head", s.head));
    out->push_back(StateField("tail", s.tail));
    out->push_back(StateField("pending", s.pending));
    out->push_back(StateField("consumed", s.consumed));
    out->push_back(StateField("checksum", s.checksum));
    out->push_back(StateField("last_stamp", s.last_stamp));
    out->push_back(StateField("overflows", s.overflows));
    out->push_back(StateField("dma_tail", s.dma_tail));
    out->push_back(StateField("dma_lost", s.dma_lost));
    for (rwd::u32 i = 0; i < fw::kRingSize; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "ring[%u]", (unsigned)i);
        out->push_back(StateField(name, s.ring[i]));
    }
}

void ExampleFirmware::describe_state(const u8* state, std::vector<StateField>* out) const {
    fw::State s = fw::State();
    if (!decode(state, sizeof(fw::State), &s)) {
        out->clear();
        return;
    }
    describe(s, out);
}

std::unique_ptr<Firmware> make_example_firmware(u64 build_id) {
    fw::Variant v;
    if (!variant_for_build_id(build_id, &v)) {
        return std::unique_ptr<Firmware>();
    }
    return std::unique_ptr<Firmware>(new ExampleFirmware(v));
}

} // namespace rwhost
