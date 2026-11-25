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
//     the evidence is identical. No cleverness here fixes that from the
//     counter alone; it needs a second, slower clock, and with one it is
//     fixed (below). Without one, suspicious() reports the doubt rather than
//     paper over it.
//
// The second clock. Any free-running counter that does not wrap during the
// longest silence will do -- an RTC, a low-power timer, a millisecond tick --
// and it need not be precise. Between two samples the counter says how far it
// moved modulo 2^32, and the reference says roughly how much time passed;
// together they say how many whole periods went by. The reference only has to
// be right to within half a period (2^31 cycles, 12.8 s at 168 MHz), which is
// a loose demand: a 32.768 kHz RTC against a 168 MHz core is good to a few
// thousand cycles per sample, and a crystal that is 200 ppm off moves a
// four-period silence by under three million. What it cannot rescue is a
// reference that is itself wrong by that much -- the wrong cycles-per-tick,
// or a counter that was reset -- so a sample where the two disagree by more
// than a quarter period is counted in doubtful() instead of being believed
// quietly.

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

    // Tells the extender how fast a reference clock runs against the counter:
    // `cycles_per_tick` counter cycles per reference tick. Returns false, and
    // leaves things as they were, for zero or for a tick longer than an eighth
    // of a period, which is too coarse to count periods with. Call before the
    // first sample; it also clears any earlier history.
    bool set_reference(u32 cycles_per_tick);
    bool has_reference() const { return cycles_per_tick_ != 0; }

    // As extend(raw), with the reference clock's reading taken at the same
    // moment. `ref` must be a free-running counter sampled in order, and may
    // itself wrap (the difference is taken modulo 2^32) provided that does not
    // happen twice between samples. Without set_reference() it is extend(raw).
    u64 extend(u32 raw, u32 ref);

    u64 value() const { return value_; }
    u32 wraps() const { return wraps_; }

    // Gaps between consecutive samples larger than half a wrap period. Not
    // proof of a missed wrap, and not able to be: it is the heuristic that
    // says sampling has drifted close enough to the limit that the extension
    // can no longer be trusted. Non-zero means look at why the firmware went
    // quiet, not at this class.
    u32 suspicious() const { return suspicious_; }

    // Whole periods the reference supplied that the counter alone could not
    // have: wraps that would have gone unnoticed.
    u32 recovered() const { return recovered_; }

    // Samples where the reference and the counter disagreed by more than a
    // quarter period. Past half a period the answer would be wrong without
    // saying so; this is the warning before that. Non-zero means the
    // cycles-per-tick is wrong, or the reference is not what it was taken for.
    u32 doubtful() const { return doubtful_; }

private:
    u32 last_raw_;
    u32 last_ref_;
    u32 cycles_per_tick_;
    u32 wraps_;
    u32 suspicious_;
    u32 recovered_;
    u32 doubtful_;
    u64 value_;
    bool primed_;
};

} // namespace rwd

#endif // REWIND_CLOCK_H
