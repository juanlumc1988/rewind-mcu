// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Reverse execution: moving to any point in a recorded run, forwards or
// backwards, and reading the firmware's state there.
//
// How it works
// ------------
// Replay is deterministic, so the state at event N is a pure function of the
// trace and N. Going backwards is therefore not a matter of undoing
// anything -- it is re-executing to an earlier point.
//
// Done naively that costs O(N) per query, and walking back through a long
// trace one event at a time costs O(N^2). Checkpoints fix it: snapshot the
// state periodically, and a seek restarts from the nearest checkpoint at or
// before the target instead of from the beginning.
//
// Why not fork()
// --------------
// rr and UndoDB checkpoint with fork(), because the state of a general
// Linux process is large, opaque and full of things you cannot copy by hand
// -- file descriptors, mappings, thread stacks. Copy-on-write is the only
// affordable way to snapshot it.
//
// None of that applies here. The entire state of the system under replay is
// three things: the firmware's static storage, the replay cursor, and the
// shim's own handful of scalars. No heap, no dynamic objects, nothing
// hidden. A checkpoint is a struct copy of a few dozen bytes, which is
// faster than fork(), portable off Linux, and debuggable.
//
// That is not a shortcut around the hard problem. It is the hard problem
// being genuinely easier in this domain: bare-metal firmware has no private
// state by construction, which is exactly the property that makes reverse
// execution cheap here and expensive everywhere else.
//
// The one thing a struct copy cannot capture is the call stack -- where in
// the code execution was. So checkpoints are taken between main-loop
// iterations, where there is no call in progress and the interrupt mask is
// balanced. Seeks land on an exact event by replaying forward from a
// checkpoint and snapshotting at the moment that event is about to be
// served.

#ifndef REWIND_TIMELINE_H
#define REWIND_TIMELINE_H

#include <cstddef>
#include <string>
#include <vector>

#include "fw/rx_pump.h"
#include "rewind/replay.h"
#include "rewind/session.h"

namespace rwhost {

// The firmware's state at one point in the recorded run.
struct Snapshot {
    std::size_t event_index = 0;   // events served before this point
    u64         ts          = 0;   // cycle count of the last event served
    u32         main_iter   = 0;   // main-loop iteration in progress
    u64         state_hash  = 0;
    fw::State   fw;
    bool        valid = false;

    Snapshot() { fw.head = 0; }
};

class Timeline {
public:
    Timeline();

    // Loads a trace and builds the checkpoint index. `main_iters` must match
    // the recording, as for replay_run(). A smaller `checkpoint_every`
    // (measured in main-loop iterations) makes seeks faster and costs
    // roughly sizeof(Snapshot) per checkpoint.
    bool open(const std::vector<u8>& trace, u32 main_iters,
              u32 checkpoint_every = 128);

    std::size_t event_count() const { return event_count_; }
    std::size_t position() const { return position_; }
    std::size_t checkpoints() const { return checkpoints_.size(); }

    // Moves to `event_index` and reports the firmware state at the instant
    // that event is about to be served. Index 0 is the state before the run
    // begins; event_count() is the state at the end.
    bool seek(std::size_t event_index, Snapshot* out);

    // Relative movement. Stepping back past the start, or forward past the
    // end, clamps rather than failing.
    bool step_back(std::size_t n, Snapshot* out);
    bool step_forward(std::size_t n, Snapshot* out);

    // Answers "when did X first become true?" by bisection.
    //
    // This is what makes a timeline more than a curiosity. Asking when a
    // counter reached a value, or when a buffer first filled, is otherwise a
    // linear scan of the whole run; here it is a handful of seeks.
    //
    // `pred` must be monotonic over the run -- false up to some point and
    // true from there on. Most things worth asking about firmware are:
    // bytes consumed, a flag that latches, a high-water mark. A predicate
    // that flickers will return *a* point where it holds, not the first.
    typedef bool (*Predicate)(const Snapshot& snap, void* ctx);
    bool find_first(Predicate pred, void* ctx, Snapshot* out);

    // Seeks performed by the last find_first call.
    u32 last_search_seeks() const { return last_search_seeks_; }

    // Total events re-served since open(). The honest measure of what
    // navigation costs: compare it against what seeking without checkpoints
    // would have needed.
    u64 events_replayed() const { return replay_.events_served(); }

    const std::string& error() const { return error_; }

private:
    struct Checkpoint {
        u32         main_iter;
        std::size_t cursor;
        u64         ts;
        fw::State   fw;
    };

    // Puts the shim into replay mode with a given firmware state and cursor.
    void install(const fw::State& state, std::size_t cursor, u64 ts);
    const Checkpoint* nearest(std::size_t event_index) const;
    static void capture(void* ctx);

    Timeline(const Timeline&);
    Timeline& operator=(const Timeline&);

    ReplayHal               replay_;
    std::vector<Checkpoint> checkpoints_;
    fw::Variant             variant_;
    u32                     main_iters_;
    u32                     checkpoint_every_;
    std::size_t             event_count_;
    std::size_t             position_;
    u32                     last_search_seeks_;
    bool                    opened_;
    std::string             error_;

    // Scratch used by the capture callback.
    Snapshot* pending_;
    u32       current_iter_;
};

} // namespace rwhost

#endif // REWIND_TIMELINE_H
