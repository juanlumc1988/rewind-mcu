// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Reverse execution. The property everything rests on is that a checkpoint
// is a complete description of the run at that point -- so reaching an event
// from a nearby checkpoint must be indistinguishable from re-executing the
// whole trace to get there.

#include "doctest.h"

#include <vector>

#include "rewind/session.h"
#include "rewind/timeline.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

const u32 kIters = 6000;

// A trace where the race fired, so there is something interesting to
// navigate to.
std::vector<u8> failing_trace(u64* seed_out = 0) {
    for (u32 seed = 1; seed <= 200u; ++seed) {
        rwhost::RunConfig cfg;
        cfg.seed    = seed;
        cfg.variant = fw::kBuggy;
        const rwhost::RecordResult rec = rwhost::record_run(cfg);
        if (rec.bug_triggered()) {
            if (seed_out != 0) {
                *seed_out = seed;
            }
            return rec.trace;
        }
    }
    return std::vector<u8>();
}

bool same(const rwhost::Snapshot& a, const rwhost::Snapshot& b) {
    return a.valid == b.valid
        && a.event_index == b.event_index
        && a.state_hash == b.state_hash
        && a.ts == b.ts
        && a.fw.consumed == b.fw.consumed
        && a.fw.pending == b.fw.pending
        && a.fw.head == b.fw.head
        && a.fw.tail == b.fw.tail;
}

} // namespace

TEST_CASE("checkpointed seeks match full re-execution exactly") {
    // The golden test. `dense` restarts from a checkpoint a few iterations
    // back; `sparse` has only the one at iteration zero, so every seek
    // re-executes from the start. They must agree at every point.
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline dense;
    rwhost::Timeline sparse;
    REQUIRE(dense.open(trace, kIters, 16));
    REQUIRE(sparse.open(trace, kIters, 100000));   // one checkpoint, at zero

    CHECK(dense.checkpoints() > 100u);
    CHECK(sparse.checkpoints() == 1u);
    REQUIRE(dense.event_count() == sparse.event_count());

    const std::size_t total = dense.event_count();
    REQUIRE(total > 1000u);

    for (std::size_t at = 0; at <= total; at += total / 40u) {
        CAPTURE(at);
        rwhost::Snapshot a;
        rwhost::Snapshot b;
        REQUIRE(dense.seek(at, &a));
        REQUIRE(sparse.seek(at, &b));
        CHECK(same(a, b));
    }
}

TEST_CASE("seeking is repeatable") {
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline timeline;
    REQUIRE(timeline.open(trace, kIters, 64));

    const std::size_t at = timeline.event_count() / 3u;
    rwhost::Snapshot  first;
    REQUIRE(timeline.seek(at, &first));

    // Arrive at the same point from elsewhere, repeatedly.
    for (u32 round = 0; round < 5u; ++round) {
        rwhost::Snapshot elsewhere;
        rwhost::Snapshot again;
        REQUIRE(timeline.seek(timeline.event_count(), &elsewhere));
        REQUIRE(timeline.seek(at, &again));
        CAPTURE(round);
        CHECK(same(first, again));
    }
}

TEST_CASE("the end of the timeline is the end of the recorded run") {
    u64                   seed  = 0;
    const std::vector<u8> trace = failing_trace(&seed);
    REQUIRE_FALSE(trace.empty());

    const rwhost::ReplayResult replayed = rwhost::replay_run(trace, kIters);
    REQUIRE(replayed.loaded);

    rwhost::Timeline timeline;
    REQUIRE(timeline.open(trace, kIters, 128));

    rwhost::Snapshot last;
    REQUIRE(timeline.seek(timeline.event_count(), &last));
    CHECK(last.state_hash == replayed.fw.state_hash);
    CHECK(last.fw.consumed == replayed.fw.consumed);
}

