// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Finding where an interrupt landed when the trace does not say.
//
// The ground truth comes from recording the same simulator run twice: once
// keeping the probe points, which says exactly where every interrupt landed,
// and once without them (kFlagNoSites), which is the shape a trace from a part
// has. The search sees only the second, and is checked against the first.
//
// The firmware is a load-add-store of a shared counter that an interrupt
// handler also increments, followed by a write of the counter to a register --
// the observable that lets a wrong landing be told from a right one:
//
//     load       tmp = value
//     site A     <- an interrupt here loses an update
//     store      value = tmp + 1
//     site B
//     write      GPIO = value          (recorded; replay checks it)

#include "doctest.h"

#include <string>
#include <vector>

#include "rewind/hal.h"
#include "rewind/locate.h"
#include "rewind/replay.h"
#include "rewind/trace_reader.h"
#include "rewind/trace_writer.h"
#include "sim/mcu.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

const u32 kIters     = 600;
const u32 kIdlePolls = 3;
const u32 kBytes     = 6;
const u32 kSeeds     = 40;
const u32 kSiteA     = 11;
const u32 kSiteB     = 12;

struct Build {
    bool site_a;
    bool site_b;
    Build(bool a, bool b) : site_a(a), site_b(b) {}
};

struct Shared {
    u32 value;
    u32 isr_calls;
    Shared() : value(0), isr_calls(0) {}
};
Shared g;

void isr_uart() {
    const u32 sr = rwd::mmio_read32(sim::kUart0Sr);
    if ((sr & sim::kSrRxne) != 0) {
        (void)rwd::mmio_read32(sim::kUart0Dr);
        g.value += 1u;
        ++g.isr_calls;
    }
}

void run_fragment(const Build& b) {
    for (u32 i = 0; i < kIters; ++i) {
        const u32 tmp = g.value;                             // load
        if (b.site_a) { RWD_SITE(kSiteA); }
        g.value = tmp + 1u;                                  // store
        if (b.site_b) { RWD_SITE(kSiteB); }
        rwd::mmio_write32(sim::kGpio0Odr, g.value);          // observable
        for (u32 p = 0; p < kIdlePolls; ++p) {
            rwd::mmio_read32(sim::kSystickCnt);
        }
    }
}

struct Recording {
    std::vector<u8>         trace;
    std::vector<rwd::Event> events;
    u32                     value;
    u32                     isr_calls;
};

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

// Records the fragment on the simulator. `keep_sites` false sets kFlagNoSites:
// the simulator still interrupts at the probe points, but the trace does not
// say so -- which is what a part would give.
Recording record(u64 seed, bool keep_sites, u32 bytes = kBytes) {
    sim::Mcu           mcu(seed, bytes);
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    std::vector<u8>    block(256);

    g = Shared();
    rwd::hal_reset();
    rwd::hal_attach(mcu.ops());
    rwd::hal_set_isr(sim::kIrqUartRx, &isr_uart);
    rwd::u16 flags = rwd::kFlagRecordWrites;
    if (!keep_sites) {
        flags = (rwd::u16)(flags | rwd::kFlagNoSites);
    }
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block.data(),
                         (u32)block.size(), 0x10CA7E, seed, flags));
    rwd::hal_mode_record(&writer);

    run_fragment(Build(true, true));

    REQUIRE(writer.finish(rwd::hal_now()));
    REQUIRE(writer.blocks_lost() == 0u);
    rwd::hal_reset();

    Recording rec;
    rec.trace     = sink.bytes;
    rec.events    = decode(sink.bytes);
    rec.value     = g.value;
    rec.isr_calls = g.isr_calls;
    return rec;
}

// For each interrupt in a trace that kept its sites: how many probe points
// the firmware had passed since the last crossing when it landed. That is the
// answer the search is trying to recover.
std::vector<u32> truth(const std::vector<rwd::Event>& full) {
    std::vector<u32> out;
    for (std::size_t i = 0; i < full.size(); ++i) {
        if (full[i].type != rwd::EV_IRQ_ENTER) {
            continue;
        }
        u32 k = 0;
        for (std::size_t j = i; j > 0 && full[j - 1].type == rwd::EV_SITE; --j) {
            ++k;
        }
        out.push_back(k);
    }
    return out;
}

