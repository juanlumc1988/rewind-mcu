// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "doctest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "rewind/session.h"
#include "rewind/trace_reader.h"
#include "rewind/tracediff.h"
#include "sim/mcu.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

rwd::Event ev(u8 type, u32 addr, u32 value = 0, u64 ts = 0) {
    rwd::Event e;
    e.type  = type;
    e.ts    = ts;
    e.addr  = addr;
    e.value = value;
    e.len   = 0;
    for (u32 i = 0; i < rwd::kMaxDmaChunk; ++i) {
        e.data[i] = 0;
    }
    return e;
}

// Events drawn from a small alphabet: reads of one of `alphabet` registers.
std::vector<rwd::Event> seq(const std::vector<u32>& symbols) {
    std::vector<rwd::Event> out;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        out.push_back(ev(rwd::EV_MMIO_READ, 0x40000000u + symbols[i] * 4u, 0, i * 10u));
    }
    return out;
}

// The alignment must cover both traces exactly, and what it calls equal must be.
void check_alignment(const std::vector<rwd::Event>& a, const std::vector<rwd::Event>& b,
                     const rwhost::TraceDiff& d, const rwhost::DiffOptions& o) {
    std::size_t ai = 0, bi = 0, eq = 0, oa = 0, ob = 0;
    for (std::size_t h = 0; h < d.hunks.size(); ++h) {
        const rwhost::DiffHunk& hk = d.hunks[h];
        REQUIRE(hk.a == ai);
        REQUIRE(hk.b == bi);
        REQUIRE(hk.na + hk.nb > 0u);
        if (hk.kind == rwhost::kHunkEqual) {
            REQUIRE(hk.na == hk.nb);
            for (std::size_t i = 0; i < hk.na; ++i) {
                REQUIRE(rwhost::events_match(a[ai + i], b[bi + i], o));
            }
            eq += hk.na;
        } else if (hk.kind == rwhost::kHunkOnlyA) {
            REQUIRE(hk.nb == 0u);
            oa += hk.na;
        } else {
            REQUIRE(hk.na == 0u);
            ob += hk.nb;
        }
        // Neighbouring hunks of one kind would have been merged.
        if (h > 0) {
            REQUIRE(d.hunks[h - 1].kind != hk.kind);
        }
        ai += hk.na;
        bi += hk.nb;
    }
    CHECK(ai == a.size());
    CHECK(bi == b.size());
    CHECK(d.equal == eq);
    CHECK(d.only_a == oa);
    CHECK(d.only_b == ob);
}

std::size_t lcs_length(const std::vector<u32>& x, const std::vector<u32>& y) {
    std::vector<std::vector<std::size_t> > t(x.size() + 1, std::vector<std::size_t>(y.size() + 1, 0));
    for (std::size_t i = 1; i <= x.size(); ++i) {
        for (std::size_t j = 1; j <= y.size(); ++j) {
            t[i][j] = (x[i - 1] == y[j - 1]) ? t[i - 1][j - 1] + 1 : std::max(t[i - 1][j], t[i][j - 1]);
        }
    }
    return t[x.size()][y.size()];
}

} // namespace

TEST_CASE("two empty traces are identical, and nothing is a hunk") {
    const rwhost::TraceDiff d = rwhost::diff_traces(std::vector<rwd::Event>(), std::vector<rwd::Event>());
    CHECK(d.identical);
    CHECK(d.hunks.empty());
    CHECK(d.first == 0u);
}

TEST_CASE("identical traces are one equal hunk") {
    const std::vector<rwd::Event> a = seq({1, 2, 3, 4, 5});
    const rwhost::TraceDiff d = rwhost::diff_traces(a, a);
    CHECK(d.identical);
    CHECK(d.minimal);
    REQUIRE(d.hunks.size() == 1u);
    CHECK(d.hunks[0].kind == rwhost::kHunkEqual);
    CHECK(d.hunks[0].na == 5u);
    CHECK(d.first == 5u);
}

TEST_CASE("a trace against nothing is all extra") {
    const std::vector<rwd::Event> a = seq({1, 2, 3});
    rwhost::DiffOptions o;

    rwhost::TraceDiff d = rwhost::diff_traces(a, std::vector<rwd::Event>());
    CHECK_FALSE(d.identical);
    CHECK(d.only_a == 3u);
    CHECK(d.only_b == 0u);
    check_alignment(a, std::vector<rwd::Event>(), d, o);

    d = rwhost::diff_traces(std::vector<rwd::Event>(), a);
    CHECK(d.only_b == 3u);
    check_alignment(std::vector<rwd::Event>(), a, d, o);
}

