// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The example firmware's third variant: the fix that is not one. It subtracts
// what it handled, as it should, but outside a critical section, so the load,
// the subtraction and the store of `pending` can be interrupted -- and none of
// them touches a peripheral, so without a probe point there is nowhere for the
// interrupt to land. test_sites.cpp shows that on a small fragment; this is
// the same thing on the firmware the CLI runs.

#include "doctest.h"

#include <string>
#include <vector>

#include "rewind/landings.h"
#include "rewind/session.h"
#include "rewind/trace_reader.h"

using rwd::u32;
using rwd::u64;

namespace {

const u32 kSeeds = 200;

rwhost::RunConfig config(u64 seed, fw::Variant variant) {
    rwhost::RunConfig cfg;
    cfg.seed    = seed;
    cfg.variant = variant;
    return cfg;
}

std::vector<rwd::Event> decode(const std::vector<rwd::u8>& trace) {
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

u32 count_type(const std::vector<rwd::Event>& events, rwd::u8 type) {
    u32 n = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].type == type) {
            ++n;
        }
    }
    return n;
}

// An interrupt delivered directly after the probe point inside the window.
bool landed_in_window(const std::vector<rwd::Event>& events) {
    for (std::size_t i = 1; i < events.size(); ++i) {
        if (events[i].type == rwd::EV_IRQ_ENTER && events[i - 1].type == rwd::EV_SITE
            && events[i - 1].addr == fw::kSitePendingRmw) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("the unguarded variant is an intermittent bug") {
    u32 failing = 0;
    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        if (rwhost::record_run(config(seed, fw::kUnguarded)).bug_triggered()) {
            ++failing;
        }
    }
    CAPTURE(failing);
    CHECK(failing > 0u);       // the probe point lets the race happen at all
    CHECK(failing < kSeeds);   // and it is not every run
}

TEST_CASE("every unguarded run that fails had an interrupt land in the window") {
    // The only way this variant loses a byte is an interrupt between the load
    // and the store of `pending`, and the only crossing in that window is the
    // probe point. So a failure and a site-then-interrupt in the trace must
    // come together.
    u32 checked = 0;
    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        const rwhost::RecordResult rec = rwhost::record_run(config(seed, fw::kUnguarded));
        if (!rec.bug_triggered()) {
            continue;
        }
        CHECK(landed_in_window(decode(rec.trace)));
        ++checked;
    }
    CHECK(checked > 0u);
}

TEST_CASE("an unguarded failure replays exactly from its trace") {
    u32 replayed = 0;
    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        const rwhost::RunConfig    cfg = config(seed, fw::kUnguarded);
        const rwhost::RecordResult rec = rwhost::record_run(cfg);
        REQUIRE(rec.writer_ok);

        const rwhost::ReplayResult rep = rwhost::replay_run(rec.trace, cfg.main_iters);
        REQUIRE(rep.loaded);
        CHECK_FALSE(rep.diverged);
        CHECK(rep.fw.state_hash == rec.fw.state_hash);
        CHECK(rep.fw.consumed   == rec.fw.consumed);
        CHECK(rep.fw.checksum   == rec.fw.checksum);
        if (rec.bug_triggered()) {
            ++replayed;
        }
    }
    CHECK(replayed > 0u);
}

TEST_CASE("only the unguarded variant crosses probe points") {
    // Adding the probe point must not move the other variants: their traces
    // carry no site, so their timing and every number quoted for them stand.
    for (u32 seed = 1; seed <= 20; ++seed) {
        CAPTURE(seed);
        CHECK(count_type(decode(rwhost::record_run(config(seed, fw::kBuggy)).trace), rwd::EV_SITE) == 0u);
        CHECK(count_type(decode(rwhost::record_run(config(seed, fw::kFixed)).trace), rwd::EV_SITE) == 0u);
        CHECK(count_type(decode(rwhost::record_run(config(seed, fw::kUnguarded)).trace), rwd::EV_SITE) > 0u);
    }
}

TEST_CASE("replaying an unguarded trace as another variant diverges") {
    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        const rwhost::RunConfig    cfg = config(seed, fw::kUnguarded);
        const rwhost::RecordResult rec = rwhost::record_run(cfg);
        if (!rec.bug_triggered()) {
            continue;
        }
        // The other variants never cross the probe point the trace recorded.
        const rwhost::ReplayResult rep =
            rwhost::replay_run_as(rec.trace, fw::kFixed, cfg.main_iters);
        CHECK(rep.diverged);
        return;
    }
    FAIL("no failing seed found");
}

TEST_CASE("build ids name all three variants") {
    const fw::Variant all[3] = { fw::kBuggy, fw::kFixed, fw::kUnguarded };
    for (int i = 0; i < 3; ++i) {
        fw::Variant back = fw::kBuggy;
        REQUIRE(rwhost::variant_for_build_id(rwhost::build_id_for(all[i]), &back));
        CHECK(back == all[i]);
    }
    CHECK(rwhost::build_id_for(fw::kBuggy) != rwhost::build_id_for(fw::kUnguarded));
    CHECK(rwhost::build_id_for(fw::kFixed) != rwhost::build_id_for(fw::kUnguarded));
}

// -- locating the landing in a trace that does not say (kFlagNoSites) --

namespace {

rwhost::RunConfig bare_config(u64 seed, fw::Variant variant) {
    rwhost::RunConfig cfg = config(seed, variant);
    cfg.flags = (rwd::u16)(cfg.flags | rwd::kFlagNoSites);
    return cfg;
}

} // namespace

