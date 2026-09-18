// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Widening a 32-bit cycle counter to the 64 bits the trace format expects.
//
// `DWT->CYCCNT` on a Cortex-M is 32 bits and free-running. At 168 MHz it
// wraps every 25.6 seconds, and the trace writer rejects a timestamp that
// moves backwards -- correctly, since every delta after a missed wrap would
// be wrong. So the wrap has to be absorbed here.
//
// The method is the obvious one: a reading lower than the previous reading
// means the counter wrapped, so bump a high word. It rests entirely on being
// sampled more often than once per wrap period, and it is worth being exact
// about what that buys and what it does not:
//
//   * Sampling happens on every recorded MMIO access. Firmware that touches
//     a peripheral even once a second is sampling roughly twenty-five times
//     more often than it needs to, and the extension is exact.
//
//   * Firmware that goes quiet for longer than a wrap period -- deep sleep,
//     a long computation with no peripheral access, a debugger halt -- can
//     skip a whole wrap. The counter alone cannot tell one wrap from two:
//     the evidence is identical. No cleverness here fixes that; it needs a
//     second, slower clock, which is why suspicious() exists to report the
//     doubt rather than paper over it.

#ifndef REWIND_CLOCK_H
#define REWIND_CLOCK_H

#include "rewind/rw_types.h"

namespace rwd {

class ClockExtender {
public:
    ClockExtender();

    void reset();

    // Feeds a raw 32-bit reading and returns the extended 64-bit count.
    // Readings must be non-decreasing modulo 2^32 -- that is, they must come
    // from a free-running counter sampled in order.
    u64 extend(u32 raw);

    u64 value() const { return value_; }
    u32 wraps() const { return wraps_; }

    // Gaps between consecutive samples larger than half a wrap period. Not
    // proof of a missed wrap, and not able to be: it is the heuristic that
    // says sampling has drifted close enough to the limit that the extension
    // can no longer be trusted. Non-zero means look at why the firmware went
    // quiet, not at this class.
    u32 suspicious() const { return suspicious_; }

private:
    u32 last_raw_;
    u32 wraps_;
    u32 suspicious_;
    u64 value_;
    bool primed_;
};

} // namespace rwd

#endif // REWIND_CLOCK_H
