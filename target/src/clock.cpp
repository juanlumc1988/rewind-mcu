// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/clock.h"

namespace rwd {
namespace {

// Half a wrap period. A sample gap beyond this means the next wrap could be
// missed, so it is reported.
const u32 kSuspiciousDelta = 0x80000000u;

// A reference tick longer than this cannot place a wrap to within half a
// period with any margin.
const u32 kMaxCyclesPerTick = 0x20000000u;

// A quarter period: the reference and the counter should agree far better
// than this, so a gap this wide means one of them is not what it was taken for.
const u64 kDoubtfulError = 0x40000000ULL;

} // namespace

ClockExtender::ClockExtender()
    : last_raw_(0), last_ref_(0), cycles_per_tick_(0), wraps_(0), suspicious_(0),
      recovered_(0), doubtful_(0), value_(0), primed_(false) {}

// Forgets the history and keeps the reference's calibration, so a recording
// that is started again does not have to be told it again.
void ClockExtender::reset() {
    last_raw_   = 0;
    last_ref_   = 0;
    wraps_      = 0;
    suspicious_ = 0;
    recovered_  = 0;
    doubtful_   = 0;
    value_      = 0;
    primed_     = false;
}

bool ClockExtender::set_reference(u32 cycles_per_tick) {
    if (cycles_per_tick == 0 || cycles_per_tick > kMaxCyclesPerTick) {
        return false;
    }
    reset();
    cycles_per_tick_ = cycles_per_tick;
    return true;
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

// How many whole periods hid between two samples. The counter moved `step`
// cycles modulo 2^32, and the reference says about `estimate` cycles went by in
// all; the periods are whatever makes the two agree best. An estimate below the
// step (the reference ran slow, or a tick of quantisation) means none.
u64 ClockExtender::extend(u32 raw, u32 ref) {
    if (cycles_per_tick_ == 0) {
        return extend(raw);
    }
    if (!primed_) {
        last_ref_ = ref;
        return extend(raw);
    }

    const u32 step     = raw - last_raw_;                     // modulo 2^32, on purpose
    const u64 estimate = (u64)(u32)(ref - last_ref_) * (u64)cycles_per_tick_;

    u64 periods = 0;
    if (estimate > (u64)step) {
        const u64 over = estimate - (u64)step;
        // Round to the nearest whole period rather than truncating: the
        // estimate errs either way, and half a period is the margin it has.
        periods = (over >> 32) + ((over >> 31) & 1u);
    }

    const u64 elapsed = (u64)step + (periods << 32);
    const u64 error   = estimate > elapsed ? estimate - elapsed : elapsed - estimate;
    if (error > kDoubtfulError) {
        ++doubtful_;
    }
    recovered_ += (u32)periods;

    last_raw_ = raw;
    last_ref_ = ref;
    value_   += elapsed;
    wraps_    = (u32)(value_ >> 32);
    return value_;
}

} // namespace rwd