TEST_CASE("the start of the timeline is a firmware that has done nothing") {
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline timeline;
    REQUIRE(timeline.open(trace, kIters, 128));

    rwhost::Snapshot first;
    REQUIRE(timeline.seek(0, &first));
    CHECK(first.fw.consumed == 0u);
    CHECK(first.fw.pending == 0u);
    CHECK(first.fw.head == 0u);
    CHECK(first.fw.tail == 0u);
    CHECK(first.ts == 0u);
}

TEST_CASE("walking backwards retraces the run") {
    // What the feature is for: step back from the end and watch the state
    // unwind. Each step must match a direct seek to the same point.
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline walker;
    rwhost::Timeline oracle;
    REQUIRE(walker.open(trace, kIters, 32));
    REQUIRE(oracle.open(trace, kIters, 32));

    const std::size_t total = walker.event_count();
    rwhost::Snapshot  snap;
    REQUIRE(walker.seek(total, &snap));

    u32 previous_consumed = snap.fw.consumed;
    u64 previous_ts       = snap.ts;

    for (u32 step = 0; step < 60u; ++step) {
        REQUIRE(walker.step_back(17, &snap));

        rwhost::Snapshot direct;
        REQUIRE(oracle.seek(walker.position(), &direct));

        CAPTURE(step);
        CAPTURE(walker.position());
        CHECK(same(snap, direct));

        // Going back in time: neither work done nor the clock may increase.
        CHECK(snap.fw.consumed <= previous_consumed);
        CHECK(snap.ts <= previous_ts);
        previous_consumed = snap.fw.consumed;
        previous_ts       = snap.ts;
    }

    CHECK(snap.fw.consumed < 24u);   // genuinely rewound, not stuck at the end
}

TEST_CASE("stepping forward and back returns to the same place") {
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline timeline;
    REQUIRE(timeline.open(trace, kIters, 64));

    const std::size_t middle = timeline.event_count() / 2u;
    rwhost::Snapshot  before;
    REQUIRE(timeline.seek(middle, &before));

    rwhost::Snapshot ignored;
    REQUIRE(timeline.step_forward(200, &ignored));
    rwhost::Snapshot after;
    REQUIRE(timeline.step_back(200, &after));

    CHECK(timeline.position() == middle);
    CHECK(same(before, after));
}

TEST_CASE("checkpoints are what make backwards navigation affordable") {
    // Without them, every step back re-executes from the beginning, and a
    // walk of N steps costs O(N^2). This measures the difference rather than
    // asserting it in a comment.
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline dense;
    rwhost::Timeline sparse;
    REQUIRE(dense.open(trace, kIters, 8));
    REQUIRE(sparse.open(trace, kIters, 100000));

    const u64 dense_start  = dense.events_replayed();
    const u64 sparse_start = sparse.events_replayed();

    // The same backwards walk on both.
    rwhost::Snapshot snap;
    REQUIRE(dense.seek(dense.event_count(), &snap));
    REQUIRE(sparse.seek(sparse.event_count(), &snap));
    for (u32 step = 0; step < 40u; ++step) {
        REQUIRE(dense.step_back(20, &snap));
        REQUIRE(sparse.step_back(20, &snap));
    }

    const u64 dense_cost  = dense.events_replayed() - dense_start;
    const u64 sparse_cost = sparse.events_replayed() - sparse_start;

    CAPTURE(dense_cost);
    CAPTURE(sparse_cost);
    CHECK(dense_cost * 4u < sparse_cost);
}

TEST_CASE("seeks past either end clamp instead of failing") {
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline timeline;
    REQUIRE(timeline.open(trace, kIters, 128));
    const std::size_t total = timeline.event_count();

    rwhost::Snapshot snap;
    REQUIRE(timeline.seek(total + 100000u, &snap));
    CHECK(timeline.position() == total);

    REQUIRE(timeline.seek(10, &snap));
    REQUIRE(timeline.step_back(9999999u, &snap));
    CHECK(timeline.position() == 0u);
    CHECK(snap.fw.consumed == 0u);
}

