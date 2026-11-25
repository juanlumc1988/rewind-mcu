// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/landings.h"

#include <algorithm>

namespace rwhost {

namespace {

bool is_crossing(u8 type) {
    return type == rwd::EV_MMIO_READ || type == rwd::EV_MMIO_WRITE
        || type == rwd::EV_SITE || type == rwd::EV_DMA;
}

bool is_interrupt(u8 type) {
    return type == rwd::EV_IRQ_ENTER || type == rwd::EV_IRQ_AT;
}

} // namespace

std::vector<Landing> find_landings(const std::vector<rwd::Event>& events) {
    std::vector<Landing> out;

    bool        have = false;
    std::size_t last = 0;

    for (std::size_t i = 0; i < events.size(); ++i) {
        const rwd::Event& ev = events[i];

        if (is_crossing(ev.type)) {
            have = true;
            last = i;
            continue;
        }
        if (!is_interrupt(ev.type)) {
            continue;
        }

        Landing l;
        l.event          = i;
        l.ts             = ev.ts;
        l.vector         = ev.addr;
        l.has_pc         = (ev.type == rwd::EV_IRQ_AT);
        l.pc             = l.has_pc ? ev.value : 0u;
        l.has_crossing   = have;
        l.crossing_event = have ? last : 0u;
        l.crossing_type  = have ? events[last].type : 0u;
        l.crossing_addr  = have ? events[last].addr : 0u;
        // Timestamps never go backwards (the writer refuses one), but a trace
        // is input, so do not let a hostile one underflow this.
        const u64 since  = have ? events[last].ts : 0u;
        l.slack          = ev.ts >= since ? ev.ts - since : 0u;
        out.push_back(l);
    }
    return out;
}

LandingStats summarize_landings(const std::vector<Landing>& landings, u64 threshold) {
    LandingStats s;
    s.count        = landings.size();
    s.over         = 0;
    s.max_slack    = 0;
    s.median_slack = 0;
    s.with_pc      = 0;

    std::vector<u64> slacks;
    for (std::size_t i = 0; i < landings.size(); ++i) {
        const Landing& l = landings[i];
        slacks.push_back(l.slack);
        if (l.slack > threshold) {
            ++s.over;
        }
        if (l.slack > s.max_slack) {
            s.max_slack = l.slack;
        }
        if (l.has_pc) {
            ++s.with_pc;
        }
    }
    if (!slacks.empty()) {
        std::sort(slacks.begin(), slacks.end());
        // Lower median: an actual observed value, not an average of two.
        s.median_slack = slacks[(slacks.size() - 1) / 2];
    }
    return s;
}

} // namespace rwhost
