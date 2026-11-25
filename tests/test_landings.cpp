// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Slack: how long after the last crossing an interrupt was taken. The traces
// here are built by hand, because the simulator cannot produce the case that
// matters -- it only ever interrupts right after a crossing -- and a hardware
// trace is what the analysis is for.

#include "doctest.h"

#include <vector>

#include "rewind/landings.h"
#include "rewind/session.h"
#include "rewind/trace_reader.h"

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
    return e;
}

} // namespace

TEST_CASE("no interrupts, no landings") {
    std::vector<rwd::Event> events;
    events.push_back(ev(rwd::EV_MMIO_READ, 5, 0x40000000u, 1));
    events.push_back(ev(rwd::EV_STOP, 9, 0));

    CHECK(rwhost::find_landings(events).empty());
    const rwhost::LandingStats s = rwhost::summarize_landings(rwhost::find_landings(events), 64);
    CHECK(s.count == 0u);
    CHECK(s.max_slack == 0u);
    CHECK(s.median_slack == 0u);
}

TEST_CASE("an interrupt right after a crossing has no slack") {
    // What the simulator produces: delivered the moment the access returns.
    std::vector<rwd::Event> events;
    events.push_back(ev(rwd::EV_MMIO_READ, 10, 0x40001000u, 3));
    events.push_back(ev(rwd::EV_IRQ_ENTER, 10, 1));
    events.push_back(ev(rwd::EV_MMIO_READ, 13, 0x40001004u, 7));

    const std::vector<rwhost::Landing> l = rwhost::find_landings(events);
    REQUIRE(l.size() == 1u);
    CHECK(l[0].event == 1u);
    CHECK(l[0].slack == 0u);
    CHECK(l[0].vector == 1u);
    CHECK_FALSE(l[0].has_pc);
    CHECK(l[0].has_crossing);
    CHECK(l[0].crossing_event == 0u);
    CHECK(l[0].crossing_type == rwd::EV_MMIO_READ);
    CHECK(l[0].crossing_addr == 0x40001000u);
}

TEST_CASE("slack is the clock distance to the last crossing, whatever its kind") {
    std::vector<rwd::Event> events;
    events.push_back(ev(rwd::EV_MMIO_READ,  100, 0x40000000u, 1));
    events.push_back(ev(rwd::EV_IRQ_AT,     130, 3, 0x08001234u));   // after a read
    events.push_back(ev(rwd::EV_MMIO_WRITE, 150, 0x40002000u, 5));
    events.push_back(ev(rwd::EV_IRQ_AT,     160, 3, 0x08001240u));   // after a write
    events.push_back(ev(rwd::EV_SITE,       200, 7));
    events.push_back(ev(rwd::EV_IRQ_AT,     212, 2, 0x08002000u));   // after a site

    const std::vector<rwhost::Landing> l = rwhost::find_landings(events);
    REQUIRE(l.size() == 3u);

    CHECK(l[0].slack == 30u);
    CHECK(l[0].has_pc);
    CHECK(l[0].pc == 0x08001234u);
    CHECK(l[0].crossing_type == rwd::EV_MMIO_READ);

    CHECK(l[1].slack == 10u);
    CHECK(l[1].crossing_type == rwd::EV_MMIO_WRITE);
    CHECK(l[1].crossing_addr == 0x40002000u);

    CHECK(l[2].slack == 12u);
    CHECK(l[2].crossing_type == rwd::EV_SITE);
    CHECK(l[2].crossing_addr == 7u);          // the site label
    CHECK(l[2].vector == 2u);
}

TEST_CASE("a DMA read is a crossing") {
    // Replay delivers an interrupt right after a DMA read, as after any access.
    std::vector<rwd::Event> events;
    events.push_back(ev(rwd::EV_MMIO_READ, 100, 0x40000000u, 1));
    events.push_back(ev(rwd::EV_DMA, 150, 2, 8));            // region 2, offset 8
    events.push_back(ev(rwd::EV_IRQ_AT, 170, 1, 0x08000100u));

    const std::vector<rwhost::Landing> l = rwhost::find_landings(events);
    REQUIRE(l.size() == 1u);
    CHECK(l[0].crossing_event == 1u);
    CHECK(l[0].crossing_type == rwd::EV_DMA);
    CHECK(l[0].crossing_addr == 2u);       // the region
    CHECK(l[0].slack == 20u);              // from the DMA read, not the register read
}