TEST_CASE("a timeline refuses a trace it cannot navigate") {
    rwhost::Timeline timeline;

    SUBCASE("corrupt") {
        rwhost::RunConfig cfg;
        cfg.seed = 5;
        rwhost::RecordResult rec = rwhost::record_run(cfg);
        REQUIRE(rec.writer_ok);
        rec.trace[rwd::kTraceHeaderSize + 14u] ^= 0x20u;
        CHECK_FALSE(timeline.open(rec.trace, kIters, 128));
        CHECK_FALSE(timeline.error().empty());
    }
    SUBCASE("unknown build") {
        rwhost::RunConfig cfg;
        cfg.seed = 5;
        rwhost::RecordResult rec = rwhost::record_run(cfg);
        REQUIRE(rec.writer_ok);
        rwd::put_u64_le(rec.trace.data() + 8, 0x1234567800000000ULL);
        CHECK_FALSE(timeline.open(rec.trace, kIters, 128));
        CHECK(timeline.error().find("unknown firmware build") != std::string::npos);
    }
}

namespace {

bool consumed_at_least(const rwhost::Snapshot& snap, void* ctx) {
    return snap.fw.consumed >= *static_cast<const u32*>(ctx);
}

bool never_true(const rwhost::Snapshot&, void*) {
    return false;
}

} // namespace

TEST_CASE("find_first locates the moment a condition first held") {
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline timeline;
    REQUIRE(timeline.open(trace, kIters, 32));

    rwhost::Snapshot last;
    REQUIRE(timeline.seek(timeline.event_count(), &last));
    const u32 final_consumed = last.fw.consumed;
    REQUIRE(final_consumed > 4u);

    for (u32 want = 1; want <= final_consumed; ++want) {
        u32              target = want;
        rwhost::Snapshot found;
        REQUIRE(timeline.find_first(&consumed_at_least, &target, &found));

        CAPTURE(want);
        CHECK(found.fw.consumed >= want);

        // It is the FIRST such point: one event earlier must not qualify.
        if (found.event_index > 0u) {
            rwhost::Snapshot just_before;
            REQUIRE(timeline.seek(found.event_index - 1u, &just_before));
            CHECK(just_before.fw.consumed < want);
        }
    }
}

TEST_CASE("find_first agrees with a linear scan") {
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline timeline;
    REQUIRE(timeline.open(trace, kIters, 32));

    u32              target = 7;
    rwhost::Snapshot found;
    REQUIRE(timeline.find_first(&consumed_at_least, &target, &found));

    // The slow, obviously-correct way.
    std::size_t      linear = timeline.event_count();
    rwhost::Snapshot probe;
    for (std::size_t at = 0; at <= timeline.event_count(); ++at) {
        REQUIRE(timeline.seek(at, &probe));
        if (probe.fw.consumed >= target) {
            linear = at;
            break;
        }
    }
    CHECK(found.event_index == linear);
}

TEST_CASE("searching is logarithmic, not linear") {
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline timeline;
    REQUIRE(timeline.open(trace, kIters, 32));

    u32              target = 12;
    rwhost::Snapshot found;
    REQUIRE(timeline.find_first(&consumed_at_least, &target, &found));

    // log2 of a few thousand events is around twelve; allow generous slack
    // and still be nowhere near the event count.
    CAPTURE(timeline.last_search_seeks());
    CAPTURE(timeline.event_count());
    CHECK(timeline.last_search_seeks() < 32u);
    CHECK(timeline.last_search_seeks() * 50u < timeline.event_count());
}

TEST_CASE("a condition that never holds is reported, not guessed at") {
    const std::vector<u8> trace = failing_trace();
    REQUIRE_FALSE(trace.empty());

    rwhost::Timeline timeline;
    REQUIRE(timeline.open(trace, kIters, 32));

    rwhost::Snapshot found;
    CHECK_FALSE(timeline.find_first(&never_true, 0, &found));
    CHECK(timeline.last_search_seeks() == 1u);   // one probe at the end

    u32 impossible = 1000000u;
    CHECK_FALSE(timeline.find_first(&consumed_at_least, &impossible, &found));
}
