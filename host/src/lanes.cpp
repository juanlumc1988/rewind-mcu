// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/lanes.h"

#include <algorithm>

namespace rwhost {

const char* lane_name(Lane lane) {
    switch (lane) {
        case kLaneRead:  return "reads";
        case kLaneWrite: return "writes";
        case kLaneIrq:   return "interrupts";
        case kLaneSite:  return "probe points";
        case kLaneDma:   return "DMA";
        default:         return "?";
    }
}

bool lane_of(u8 type, Lane* out) {
    switch (type) {
        case rwd::EV_MMIO_READ:  *out = kLaneRead;  return true;
        case rwd::EV_MMIO_WRITE: *out = kLaneWrite; return true;
        case rwd::EV_IRQ_ENTER:
        case rwd::EV_IRQ_AT:     *out = kLaneIrq;   return true;
        case rwd::EV_SITE:       *out = kLaneSite;  return true;
        case rwd::EV_DMA:        *out = kLaneDma;   return true;
        default:                 return false;
    }
}

std::size_t bucket_of(u64 ts, u64 t0, u64 t1, std::size_t buckets) {
    if (buckets == 0 || t1 <= t0 || ts <= t0) {
        return 0;
    }
    if (ts >= t1) {
        return buckets - 1;
    }
    // Through long double: a range of 2^40 cycles times a thousand slices does
    // not fit in 64 bits, and a pixel of error is nothing here.
    const long double frac = (long double)(ts - t0) / (long double)(t1 - t0);
    const std::size_t i    = (std::size_t)(frac * (long double)buckets);
    return std::min(i, buckets - 1);
}

std::vector<Bucket> bucketize(const std::vector<rwd::Event>& events, u64 t0, u64 t1,
                              std::size_t buckets) {
    std::vector<Bucket> out(buckets);
    if (buckets == 0 || t1 <= t0) {
        return out;
    }
    for (std::size_t i = first_event_at_or_after(events, t0); i < events.size(); ++i) {
        const rwd::Event& e = events[i];
        if (e.ts > t1) {
            break;
        }
        Lane lane;
        if (lane_of(e.type, &lane)) {
            ++out[bucket_of(e.ts, t0, t1, buckets)].n[lane];
        }
    }
    return out;
}

std::size_t first_event_at_or_after(const std::vector<rwd::Event>& events, u64 ts) {
    std::size_t lo = 0, hi = events.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (events[mid].ts < ts) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

} // namespace rwhost
