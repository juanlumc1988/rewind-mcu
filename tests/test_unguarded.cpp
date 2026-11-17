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

#include <vector>

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
