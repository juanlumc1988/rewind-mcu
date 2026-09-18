// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/clock.h"

namespace rwd {
namespace {

// Half a wrap period. A sample gap beyond this means the next wrap could be
// missed, so it is reported.
const u32 kSuspiciousDelta = 0x80000000u;

} // namespace

ClockExtender::ClockExtender()
    : last_raw_(0), wraps_(0), suspicious_(0), value_(0), primed_(false) {}

void ClockExtender::reset() {
    last_raw_   = 0;
    wraps_      = 0;
    suspicious_ = 0;
    value_      = 0;
    primed_     = false;
}

u64 ClockExtender::extend(u32 raw) {
    if (!primed_) {
        // The first reading anchors the sequence. A part whose counter is
        // already part-way round at startup -- anything that resets the core
        // without resetting DWT -- would otherwise be charged a spurious
        // wrap on its second sample.
        primed_   = true;
        last_raw_ = raw;
        value_    = (u64)raw;
        return value_;
    }

    if (raw < last_raw_) {
        ++wraps_;
    } else if (raw - last_raw_ >= kSuspiciousDelta) {
        // Forward, but far enough that a wrap could have hidden inside it.
        ++suspicious_;
    }

    last_raw_ = raw;
    value_    = ((u64)wraps_ << 32) | (u64)raw;
    return value_;
}

} // namespace rwd