void run_replay(void* ctx, rwhost::ReplayHal* hal) {
    const Build* b = static_cast<const Build*>(ctx);
    g = Shared();
    rwd::hal_reset();
    rwd::hal_attach(hal->ops());
    rwd::hal_mode_replay();
    rwd::hal_set_isr(sim::kIrqUartRx, &isr_uart);
    run_fragment(*b);
    rwd::hal_reset();
}

bool has_landing_in_window(const std::vector<u32>& t) {
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == 1u) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("recording without sites does not change the run") {
    // The simulator interrupts at the probe points either way; only whether
    // the trace says so differs. So the two traces are the same but for the
    // site events and the flag.
    for (u32 seed = 1; seed <= 10; ++seed) {
        CAPTURE(seed);
        const Recording full = record(seed, true);
        const Recording bare = record(seed, false);

        CHECK(bare.value == full.value);
        CHECK(bare.isr_calls == full.isr_calls);

        std::vector<rwd::Event> without_sites;
        for (std::size_t i = 0; i < full.events.size(); ++i) {
            if (full.events[i].type != rwd::EV_SITE) {
                without_sites.push_back(full.events[i]);
            }
        }
        REQUIRE(bare.events.size() == without_sites.size());
        for (std::size_t i = 0; i < bare.events.size(); ++i) {
            CHECK(bare.events[i].type  == without_sites[i].type);
            CHECK(bare.events[i].ts    == without_sites[i].ts);
            CHECK(bare.events[i].addr  == without_sites[i].addr);
            CHECK(bare.events[i].value == without_sites[i].value);
        }
    }
}

TEST_CASE("replay alone puts an interrupt after the crossing, which can be wrong") {
    u32 landed_in_window = 0;
    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        const Recording full = record(seed, true);
        if (!has_landing_in_window(truth(full.events))) {
            continue;
        }
        ++landed_in_window;

        const Recording bare = record(seed, false);
        rwhost::ReplayHal hal;
        REQUIRE(hal.load(bare.trace.data(), bare.trace.size()));
        REQUIRE_FALSE(hal.sites_recorded());
        const Build b(true, true);
        run_replay(const_cast<Build*>(&b), &hal);

        // The window's lost update shows in the write that follows.
        CHECK(hal.diverged());
    }
    CHECK(landed_in_window > 0u);
}

TEST_CASE("locate recovers where each interrupt landed") {
    u32 located      = 0;
    u32 in_window    = 0;
    u32 isolated_in_window = 0;
    u32 ambiguous    = 0;

    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        const Recording full = record(seed, true);
        const std::vector<u32> t = truth(full.events);
        const Recording bare = record(seed, false);

        const Build b(true, true);
        const rwhost::LocateResult r =
            rwhost::locate_landings(bare.trace, &run_replay, const_cast<Build*>(&b));

        REQUIRE(r.applicable);
        REQUIRE(r.choices.size() == t.size());
        CHECK(r.unresolved() == 0u);

        // The located replay is the recording, event for event, and leaves
        // the firmware in the state the recording ended in.
        CHECK_FALSE(r.final_diverged);
        CHECK(g.value == bare.value);
        CHECK(g.isr_calls == bare.isr_calls);

        for (std::size_t i = 0; i < t.size(); ++i) {
            CAPTURE(i);
            const rwhost::LandingChoice& c = r.choices[i];
            // Either it found the landing, or the landing is one the trace
            // cannot tell from another and the search says so.
            CHECK((c.probes == t[i] || c.ambiguous));

            // Two interrupts close together can swap without the trace
            // noticing -- which one lost an update is not observable, only
            // that two were lost -- so for those, ambiguity is the right
            // answer. For an interrupt with nothing near it, the placement
            // that loses the update is the only one that fits the write that
            // follows.
            const std::size_t near_gap = 12;
            const bool isolated =
                (i == 0 || c.event - r.choices[i - 1].event > near_gap)
                && (i + 1 == t.size() || r.choices[i + 1].event - c.event > near_gap);
            if (t[i] == 1u && isolated) {
                CHECK(c.probes == 1u);
                CHECK_FALSE(c.ambiguous);
                ++isolated_in_window;
            }
            if (t[i] == 1u) {
                ++in_window;
            }
            if (c.ambiguous) {
                ++ambiguous;
            }
        }
        ++located;
    }
    CHECK(located == kSeeds);
    CHECK(in_window > 0u);
    CHECK(isolated_in_window > 0u);   // the strong claim was actually exercised
    // Landing right after a crossing and landing after the store give the same
    // write, so the trace cannot tell them apart; that must be reported, not
    // hidden.
    CHECK(ambiguous > 0u);
}

