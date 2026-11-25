// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// What the command line and the viewer share: words for events, the lanes a
// timeline draws, and a trace opened for looking at.

#include "doctest.h"

#include <string>
#include <vector>

#include "rewind/describe.h"
#include "rewind/lanes.h"
#include "rewind/session.h"
#include "rewind/tracedoc.h"
#include "sim/mcu.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

rwd::Event ev(u8 type, u64 ts, u32 addr, u32 value = 0) {
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

} // namespace

TEST_CASE("events are described in words") {
    using rwhost::describe_event;
    CHECK(describe_event(ev(rwd::EV_MMIO_READ, 0, sim::kSystickCnt, 6)) ==
          "read 0x40000000 SYSTICK.CNT -> 0x00000006");
    CHECK(describe_event(ev(rwd::EV_MMIO_WRITE, 0, sim::kGpio0Odr, 0x14)) ==
          "write 0x40002000 GPIO0.ODR <- 0x00000014");
    // An address with no name is not given a blank one.
    CHECK(describe_event(ev(rwd::EV_MMIO_READ, 0, 0x50000000u, 1)) ==
          "read 0x50000000 -> 0x00000001");
    CHECK(describe_event(ev(rwd::EV_IRQ_ENTER, 0, sim::kIrqUartRx)) == "irq vector 1 UART_RX");
    CHECK(describe_event(ev(rwd::EV_IRQ_ENTER, 0, 5)) == "irq vector 5");
    CHECK(describe_event(ev(rwd::EV_IRQ_AT, 0, 1, 0x08001234u)) ==
          "irq vector 1 UART_RX returns to 0x08001234");
    CHECK(describe_event(ev(rwd::EV_SITE, 0, 7)) == "site 7");
    CHECK(describe_event(ev(rwd::EV_STOP, 0, 0)) == "stop");

    rwd::Event d = ev(rwd::EV_DMA, 0, 2, 12);
    d.len = 3;
    d.data[0] = 0xDE;
    d.data[1] = 0xAD;
    d.data[2] = 0x01;
    CHECK(describe_event(d) == "dma region 2 +12 len 3 DEAD01");

    CHECK(describe_event(ev(0x7F, 0, 0)).find("type 0x7F") != std::string::npos);
}

TEST_CASE("register and event names") {
    CHECK(std::string(rwhost::register_name(sim::kDma0Count)) == "DMA0.COUNT");
    CHECK(std::string(rwhost::register_name(0x12345678u)) == "");
    CHECK(std::string(rwhost::event_kind(rwd::EV_IRQ_AT)) == "irq");
    CHECK(std::string(rwhost::event_kind(rwd::EV_IRQ_ENTER)) == "irq");
    CHECK(std::string(rwhost::event_kind(0x7F)) == "?");
}

TEST_CASE("every event kind has a lane, except the stop") {
    rwhost::Lane lane = rwhost::kLaneCount;
    CHECK(rwhost::lane_of(rwd::EV_MMIO_READ, &lane));  CHECK(lane == rwhost::kLaneRead);
    CHECK(rwhost::lane_of(rwd::EV_MMIO_WRITE, &lane)); CHECK(lane == rwhost::kLaneWrite);
    CHECK(rwhost::lane_of(rwd::EV_IRQ_ENTER, &lane));  CHECK(lane == rwhost::kLaneIrq);
    CHECK(rwhost::lane_of(rwd::EV_IRQ_AT, &lane));     CHECK(lane == rwhost::kLaneIrq);
    CHECK(rwhost::lane_of(rwd::EV_SITE, &lane));       CHECK(lane == rwhost::kLaneSite);
    CHECK(rwhost::lane_of(rwd::EV_DMA, &lane));        CHECK(lane == rwhost::kLaneDma);
    CHECK_FALSE(rwhost::lane_of(rwd::EV_STOP, &lane));
    CHECK_FALSE(rwhost::lane_of(0x7F, &lane));
    for (int i = 0; i < rwhost::kLaneCount; ++i) {
        CHECK(std::string(rwhost::lane_name((rwhost::Lane)i)) != "?");
    }
}