TEST_CASE("one inserted event, one deleted event, one replaced event") {
    const std::vector<rwd::Event> base = seq({1, 2, 3, 4, 5, 6, 7, 8});
    rwhost::DiffOptions o;

    SUBCASE("inserted") {
        const std::vector<rwd::Event> b = seq({1, 2, 3, 9, 4, 5, 6, 7, 8});
        const rwhost::TraceDiff d = rwhost::diff_traces(base, b);
        CHECK(d.first == 3u);
        CHECK(d.only_a == 0u);
        CHECK(d.only_b == 1u);
        CHECK(d.equal == 8u);
        check_alignment(base, b, d, o);
    }
    SUBCASE("deleted") {
        const std::vector<rwd::Event> b = seq({1, 2, 3, 5, 6, 7, 8});
        const rwhost::TraceDiff d = rwhost::diff_traces(base, b);
        CHECK(d.only_a == 1u);
        CHECK(d.only_b == 0u);
        check_alignment(base, b, d, o);
    }
    SUBCASE("replaced") {
        const std::vector<rwd::Event> b = seq({1, 2, 3, 9, 5, 6, 7, 8});
        const rwhost::TraceDiff d = rwhost::diff_traces(base, b);
        CHECK(d.only_a == 1u);
        CHECK(d.only_b == 1u);
        CHECK(d.equal == 7u);
        check_alignment(base, b, d, o);
    }
}

TEST_CASE("the alignment is a shortest one: checked against a dynamic program") {
    // A small alphabet, so that matches are plentiful and the choices real.
    u64 state = 0x9E3779B97F4A7C15ULL;
    auto next = [&state]() {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return (u32)(state >> 33);
    };

    rwhost::DiffOptions o;
    for (u32 round = 0; round < 400; ++round) {
        std::vector<u32> x, y;
        const u32 n = next() % 26u, m = next() % 26u, alphabet = 2u + next() % 3u;
        for (u32 i = 0; i < n; ++i) { x.push_back(next() % alphabet); }
        for (u32 i = 0; i < m; ++i) { y.push_back(next() % alphabet); }

        const std::vector<rwd::Event> a = seq(x);
        const std::vector<rwd::Event> b = seq(y);
        const rwhost::TraceDiff d = rwhost::diff_traces(a, b, o);
        const std::size_t lcs = lcs_length(x, y);

        CAPTURE(round);
        CHECK(d.minimal);
        CHECK(d.equal  == lcs);
        CHECK(d.only_a == x.size() - lcs);
        CHECK(d.only_b == y.size() - lcs);
        check_alignment(a, b, d, o);
    }
}

TEST_CASE("past the cap the alignment is still valid, and says it is not minimal") {
    // Two traces with nothing in common cannot be aligned in two edits.
    const std::vector<rwd::Event> a = seq({1, 1, 1, 1, 1, 1, 1, 1, 1, 1});
    const std::vector<rwd::Event> b = seq({2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2});
    rwhost::DiffOptions o;
    o.max_edits = 2;

    const rwhost::TraceDiff d = rwhost::diff_traces(a, b, o);
    CHECK_FALSE(d.minimal);
    CHECK_FALSE(d.identical);
    CHECK(d.first == 0u);
    check_alignment(a, b, d, o);

    // With room it finds the same answer: nothing matches.
    o.max_edits = 100;
    const rwhost::TraceDiff full = rwhost::diff_traces(a, b, o);
    CHECK(full.minimal);
    CHECK(full.equal == 0u);
    check_alignment(a, b, full, o);
}

TEST_CASE("a common prefix and suffix are kept whatever the cap") {
    std::vector<u32> x, y;
    for (u32 i = 0; i < 50; ++i) { x.push_back(i % 3u); y.push_back(i % 3u); }
    x.insert(x.begin() + 25, 7, 5u);          // seven extra events in the middle of x
    y.insert(y.begin() + 25, 9, 6u);          // nine different ones in y

    rwhost::DiffOptions o;
    o.max_edits = 4;                           // not enough to align the middle
    const std::vector<rwd::Event> a = seq(x), b = seq(y);
    const rwhost::TraceDiff d = rwhost::diff_traces(a, b, o);
    CHECK_FALSE(d.minimal);
    CHECK(d.first == 25u);
    CHECK(d.equal == 50u);                     // the prefix and suffix, all of them
    check_alignment(a, b, d, o);
}

TEST_CASE("timestamps count only when asked") {
    std::vector<rwd::Event> a = seq({1, 2, 3});
    std::vector<rwd::Event> b = seq({1, 2, 3});
    b[1].ts += 5;

    CHECK(rwhost::diff_traces(a, b).identical);

    rwhost::DiffOptions strict;
    strict.compare_time = true;
    const rwhost::TraceDiff d = rwhost::diff_traces(a, b, strict);
    CHECK_FALSE(d.identical);
    CHECK(d.first == 1u);
}

