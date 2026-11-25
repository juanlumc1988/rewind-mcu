// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// A firmware that is not the example, and a device to run it against: a
// button, an LED and a free-running counter. Shared by the tests that check
// the tools do not depend on the example firmware (test_firmware.cpp, and the
// viewer's tests). Header only, so each test binary gets its own copy of the
// static storage.

#ifndef REWIND_TESTS_BLINKER_H
#define REWIND_TESTS_BLINKER_H

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "rewind/firmware.h"
#include "rewind/hal.h"
#include "rewind/session.h"
#include "rewind/timeline.h"

namespace blinker {

using rwd::u8;
using rwd::u32;
using rwd::u64;

inline constexpr u64 kBlinkerBuild = 0xB11A4E7000000001ULL;

// -- a device: a free-running counter, a button, an LED --

inline constexpr u32 kCnt = 0x50000000u;   // RO
inline constexpr u32 kBtn = 0x50000004u;   // RO: 0 or 1
inline constexpr u32 kLed = 0x50000008u;   // WO
inline constexpr u32 kIrqButton = 3u;

class Device {
public:
    explicit Device(u32 seed) : cycles_(0), button_(0), led_(0), irq_(false), rng_(seed ? seed : 1u),
                                next_toggle_(60) {
        ops_.read32   = &Device::s_read;
        ops_.write32  = &Device::s_write;
        ops_.now      = &Device::s_now;
        ops_.poll_irq = &Device::s_poll;
        ops_.ctx      = this;
    }
    const rwd::HalOps* ops() const { return &ops_; }
    u32 led() const { return led_; }

private:
    u32 next() {
        rng_ = rng_ * 1664525u + 1013904223u;
        return rng_ >> 8;
    }
    void advance(u32 dt) {
        cycles_ += dt;
        while (cycles_ >= next_toggle_) {
            button_ ^= 1u;
            irq_ = true;
            next_toggle_ += 40 + next() % 90;
        }
    }
    static u32 s_read(void* c, u32 addr) {
        Device* d = static_cast<Device*>(c);
        d->advance(3);
        return addr == kCnt ? (u32)d->cycles_ : addr == kBtn ? d->button_ : 0u;
    }
    static void s_write(void* c, u32 addr, u32 v) {
        Device* d = static_cast<Device*>(c);
        d->advance(3);
        if (addr == kLed) {
            d->led_ = v;
        }
    }
    static u64 s_now(void* c) { return static_cast<Device*>(c)->cycles_; }
    static u32 s_poll(void* c) {
        Device* d = static_cast<Device*>(c);
        if (d->irq_) {
            d->irq_ = false;
            return kIrqButton;
        }
        return rwd::kNoIrq;
    }

    rwd::HalOps ops_;
    u64  cycles_;
    u32  button_;
    u32  led_;
    bool irq_;
    u32  rng_;
    u64  next_toggle_;
};

// -- the firmware --
//
// The state is static storage, as bare-metal firmware keeps it; the class below
// only gives the tools a way to get at it.

struct BlinkState {
    u32 seen;          // the button as the main loop last saw it
    u32 presses;
    u32 irqs;          // edges the handler saw
    u32 history[4];    // cycle counter at the last four presses
};
inline BlinkState g_state;
inline bool       g_inverted = false;   // the "bug" a test can build in

inline void button_isr() {
    ++g_state.irqs;
    (void)rwd::mmio_read32(kBtn);
}

class Blinker : public rwhost::Firmware {
public:
    explicit Blinker(bool inverted = false) : inverted_(inverted) {}

    const char* name() const override { return "blinker"; }
    void reset() override { std::memset(&g_state, 0, sizeof(g_state)); g_inverted = inverted_; }
    void install() override { rwd::hal_set_isr(kIrqButton, &button_isr); }
    void run(u32 n) override {
        for (u32 i = 0; i < n; ++i) {
            const u32 b = rwd::mmio_read32(kBtn);
            if (b != g_state.seen) {
                g_state.seen = b;
                ++g_state.presses;
                g_state.history[g_state.presses % 4] = rwd::mmio_read32(kCnt);
            }
            const u32 on = (g_state.presses & 1u) != 0 ? 1u : 0u;
            rwd::mmio_write32(kLed, g_inverted ? 1u - on : on);
        }
    }

    std::size_t state_size() const override { return sizeof(BlinkState); }
    void save_state(u8* out) const override { std::memcpy(out, &g_state, sizeof(BlinkState)); }
    void restore_state(const u8* in) override { std::memcpy(&g_state, in, sizeof(BlinkState)); }
    void describe_state(const u8* s, std::vector<rwhost::StateField>* out) const override {
        BlinkState b;
        std::memcpy(&b, s, sizeof(b));
        out->clear();
        out->push_back(rwhost::StateField("seen", b.seen));
        out->push_back(rwhost::StateField("presses", b.presses));
        out->push_back(rwhost::StateField("irqs", b.irqs));
        for (u32 i = 0; i < 4; ++i) {
            out->push_back(rwhost::StateField("history[" + std::to_string(i) + "]", b.history[i]));
        }
    }
    // No state_hash(): the default, a hash of the saved bytes, is what is used.

private:
    bool inverted_;
};

inline constexpr u32 kIters = 600;

inline rwhost::FirmwareRecording record(u32 seed, bool inverted = false) {
    Device  dev(seed);
    Blinker fw(inverted);
    return rwhost::record_firmware(fw, dev.ops(), kBlinkerBuild, seed, rwd::kFlagRecordWrites, kIters);
}

inline std::unique_ptr<rwhost::Firmware> factory(u64 build_id) {
    if (build_id != kBlinkerBuild) {
        return std::unique_ptr<rwhost::Firmware>();
    }
    return std::unique_ptr<rwhost::Firmware>(new Blinker());
}

inline BlinkState decode(const rwhost::Snapshot& s) {
    BlinkState b;
    std::memset(&b, 0, sizeof(b));
    if (s.state.size() == sizeof(b)) {
        std::memcpy(&b, s.state.data(), sizeof(b));
    }
    return b;
}

} // namespace blinker

#endif // REWIND_TESTS_BLINKER_H