TEST_CASE("bucketing counts every event once, in the slice its time falls in") {
    std::vector<rwd::Event> e;
    e.push_back(ev(rwd::EV_MMIO_READ, 0, 1));
    e.push_back(ev(rwd::EV_MMIO_READ, 9, 1));
    e.push_back(ev(rwd::EV_MMIO_WRITE, 10, 1));
    e.push_back(ev(rwd::EV_IRQ_ENTER, 25, 1));
    e.push_back(ev(rwd::EV_SITE, 39, 1));
    e.push_back(ev(rwd::EV_MMIO_READ, 40, 1));    // exactly at the end of the range
    e.push_back(ev(rwd::EV_STOP, 40, 0));         // no lane: not counted

    const std::vector<rwhost::Bucket> b = rwhost::bucketize(e, 0, 40, 4);   // slices of ten
    REQUIRE(b.size() == 4u);
    CHECK(b[0].n[rwhost::kLaneRead] == 2u);       // 0 and 9
    CHECK(b[1].n[rwhost::kLaneWrite] == 1u);      // 10
    CHECK(b[2].n[rwhost::kLaneIrq] == 1u);        // 25
    CHECK(b[3].n[rwhost::kLaneSite] == 1u);       // 39
    CHECK(b[3].n[rwhost::kLaneRead] == 1u);       // 40 lands in the last slice

    u32 total = 0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        for (int l = 0; l < rwhost::kLaneCount; ++l) {
            total += b[i].n[l];
        }
    }
    CHECK(total == 6u);
}

TEST_CASE("bucketing a sub-range counts only what is in it") {
    std::vector<rwd::Event> e;
    for (u64 t = 0; t < 100; ++t) {
        e.push_back(ev(rwd::EV_MMIO_READ, t, 1));
    }
    const std::vector<rwhost::Bucket> b = rwhost::bucketize(e, 20, 30, 2);
    // 20..30 inclusive of both ends is eleven events.
    CHECK(b[0].n[rwhost::kLaneRead] + b[1].n[rwhost::kLaneRead] == 11u);
}

TEST_CASE("degenerate ranges give empty buckets, not a crash") {
    std::vector<rwd::Event> e;
    e.push_back(ev(rwd::EV_MMIO_READ, 5, 1));
    CHECK(rwhost::bucketize(e, 0, 10, 0).empty());
    CHECK(rwhost::bucketize(e, 10, 10, 4).size() == 4u);
    CHECK(rwhost::bucketize(e, 10, 5, 4).size() == 4u);
    CHECK(rwhost::bucketize(std::vector<rwd::Event>(), 0, 10, 3).size() == 3u);
    CHECK(rwhost::bucket_of(7, 5, 5, 4) == 0u);
}

TEST_CASE("a huge range does not overflow the arithmetic") {
    const u64 big = 1ULL << 62;
    CHECK(rwhost::bucket_of(0, 0, big, 1000) == 0u);
    CHECK(rwhost::bucket_of(big, 0, big, 1000) == 999u);
    CHECK(rwhost::bucket_of(big / 2, 0, big, 1000) == 500u);
    std::size_t last = 0;
    for (u64 t = 0; t <= big; t += big / 97) {
        const std::size_t i = rwhost::bucket_of(t, 0, big, 1000);
        CHECK(i >= last);                 // never goes backwards
        CHECK(i < 1000u);
        last = i;
    }
}

TEST_CASE("the first event at or after a time is found by bisection") {
    std::vector<rwd::Event> e;
    const u64 times[6] = { 3, 3, 8, 8, 8, 20 };
    for (int i = 0; i < 6; ++i) {
        e.push_back(ev(rwd::EV_MMIO_READ, times[i], 1));
    }
    CHECK(rwhost::first_event_at_or_after(e, 0) == 0u);
    CHECK(rwhost::first_event_at_or_after(e, 3) == 0u);
    CHECK(rwhost::first_event_at_or_after(e, 4) == 2u);
    CHECK(rwhost::first_event_at_or_after(e, 8) == 2u);
    CHECK(rwhost::first_event_at_or_after(e, 9) == 5u);
    CHECK(rwhost::first_event_at_or_after(e, 21) == 6u);
    CHECK(rwhost::first_event_at_or_after(std::vector<rwd::Event>(), 5) == 0u);
}

namespace {

rwhost::RecordResult record(u64 seed, fw::Variant v) {
    rwhost::RunConfig cfg;
    cfg.seed    = seed;
    cfg.variant = v;
    return rwhost::record_run(cfg);
}

} // namespace

