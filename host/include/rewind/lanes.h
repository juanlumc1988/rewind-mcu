// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// What a timeline draws, without drawing it.
//
// A trace is thousands of events across a few thousand cycles. A view of it
// shows one row per kind of event and, along the cycle axis, how many fall in
// each slice of the visible range: dense where the firmware is busy, empty
// where it idles. Zooming changes the range and the number of slices; nothing
// else about this changes. Keeping it free of any widget toolkit is what lets
// it be tested without a screen.

#ifndef REWIND_LANES_H
#define REWIND_LANES_H

#include <cstddef>
#include <vector>

#include "rewind/trace.h"

namespace rwhost {

using rwd::u8;
using rwd::u32;
using rwd::u64;

enum Lane {
    kLaneRead = 0,
    kLaneWrite,
    kLaneIrq,
    kLaneSite,
    kLaneDma,
    kLaneCount
};

const char* lane_name(Lane lane);

// Which lane an event is drawn in. False for events that have none (the stop).
bool lane_of(u8 type, Lane* out);

struct Bucket {
    Bucket() { for (int i = 0; i < kLaneCount; ++i) { n[i] = 0; } }
    u32 n[kLaneCount];
};

// Counts events per lane in `buckets` equal slices of [t0, t1). An event
// exactly at t1 falls in the last slice, so the whole range is covered.
std::vector<Bucket> bucketize(const std::vector<rwd::Event>& events,
                              u64 t0, u64 t1, std::size_t buckets);

// Index of the first event with a timestamp >= ts, or events.size().
std::size_t first_event_at_or_after(const std::vector<rwd::Event>& events, u64 ts);

// The slice of [t0, t1) that a timestamp falls in, for `buckets` slices.
std::size_t bucket_of(u64 ts, u64 t0, u64 t1, std::size_t buckets);

} // namespace rwhost

#endif // REWIND_LANES_H