TEST_CASE("an interrupt is not a crossing") {
    // Two interrupts with no access between them are both measured from the
    // access before: that is where replay delivers each of them.
    std::vector<rwd::Event> events;
    events.push_back(ev(rwd::EV_MMIO_READ, 100, 0x40000000u, 1));
    events.push_back(ev(rwd::EV_IRQ_AT,    120, 1, 0x08000100u));
    events.push_back(ev(rwd::EV_IRQ_AT,    150, 2, 0x08000200u));

    const std::vector<rwhost::Landing> l = rwhost::find_landings(events);
    REQUIRE(l.size() == 2u);
    CHECK(l[0].slack == 20u);
    CHECK(l[1].slack == 50u);
    CHECK(l[1].crossing_event == 0u);
}

TEST_CASE("an interrupt before any crossing is measured from the start") {
    std::vector<rwd::Event> events;
    events.push_back(ev(rwd::EV_IRQ_AT, 400, 1, 0x08000000u));

    const std::vector<rwhost::Landing> l = rwhost::find_landings(events);
    REQUIRE(l.size() == 1u);
    CHECK_FALSE(l[0].has_crossing);
    CHECK(l[0].slack == 400u);
}

TEST_CASE("a timestamp that went backwards does not underflow") {
    // The writer refuses one, but a trace is input.
    std::vector<rwd::Event> events;
    events.push_back(ev(rwd::EV_MMIO_READ, 500, 0x40000000u, 1));
    events.push_back(ev(rwd::EV_IRQ_ENTER, 100, 1));

    const std::vector<rwhost::Landing> l = rwhost::find_landings(events);
    REQUIRE(l.size() == 1u);
    CHECK(l[0].slack == 0u);
}

TEST_CASE("the summary counts what is over the threshold") {
    std::vector<rwd::Event> events;
    u64 t = 0;
    const u64 slacks[5] = { 0, 4, 12, 200, 900 };
    for (int i = 0; i < 5; ++i) {
        events.push_back(ev(rwd::EV_MMIO_READ, t, 0x40000000u, 1));
        events.push_back(ev(i % 2 ? rwd::EV_IRQ_AT : rwd::EV_IRQ_ENTER, t + slacks[i], 1, 0x08000000u));
        t += 1000;
    }

    const rwhost::LandingStats s = rwhost::summarize_landings(rwhost::find_landings(events), 64);
    CHECK(s.count == 5u);
    CHECK(s.over == 2u);              // 200 and 900
    CHECK(s.max_slack == 900u);
    CHECK(s.median_slack == 12u);
    CHECK(s.with_pc == 2u);

    // Strictly greater: a threshold equal to the slack does not count.
    CHECK(rwhost::summarize_landings(rwhost::find_landings(events), 200).over == 1u);
    CHECK(rwhost::summarize_landings(rwhost::find_landings(events), 900).over == 0u);
}

TEST_CASE("the median is an observed value, not an average") {
    std::vector<rwd::Event> events;
    const u64 slacks[4] = { 10, 20, 30, 40 };
    u64 t = 0;
    for (int i = 0; i < 4; ++i) {
        events.push_back(ev(rwd::EV_MMIO_READ, t, 0x40000000u, 1));
        events.push_back(ev(rwd::EV_IRQ_ENTER, t + slacks[i], 1));
        t += 1000;
    }
    CHECK(rwhost::summarize_landings(rwhost::find_landings(events), 0).median_slack == 20u);
}

TEST_CASE("a simulator trace has no slack anywhere") {
    // The simulator interrupts only right after a crossing, probe points
    // included: this is the shape every simulated trace has.
    const fw::Variant variants[3] = { fw::kBuggy, fw::kFixed, fw::kUnguarded };
    for (int v = 0; v < 3; ++v) {
        for (u32 seed = 1; seed <= 10; ++seed) {
            rwhost::RunConfig cfg;
            cfg.seed    = seed;
            cfg.variant = variants[v];
            const rwhost::RecordResult rec = rwhost::record_run(cfg);

            std::vector<rwd::Event> events;
            rwd::TraceReader        reader;
            REQUIRE(reader.open(rec.trace.data(), (u32)rec.trace.size()));
            rwd::Event e;
            while (reader.next(&e)) {
                events.push_back(e);
            }

            const std::vector<rwhost::Landing> l = rwhost::find_landings(events);
            CHECK(l.size() == rec.tx_sent);   // one interrupt per byte
            for (std::size_t i = 0; i < l.size(); ++i) {
                CHECK(l[i].slack == 0u);
                CHECK_FALSE(l[i].has_pc);
            }
        }
    }
}