TEST_CASE("recording the example firmware without its sites changes nothing but the trace") {
    for (u32 seed = 1; seed <= 30; ++seed) {
        CAPTURE(seed);
        const rwhost::RecordResult full = rwhost::record_run(config(seed, fw::kUnguarded));
        const rwhost::RecordResult bare = rwhost::record_run(bare_config(seed, fw::kUnguarded));

        CHECK(bare.fw.state_hash == full.fw.state_hash);
        CHECK(bare.fw.consumed   == full.fw.consumed);
        CHECK(bare.cycles        == full.cycles);
        CHECK(count_type(decode(bare.trace), rwd::EV_SITE) == 0u);
        CHECK(count_type(decode(full.trace), rwd::EV_SITE) > 0u);
    }
}

TEST_CASE("locate reproduces an unguarded failure from a trace without sites") {
    u32 failing = 0;
    u32 naive_wrong = 0;

    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        const rwhost::RunConfig    cfg  = bare_config(seed, fw::kUnguarded);
        const rwhost::RecordResult bare = rwhost::record_run(cfg);
        if (!bare.bug_triggered()) {
            continue;
        }
        ++failing;

        // What replay alone does with it: usually wrong, and it says so.
        const rwhost::ReplayResult naive = rwhost::replay_run(bare.trace, cfg.main_iters);
        REQUIRE(naive.loaded);
        CHECK_FALSE(naive.sites_recorded);
        if (naive.diverged) {
            ++naive_wrong;
        }

        const rwhost::LocateReport rep = rwhost::locate_run(bare.trace, cfg.main_iters);
        REQUIRE(rep.loaded);
        REQUIRE(rep.located.applicable);
        CHECK_FALSE(rep.located.final_diverged);
        CHECK(rep.located.unresolved() == 0u);
        CHECK_FALSE(rep.located.gave_up);

        // Reproduces the recording, observable for observable.
        CHECK(rep.fw.state_hash == bare.fw.state_hash);
        CHECK(rep.fw.consumed   == bare.fw.consumed);
        CHECK(rep.fw.checksum   == bare.fw.checksum);

        // And exactly one placement is not "right after the crossing": the
        // interrupt that landed in the window.
        u32 elsewhere = 0;
        for (std::size_t i = 0; i < rep.located.choices.size(); ++i) {
            if (rep.located.choices[i].probes != 0u) {
                ++elsewhere;
            }
        }
        CHECK(elsewhere >= 1u);
    }
    CHECK(failing > 0u);
    CHECK(naive_wrong > 0u);   // the problem locate solves is real
}

TEST_CASE("locate agrees with the clock about which interrupts landed away from a crossing") {
    // Two independent routes to the same answer: the slack in the trace says
    // which interrupts were taken away from an MMIO access, and the search
    // says which needed a placement other than "right after the crossing".
    u32 compared = 0;
    for (u32 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        const rwhost::RunConfig    cfg  = bare_config(seed, fw::kUnguarded);
        const rwhost::RecordResult bare = rwhost::record_run(cfg);
        if (!bare.bug_triggered()) {
            continue;
        }

        std::vector<rwhost::Landing> slack = rwhost::find_landings(decode(bare.trace));
        const rwhost::LocateReport   rep   = rwhost::locate_run(bare.trace, cfg.main_iters);
        REQUIRE(rep.located.applicable);
        REQUIRE(rep.located.choices.size() == slack.size());

        for (std::size_t i = 0; i < slack.size(); ++i) {
            CAPTURE(i);
            if (rep.located.choices[i].probes != 0u) {
                // Placed past a probe point, so it was taken after the crossing.
                CHECK(slack[i].slack > 0u);
            }
        }
        ++compared;
    }
    CHECK(compared > 0u);
}

TEST_CASE("locate has nothing to do on the variants that cross no probe point") {
    const fw::Variant variants[2] = { fw::kBuggy, fw::kFixed };
    for (int v = 0; v < 2; ++v) {
        for (u32 seed = 1; seed <= 10; ++seed) {
            CAPTURE(seed);
            const rwhost::RunConfig    cfg  = bare_config(seed, variants[v]);
            const rwhost::RecordResult bare = rwhost::record_run(cfg);

            const rwhost::LocateReport rep = rwhost::locate_run(bare.trace, cfg.main_iters);
            REQUIRE(rep.located.applicable);
            CHECK_FALSE(rep.located.final_diverged);
            CHECK(rep.located.unresolved() == 0u);
            for (std::size_t i = 0; i < rep.located.choices.size(); ++i) {
                CHECK(rep.located.choices[i].probes == 0u);
                CHECK_FALSE(rep.located.choices[i].ambiguous);
            }
            CHECK(rep.fw.state_hash == bare.fw.state_hash);
        }
    }
}

TEST_CASE("locate refuses a trace that kept its sites, and one it cannot read") {
    const rwhost::RunConfig    cfg  = config(5, fw::kUnguarded);
    const rwhost::RecordResult full = rwhost::record_run(cfg);
    const rwhost::LocateReport kept = rwhost::locate_run(full.trace, cfg.main_iters);
    REQUIRE(kept.loaded);
    CHECK_FALSE(kept.located.applicable);
    CHECK(kept.located.why.find("recorded its probe points") != std::string::npos);

    const std::vector<rwd::u8> junk(64, 0x55);
    const rwhost::LocateReport bad = rwhost::locate_run(junk, cfg.main_iters);
    CHECK_FALSE(bad.loaded);
    CHECK_FALSE(bad.error.empty());
}