TEST_CASE("a register listed as ignored matches whatever it held") {
    std::vector<rwd::Event> a, b;
    a.push_back(ev(rwd::EV_MMIO_READ, sim::kSystickCnt, 100));
    a.push_back(ev(rwd::EV_MMIO_READ, sim::kUart0Sr, 1));
    b.push_back(ev(rwd::EV_MMIO_READ, sim::kSystickCnt, 999));
    b.push_back(ev(rwd::EV_MMIO_READ, sim::kUart0Sr, 1));

    CHECK_FALSE(rwhost::diff_traces(a, b).identical);

    rwhost::DiffOptions o;
    o.ignore_value_at.push_back(sim::kSystickCnt);
    CHECK(rwhost::diff_traces(a, b, o).identical);

    // Only for reads: a write of a different value to it is a difference.
    a.push_back(ev(rwd::EV_MMIO_WRITE, sim::kSystickCnt, 1));
    b.push_back(ev(rwd::EV_MMIO_WRITE, sim::kSystickCnt, 2));
    CHECK_FALSE(rwhost::diff_traces(a, b, o).identical);
    // And the register itself still has to be the same one.
    b.pop_back();
    b.push_back(ev(rwd::EV_MMIO_WRITE, sim::kGpio0Odr, 1));
    CHECK_FALSE(rwhost::diff_traces(a, b, o).identical);
}

TEST_CASE("events of different kinds, or with different DMA bytes, differ") {
    rwhost::DiffOptions o;
    CHECK_FALSE(rwhost::events_match(ev(rwd::EV_MMIO_READ, 1), ev(rwd::EV_MMIO_WRITE, 1), o));
    CHECK_FALSE(rwhost::events_match(ev(rwd::EV_IRQ_ENTER, 1), ev(rwd::EV_IRQ_AT, 1), o));

    rwd::Event d1 = ev(rwd::EV_DMA, 0, 4);
    rwd::Event d2 = d1;
    d1.len = d2.len = 3;
    d1.data[0] = d2.data[0] = 1;
    d1.data[1] = d2.data[1] = 2;
    d1.data[2] = 3;
    d2.data[2] = 4;
    CHECK_FALSE(rwhost::events_match(d1, d2, o));
    d2.data[2] = 3;
    CHECK(rwhost::events_match(d1, d2, o));
    // Bytes past the length are not part of the event.
    d2.data[10] = 0xFF;
    CHECK(rwhost::events_match(d1, d2, o));
    d2.len = 2;
    CHECK_FALSE(rwhost::events_match(d1, d2, o));
}

TEST_CASE("the state diff lists the fields that differ, in a fixed order") {
    fw::State a = fw::State();
    fw::State b = fw::State();
    CHECK(rwhost::diff_state(a, b).empty());

    b.pending  = 2;
    b.consumed = 19;
    b.ring[3]  = 0x55;
    b.dma_lost = 1;
    const std::vector<rwhost::StateDelta> d = rwhost::diff_state(a, b);
    REQUIRE(d.size() == 4u);
    CHECK(d[0].name == "pending");
    CHECK(d[0].a == 0u);
    CHECK(d[0].b == 2u);
    CHECK(d[1].name == "consumed");
    CHECK(d[2].name == "dma_lost");
    CHECK(d[3].name == "ring[3]");
    CHECK(d[3].b == 0x55u);
}

TEST_CASE("a failing run against a clean one, from the example firmware") {
    rwhost::DiffOptions o;
    o.ignore_value_at.push_back(sim::kSystickCnt);

    // The same seed, buggy and fixed: the same interrupt arrivals until the bug
    // bites, so the runs agree for a long stretch and then part.
    rwhost::RunConfig cfg;
    cfg.seed = 2;
    cfg.variant = fw::kBuggy;
    const rwhost::RecordResult bug = rwhost::record_run(cfg);
    cfg.variant = fw::kFixed;
    const rwhost::RecordResult fix = rwhost::record_run(cfg);
    REQUIRE(bug.bug_triggered());
    REQUIRE_FALSE(fix.bug_triggered());

    auto decode = [](const std::vector<u8>& t) {
        std::vector<rwd::Event> out;
        rwd::TraceReader r;
        REQUIRE(r.open(t.data(), (u32)t.size()));
        rwd::Event e;
        while (r.next(&e)) { out.push_back(e); }
        CHECK(r.ok());
        return out;
    };
    const std::vector<rwd::Event> a = decode(bug.trace);
    const std::vector<rwd::Event> b = decode(fix.trace);

    const rwhost::TraceDiff same = rwhost::diff_traces(a, a, o);
    CHECK(same.identical);

    const rwhost::TraceDiff d = rwhost::diff_traces(a, b, o);
    CHECK_FALSE(d.identical);
    CHECK(d.minimal);
    CHECK(d.first > 100u);        // they agreed for a good while
    CHECK(d.equal > a.size() / 2);
    check_alignment(a, b, d, o);
}
