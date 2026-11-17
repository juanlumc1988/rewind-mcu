// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Finding where an interrupt landed, when the trace does not say.
//
// A trace recorded without its probe points (kFlagNoSites) says that an
// interrupt came after crossing A and before crossing B, and no more. Replay
// alone delivers it right after A. If the firmware did something between A and
// B that the handler also touches, that is the wrong place, and the replay
// diverges from the recording.
//
// But the build that replays has probe points the recording did not keep, and
// each is a place the interrupt could have been delivered. Try them. The
// recorded reads and writes that come after are an oracle: a delivery that
// matches the real landing reproduces them, and one that does not makes the
// firmware read or write something the trace does not have, at which point
// replay diverges, and how far it got says how wrong the guess was.
//
// This is a search over a replay, not a reconstruction. It is depth-first with
// backtracking: fix a placement for the first interrupt, move on to the next,
// and when none of the next one's placements fits, go back and try another for
// the one before. A placement "fits" if the replay gets past the next
// interrupt (or to the end) without diverging; the last interrupt must replay
// to the end. Limits, stated because they decide when to trust it:
//
//  * The oracle is the *observable* part of the run. A landing that changes
//    nothing a register read or a recorded write ever shows is
//    indistinguishable from the real one, and that is fine -- but it means two
//    placements can both reproduce the trace. After finding one, each interrupt
//    is tried at its other placements with everything else held; any that also
//    reproduces the whole trace makes it ambiguous, and the smallest is kept.
//  * It can only choose among the probe points the host build has. If the real
//    landing was somewhere with none, no placement fits and the first such
//    interrupt is reported unresolved; the ones after it are not searched.
//  * Every run replays the trace from the start, so the cost is the number of
//    runs times the length of the trace. That is nothing for a few thousand
//    events and hopeless for a long recording; resuming from checkpoints, as
//    reverse execution does, would fix it. max_runs stops a search that is
//    getting out of hand, and says so.

#ifndef REWIND_LOCATE_H
#define REWIND_LOCATE_H

#include <cstddef>
#include <string>
#include <vector>

#include "rewind/replay.h"

namespace rwhost {

// Runs the firmware once, from scratch, against `hal`: reset its state, attach
// hal->ops() to the shim in replay mode, install the handlers, run the main
// loop to the end, and detach. The search reads hal->diverged() and
// hal->cursor() afterwards. The last run it makes is the located replay, so
// the firmware's state when locate_landings() returns is that replay's outcome.
typedef void (*RunFirmwareFn)(void* ctx, ReplayHal* hal);

struct LocateOptions {
    LocateOptions() : max_probes(32), max_runs(20000) {}
    // Most probe points to try past one crossing, per interrupt. A firmware
    // with a probe in a long loop has as many candidates as iterations.
    u32 max_probes;
    // Most replays of the firmware, in all, before giving up.
    u32 max_runs;
};

struct LandingChoice {
    std::size_t event;        // the interrupt's index in the trace
    u64         ts;
    u32         vector;
    u32         probes;       // delivered after this many probe points past the
                              // last crossing (0 = right after the crossing)
    u32         candidates;   // placements tried
    u32         fitting;      // placements that reproduce the whole trace with
                              // every other interrupt where it was put
    bool        resolved;     // a placement was found that fits
    bool        ambiguous;    // more than one reproduces the trace
    bool        truncated;    // hit max_probes with more still possible
};

struct LocateResult {
    LocateResult() : applicable(false), runs(0), gave_up(false),
                     final_diverged(false), final_cursor(0), events_total(0) {}

    bool        applicable;   // false: the trace has nothing to search for
    std::string why;          // when not applicable, or the trace is unusable

    std::vector<LandingChoice> choices;
    u32         runs;         // times the firmware was replayed
    bool        gave_up;      // stopped at max_runs

    // The replay with every chosen landing applied.
    bool        final_diverged;
    std::string final_divergence;
    std::size_t final_cursor;
    std::size_t events_total;

    std::size_t unresolved() const;
    std::size_t ambiguous() const;
};

LocateResult locate_landings(const std::vector<u8>& trace, RunFirmwareFn run,
                             void* ctx, const LocateOptions& options = LocateOptions());

} // namespace rwhost

#endif // REWIND_LOCATE_H