TEST_CASE("a trace opened for looking at") {
    const rwhost::RecordResult rec = record(2, fw::kBuggy);
    const rwhost::ReplayResult rep = rwhost::replay_run(rec.trace, rwhost::RunConfig().main_iters);

    rwhost::TraceDoc doc;
    REQUIRE(doc.open(rec.trace));
    CHECK(doc.error().empty());
    CHECK(doc.events().size() == rep.events_total);
    CHECK(doc.header().seed == 2u);
    CHECK(doc.size_bytes() == rec.trace.size());
    CHECK(doc.duration() == doc.events().back().ts);
    CHECK(doc.has_firmware());
    CHECK(doc.variant() == fw::kBuggy);
    CHECK(std::string(doc.variant_name()) == "buggy");

    // The firmware before the run and after it.
    rwhost::Snapshot start, end, past;
    REQUIRE(doc.snapshot_at(0, &start));
    CHECK(start.fw.consumed == 0u);
    CHECK(start.event_index == 0u);
    REQUIRE(doc.snapshot_at(doc.events().size(), &end));
    CHECK(end.state_hash == rep.fw.state_hash);
    CHECK(end.fw.consumed == rep.fw.consumed);

    // Asking past the end is the end. The trailing stop is never served -- the
    // firmware does not ask for it -- so the end is one before the last event.
    REQUIRE(doc.snapshot_at(doc.events().size() + 100, &past));
    CHECK(past.event_index == doc.events().size() - 1u);
    CHECK(end.event_index == past.event_index);
    CHECK(past.state_hash == end.state_hash);
}

TEST_CASE("a trace from a build this cannot run is still readable") {
    const rwhost::RecordResult rec = record(2, fw::kBuggy);
    std::vector<u8> foreign = rec.trace;
    rwd::put_u64_le(foreign.data() + 8, 0x1234567890ABCDEFULL);   // the build id

    rwhost::TraceDoc doc;
    REQUIRE(doc.open(foreign));
    CHECK_FALSE(doc.has_firmware());
    CHECK(std::string(doc.variant_name()) == "unknown");
    CHECK(doc.events().size() > 1000u);

    rwhost::Snapshot s;
    CHECK_FALSE(doc.snapshot_at(10, &s));
}

TEST_CASE("a trace that cannot be read is refused whole") {
    rwhost::TraceDoc doc;

    const std::vector<u8> junk(100, 0x5A);
    CHECK_FALSE(doc.open(junk));
    CHECK_FALSE(doc.error().empty());
    CHECK(doc.events().empty());

    // Cut off in the middle of a block, not at the end of one.
    const rwhost::RecordResult rec = record(2, fw::kBuggy);
    std::vector<u8> cut(rec.trace.begin(), rec.trace.end() - 21);
    CHECK_FALSE(doc.open(cut));
    CHECK(doc.events().empty());

    // And a good one afterwards opens as if nothing had happened.
    CHECK(doc.open(rec.trace));
    CHECK_FALSE(doc.events().empty());
}

TEST_CASE("two documents can be looked at in turn") {
    // The shim and the firmware's state are one per process, so each lookup
    // has to install what it needs from scratch. Interleaved, they must agree
    // with each looked at alone.
    const rwhost::RecordResult ra = record(2, fw::kBuggy);
    const rwhost::RecordResult rb = record(2, fw::kFixed);

    rwhost::TraceDoc a, b, alone_a, alone_b;
    REQUIRE(a.open(ra.trace));
    REQUIRE(b.open(rb.trace));
    REQUIRE(alone_a.open(ra.trace));
    REQUIRE(alone_b.open(rb.trace));

    for (std::size_t i = 1; i <= 12; ++i) {
        const std::size_t at = 2090u * i / 12u;
        rwhost::Snapshot sa, sb, xa, xb;
        REQUIRE(a.snapshot_at(at, &sa));
        REQUIRE(b.snapshot_at(at, &sb));      // the other one, in between
        REQUIRE(alone_a.snapshot_at(at, &xa));
        REQUIRE(alone_b.snapshot_at(at, &xb));
        CAPTURE(at);
        CHECK(sa.state_hash == xa.state_hash);
        CHECK(sb.state_hash == xb.state_hash);
    }
}
