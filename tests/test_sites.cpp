// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// What probe points are for: a race whose whole window is computation.
//
// The firmware below loads a shared counter, adds one, and stores it, with no
// peripheral access in between. An interrupt handler increments the same
// counter. If the interrupt lands between the load and the store, its
// increment is overwritten: a lost update. This is the classic race, and it is
// exactly what the execution model cannot see, because interrupts are only
// delivered where the shim runs and the shim only runs at a peripheral access.
//
//   1. Without a probe point the window cannot be entered, so the race never
//      shows, on any seed.
//   2. With one inside the window, it shows on some seeds and not others: an
//      intermittent bug, as in the field.
//   3. A run that lost an update replays from its trace alone and loses the
//      same update.
//   4. A critical section still keeps the interrupt out, probe point or not.

#include "doctest.h"

#include <string>
#include <vector>

#include "rewind/hal.h"
#include "rewind/replay.h"
#include "rewind/trace_reader.h"
#include "rewind/trace_writer.h"
#include "sim/mcu.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

// Sized so the bug is intermittent, as it would be in the field: the window
// is one cycle of every ten in the loop, and only six bytes arrive, so a given
// seed has about even odds of getting an interrupt inside it. Make the window
// wider or send more bytes and every seed loses an update.
const u32 kIters    = 1000;
const u32 kIdlePolls = 3;
const u32 kBytes    = 6;
const u32 kSiteId   = 7;
const u32 kSeeds    = 60;
const u64 kBuildId  = 0x51735173ULL;

struct Shared {
    u32 value;
    u32 isr_calls;
    Shared() : value(0), isr_calls(0) {}
};
Shared g;

// What an ordinary receive handler does: read the status, drain the data
// register, and bump the counter the main loop is also bumping.
void isr_uart() {
    const u32 sr = rwd::mmio_read32(sim::kUart0Sr);
    if ((sr & sim::kSrRxne) != 0) {
        (void)rwd::mmio_read32(sim::kUart0Dr);
        g.value += 1u;
        ++g.isr_calls;
    }
}

enum Window { kOpen, kProbed, kMasked };

// The racy loop. The idle polls at the end of each pass are the only
// peripheral accesses, and they sit outside the load-store window, so they are
// where interrupts can land when there is no probe point.
void run(Window window, u32 site_id) {
    for (u32 i = 0; i < kIters; ++i) {
        if (window == kMasked) {
            rwd::hal_irq_disable();
        }
        const u32 tmp = g.value;               // load
        if (window != kOpen) {
            RWD_SITE(site_id);                 // an interrupt may land here
        }
        g.value = tmp + 1u;                    // store
        if (window == kMasked) {
            rwd::hal_irq_enable();
        }
        for (u32 p = 0; p < kIdlePolls; ++p) {
            rwd::mmio_read32(sim::kSystickCnt);    // idle poll
        }
    }
}

// Every increment is either the main loop's or an interrupt's; if the total
// is short, an update was lost.
u32 lost_updates() {
    return (kIters + g.isr_calls) - g.value;
}

struct Recording {
    std::vector<u8> trace;
    u32             lost;
    u32             isr_calls;
    u32             value;
};

Recording record(u64 seed, Window window) {
    sim::Mcu           mcu(seed, kBytes);
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    std::vector<u8>    block(256);

    g = Shared();
    rwd::hal_reset();
    rwd::hal_attach(mcu.ops());
    rwd::hal_set_isr(sim::kIrqUartRx, &isr_uart);
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block.data(),
                         (u32)block.size(), kBuildId, seed, rwd::kFlagRecordWrites));
    rwd::hal_mode_record(&writer);

    run(window, kSiteId);

    REQUIRE(writer.finish(rwd::hal_now()));
    REQUIRE(writer.blocks_lost() == 0u);
    rwd::hal_reset();

    Recording rec;
    rec.trace     = sink.bytes;
    rec.lost      = lost_updates();
    rec.isr_calls = g.isr_calls;
    rec.value     = g.value;
    return rec;   // the Mcu is destroyed on the way out
}

struct Replayed {
    bool        loaded;
    bool        diverged;
    std::string why;
    u32         lost;
    u32         isr_calls;
    u32         value;
    std::size_t consumed;
    std::size_t total;
};

