// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Comparing two traces.
//
// The useful comparison is between two runs that should have behaved alike: the
// same firmware on a seed that failed and one that did not, or a build before
// and after a fix. Two traces from different runs never match byte for byte --
// the clock alone differs on every event -- so this is an alignment, not an
// equality test: the events that match are paired off, and what is left over is
// what only one run did. The first place they stop matching is the first thing
// worth looking at.
//
// What counts as the same event is the caller's to say. By default the
// timestamp is not compared (two runs rarely agree on it, and a difference
// that is only timing is not the interesting kind) and neither is the value
// read from any register in `ignore_value_at`, which is how a free-running
// counter is kept from making every read look different.
//
// The alignment is a shortest edit script (Myers). Its cost grows with how
// different the traces are, not with how long they are, and it is capped:
// past `max_edits` differing events the result is still a valid alignment but
// not a minimal one, and says so.

#ifndef REWIND_TRACEDIFF_H
#define REWIND_TRACEDIFF_H

#include <cstddef>
#include <string>
#include <vector>

#include "fw/rx_pump.h"
#include "rewind/trace.h"

namespace rwhost {

using rwd::u8;
using rwd::u32;
using rwd::u64;

struct DiffOptions {
    DiffOptions() : compare_time(false), max_edits(2000) {}

    bool compare_time;                 // also require equal timestamps
    u32  max_edits;                    // stop looking for a minimal alignment here
    std::vector<u32> ignore_value_at;  // register addresses whose value is not compared
};

enum HunkKind {
    kHunkEqual = 0,   // the same events in both
    kHunkOnlyA = 1,   // events only the first trace has
    kHunkOnlyB = 2    // events only the second trace has
};

// A run of the alignment. For kHunkEqual, na == nb. Hunks are in order and
// together cover both traces completely.
struct DiffHunk {
    HunkKind    kind;
    std::size_t a;    // first event index in the first trace
    std::size_t na;   // how many
    std::size_t b;    // first event index in the second trace
    std::size_t nb;
};

struct TraceDiff {
    TraceDiff() : identical(false), minimal(true), first(0), equal(0),
                  only_a(0), only_b(0) {}

    bool identical;
    bool minimal;          // false: stopped at max_edits; valid, not shortest
    std::size_t first;     // index of the first event that differs, in both
    std::size_t equal;
    std::size_t only_a;
    std::size_t only_b;
    std::vector<DiffHunk> hunks;
};

TraceDiff diff_traces(const std::vector<rwd::Event>& a,
                      const std::vector<rwd::Event>& b,
                      const DiffOptions& options = DiffOptions());

// Whether two events are the same under these options.
bool events_match(const rwd::Event& a, const rwd::Event& b, const DiffOptions& options);

// One field that differs between two firmware states.
struct StateDelta {
    std::string name;
    u64         a;
    u64         b;
};

// The fields that differ, in a fixed order; the ring one byte at a time.
std::vector<StateDelta> diff_state(const fw::State& a, const fw::State& b);

} // namespace rwhost

#endif // REWIND_TRACEDIFF_H
