// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Where did each interrupt land, relative to the places replay can deliver it?
//
// Replay delivers an interrupt right after the crossing -- an MMIO access or a
// probe point -- that precedes it in the trace. If the hardware actually took
// the interrupt later than that crossing, the stretch of code that ran in
// between ran *before* the handler on the device and runs *after* it in
// replay. Whether that matters depends on whether that code touches state the
// handler shares; if it does, replay does not reproduce the run, and usually
// says so loudly (a checked write or read comes out different). If it does
// not, nothing is lost.
//
// The trace already says how long that stretch was: every event carries the
// clock. "Slack" is the cycles between the last crossing and the interrupt.
// It needs no return address to compute; the return address, when the trace
// has one, says where the code that ran is.

#ifndef REWIND_LANDINGS_H
#define REWIND_LANDINGS_H

#include <cstddef>
#include <vector>

#include "rewind/trace.h"

namespace rwhost {

using rwd::u8;
using rwd::u32;
using rwd::u64;

struct Landing {
    std::size_t event;            // index of the interrupt in the trace
    u64         ts;               // its timestamp
    u32         vector;

    bool        has_pc;           // recorded as EV_IRQ_AT
    u32         pc;               // the instruction it landed in front of

    // The last crossing before it in trace order: a read, a write or a site.
    bool        has_crossing;
    std::size_t crossing_event;
    u8          crossing_type;
    u32         crossing_addr;    // the address, or the site label

    // Cycles from that crossing to the interrupt, or from the start of the
    // trace when there is none.
    u64         slack;
};

struct LandingStats {
    std::size_t count;
    std::size_t over;             // slack above the threshold
    u64         max_slack;
    u64         median_slack;
    std::size_t with_pc;
};

// One entry per interrupt event, in trace order.
std::vector<Landing> find_landings(const std::vector<rwd::Event>& events);

LandingStats summarize_landings(const std::vector<Landing>& landings, u64 threshold);

// Interrupt entry on a Cortex-M costs about a dozen cycles, plus whatever the
// handler does before it reads the clock. This is comfortably above that. It
// is a guess: nothing here has been calibrated on a part.
const u64 kDefaultLandingThreshold = 64;

} // namespace rwhost

#endif // REWIND_LANDINGS_H