Replayed replay(const std::vector<u8>& trace, Window window, u32 site_id) {
    Replayed out;
    rwhost::ReplayHal hal;
    out.loaded = hal.load(trace.data(), trace.size());
    REQUIRE(out.loaded);

    g = Shared();
    rwd::hal_reset();
    rwd::hal_attach(hal.ops());
    rwd::hal_mode_replay();
    rwd::hal_set_isr(sim::kIrqUartRx, &isr_uart);

    run(window, site_id);
    rwd::hal_reset();

    out.diverged  = hal.diverged();
    out.why       = hal.divergence();
    out.lost      = lost_updates();
    out.isr_calls = g.isr_calls;
    out.value     = g.value;
    out.consumed  = hal.cursor();
    out.total     = hal.event_count();
    return out;
}

std::vector<rwd::Event> decode(const std::vector<u8>& trace) {
    std::vector<rwd::Event> out;
    rwd::TraceReader        reader;
    REQUIRE(reader.open(trace.data(), (u32)trace.size()));
    rwd::Event ev;
    while (reader.next(&ev)) {
        out.push_back(ev);
    }
    CHECK(reader.ok());
    return out;
}

} // namespace

TEST_CASE("without a probe point the race cannot show, on any seed") {
    u32 interrupts = 0;
    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        const Recording rec = record(seed, kOpen);
        CHECK(rec.lost == 0u);
        interrupts += rec.isr_calls;
    }
    // The handler really did run: the sweep is not clean because nothing
    // happened.
    CHECK(interrupts > 0u);
}

TEST_CASE("a probe point inside the window makes the race intermittent") {
    u32 racy = 0;
    u32 clean = 0;
    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        const Recording rec = record(seed, kProbed);
        if (rec.lost > 0u) {
            ++racy;
        } else {
            ++clean;
        }
    }
    CAPTURE(racy);
    CAPTURE(clean);
    // Both outcomes must occur. Always racing would be a different bug, and
    // never racing would mean the probe point does nothing.
    CHECK(racy > 0u);
    CHECK(clean > 0u);
}

TEST_CASE("a run that lost an update replays from its trace and loses the same one") {
    u32 reproduced = 0;

    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        const Recording rec = record(seed, kProbed);
        if (rec.lost == 0u) {
            continue;
        }

        // From here on the trace is all there is: the Mcu is gone.
        const Replayed rep = replay(rec.trace, kProbed, kSiteId);

        CHECK_FALSE(rep.diverged);
        CHECK(rep.why.empty());
        CHECK(rep.value     == rec.value);
        CHECK(rep.isr_calls == rec.isr_calls);
        CHECK(rep.lost      == rec.lost);
        CHECK(rep.consumed  == rep.total - 1u);   // all but the trailing STOP

        // And the interrupt did land at a probe point: somewhere in the
        // trace an interrupt follows a site directly.
        const std::vector<rwd::Event> events = decode(rec.trace);
        bool landed_at_site = false;
        for (std::size_t i = 1; i < events.size(); ++i) {
            if (events[i].type == rwd::EV_IRQ_ENTER && events[i - 1].type == rwd::EV_SITE) {
                landed_at_site = true;
            }
        }
        CHECK(landed_at_site);
        ++reproduced;
    }

    CHECK(reproduced > 0u);
}

TEST_CASE("a critical section keeps the interrupt out of a probed window") {
    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        const Recording rec = record(seed, kMasked);
        CHECK(rec.lost == 0u);

        const Replayed rep = replay(rec.trace, kMasked, kSiteId);
        CHECK_FALSE(rep.diverged);
        CHECK(rep.value == rec.value);
    }
}

TEST_CASE("replaying against a build with different probe points diverges") {
    // Find a seed where the interrupt did land at the probe point.
    u64 seed = 0;
    for (u64 s = 1; s <= kSeeds && seed == 0; ++s) {
        if (record(s, kProbed).lost > 0u) {
            seed = s;
        }
    }
    REQUIRE(seed != 0);
    const Recording rec = record(seed, kProbed);

    SUBCASE("a build with no probe point") {
        const Replayed rep = replay(rec.trace, kOpen, kSiteId);
        CHECK(rep.diverged);
        CHECK(rep.why.find("trace has SITE") != std::string::npos);
    }
    SUBCASE("a build with the probe point relabelled") {
        const Replayed rep = replay(rec.trace, kProbed, kSiteId + 1u);
        CHECK(rep.diverged);
        CHECK(rep.why.find("recorded site 7") != std::string::npos);
    }
}