TEST_CASE("a trace that kept its sites has nothing to search") {
    const Recording full = record(3, true);
    const Build     b(true, true);
    const rwhost::LocateResult r =
        rwhost::locate_landings(full.trace, &run_replay, const_cast<Build*>(&b));

    CHECK_FALSE(r.applicable);
    CHECK(r.why.find("recorded its probe points") != std::string::npos);
    CHECK(r.choices.empty());
    CHECK(r.runs == 0u);
}

TEST_CASE("a trace nobody could read is not searched") {
    std::vector<u8> junk(40, 0xAB);
    const Build     b(true, true);
    const rwhost::LocateResult r =
        rwhost::locate_landings(junk, &run_replay, const_cast<Build*>(&b));
    CHECK_FALSE(r.applicable);
    CHECK_FALSE(r.why.empty());
}

TEST_CASE("when the host build has no probe where it landed, it says so") {
    // Find a seed where an interrupt landed in the window, then replay with a
    // build that has no probe point there. The only candidates left are before
    // the load and after the store, and neither fits.
    bool exercised = false;
    for (u32 seed = 1; seed <= kSeeds && !exercised; ++seed) {
        const Recording full = record(seed, true);
        if (!has_landing_in_window(truth(full.events))) {
            continue;
        }
        exercised = true;

        const Recording bare = record(seed, false);
        const Build     no_window(false, true);
        const rwhost::LocateResult r =
            rwhost::locate_landings(bare.trace, &run_replay, const_cast<Build*>(&no_window));

        REQUIRE(r.applicable);
        CHECK(r.unresolved() > 0u);
        CHECK(r.final_diverged);
        CHECK_FALSE(r.final_divergence.empty());
    }
    CHECK(exercised);
}

TEST_CASE("a candidate cap is reported when it could have cut the search short") {
    const Recording full = record(3, true);
    const Recording bare = record(3, false);
    const Build     b(true, true);

    rwhost::LocateOptions tight;
    tight.max_probes = 1;
    const rwhost::LocateResult capped =
        rwhost::locate_landings(bare.trace, &run_replay, const_cast<Build*>(&b), tight);
    bool any_truncated = false;
    for (std::size_t i = 0; i < capped.choices.size(); ++i) {
        any_truncated = any_truncated || capped.choices[i].truncated;
    }
    CHECK(any_truncated);

    const rwhost::LocateResult roomy =
        rwhost::locate_landings(bare.trace, &run_replay, const_cast<Build*>(&b));
    for (std::size_t i = 0; i < roomy.choices.size(); ++i) {
        CHECK_FALSE(roomy.choices[i].truncated);
    }
    (void)full;
}

TEST_CASE("a run with no interrupts has nothing to locate") {
    const Recording bare = record(1, false, 0);   // the UART delivers nothing
    const Build     b(true, true);
    const rwhost::LocateResult r =
        rwhost::locate_landings(bare.trace, &run_replay, const_cast<Build*>(&b));

    CHECK(r.applicable);
    CHECK(r.choices.empty());
    CHECK_FALSE(r.final_diverged);
    CHECK(r.runs == 1u);   // just the replay itself
}
