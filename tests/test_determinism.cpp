// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The golden tests. Everything else in this repository exists to make these
// pass:
//
//   1. A replay reaches exactly the state the recording did.
//   2. It does so with no simulator, no hardware and no seed -- the trace
//      alone. That is what makes a trace useful when the failure happened on
//      a customer's bench and you have a file and nothing else.
//   3. A bug that is intermittent when recorded is deterministic when
//      replayed. Once.

#include "doctest.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "rewind/session.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

const u32 kSeedSweep = 200;

rwhost::RunConfig config(u64 seed, fw::Variant variant = fw::kBuggy) {
    rwhost::RunConfig cfg;
    cfg.seed    = seed;
    cfg.variant = variant;
    return cfg;
}

} // namespace

TEST_CASE("replay reproduces the recorded run exactly, over a seed sweep") {
    u32 with_bug = 0;

    for (u32 seed = 1; seed <= kSeedSweep; ++seed) {
        CAPTURE(seed);

        const rwhost::RunConfig  cfg = config(seed);
        const rwhost::RecordResult rec = rwhost::record_run(cfg);

        REQUIRE(rec.writer_ok);
        REQUIRE(rec.events > 0u);
        REQUIRE_FALSE(rec.trace.empty());

        if (rec.bug_triggered()) {
            ++with_bug;
        }

        // From here on, `rec.trace` is all we have. The Mcu that produced it
        // was destroyed when record_run() returned.
        const rwhost::ReplayResult rep = rwhost::replay_run(rec.trace, cfg.main_iters);

        REQUIRE(rep.loaded);
        CHECK(rep.error.empty());
        CHECK_FALSE(rep.diverged);
        CHECK(rep.divergence.empty());

        CHECK(rep.fw.state_hash == rec.fw.state_hash);
        CHECK(rep.fw.consumed   == rec.fw.consumed);
        CHECK(rep.fw.checksum   == rec.fw.checksum);
        CHECK(rep.fw.overflows  == rec.fw.overflows);

        // Everything but the trailing EV_STOP, which the firmware never
        // asks for: it stops on its own, at the same iteration count.
        CHECK(rep.events_consumed == rep.events_total - 1u);
    }

    // The bug must be intermittent across seeds. If it fired every time it
    // would not be the bug we are demonstrating, and if it never fired the
    // sweep would be proving nothing at all.
    CAPTURE(with_bug);
    CHECK(with_bug > 0u);
    CHECK(with_bug < kSeedSweep);
}

TEST_CASE("recording is reproducible byte for byte") {
    // Not the same claim as replay determinism: this says the simulator and
    // the writer are themselves deterministic, so a difference between two
    // traces always means a real difference in behaviour.
    for (u32 seed = 1; seed <= 20u; ++seed) {
        CAPTURE(seed);
        const rwhost::RecordResult a = rwhost::record_run(config(seed));
        const rwhost::RecordResult b = rwhost::record_run(config(seed));

        CHECK(a.trace == b.trace);
        CHECK(a.fw.state_hash == b.fw.state_hash);
        CHECK(a.tx_checksum == b.tx_checksum);
    }
}

TEST_CASE("different seeds produce different traces") {
    std::set<std::vector<u8> > traces;
    for (u32 seed = 1; seed <= 20u; ++seed) {
        traces.insert(rwhost::record_run(config(seed)).trace);
    }
    CHECK(traces.size() == 20u);
}

TEST_CASE("a trace that captured the bug reproduces it every single time") {
    // The entire pitch, in one test. Find a seed where the race fires, then
    // replay its trace repeatedly: the failure is now a fixed point.
    std::vector<u8> failing;
    u64             expected_hash = 0;
    u32             expected_consumed = 0;
    u32             tx_sent = 0;

    for (u32 seed = 1; seed <= kSeedSweep && failing.empty(); ++seed) {
        const rwhost::RecordResult rec = rwhost::record_run(config(seed));
        if (rec.bug_triggered()) {
            failing           = rec.trace;
            expected_hash     = rec.fw.state_hash;
            expected_consumed = rec.fw.consumed;
            tx_sent           = rec.tx_sent;
        }
    }

    REQUIRE_FALSE(failing.empty());
    // The signature of this bug: bytes arrived that the firmware never
    // consumed, because the drain loop cleared a counter the ISR had bumped.
    CHECK(expected_consumed < tx_sent);

    for (u32 attempt = 0; attempt < 10u; ++attempt) {
        CAPTURE(attempt);
        const rwhost::ReplayResult rep =
            rwhost::replay_run(failing, rwhost::RunConfig().main_iters);

        REQUIRE(rep.loaded);
        CHECK_FALSE(rep.diverged);
        CHECK(rep.fw.state_hash == expected_hash);
        CHECK(rep.fw.consumed   == expected_consumed);
    }
}

TEST_CASE("the fixed firmware never loses a byte, on any seed") {
    for (u32 seed = 1; seed <= kSeedSweep; ++seed) {
        CAPTURE(seed);
        const rwhost::RecordResult rec = rwhost::record_run(config(seed, fw::kFixed));

        REQUIRE(rec.writer_ok);
        CHECK(rec.fw.consumed  == rec.tx_sent);
        CHECK(rec.fw.checksum  == rec.tx_checksum);
        CHECK(rec.fw.overflows == 0u);
        CHECK_FALSE(rec.bug_triggered());

        const rwhost::ReplayResult rep = rwhost::replay_run(rec.trace, rwhost::RunConfig().main_iters);
        REQUIRE(rep.loaded);
        CHECK_FALSE(rep.diverged);
        CHECK(rep.fw.state_hash == rec.fw.state_hash);
    }
}

TEST_CASE("a trace survives a round trip through a file") {
    // The field workflow: the device writes a trace, someone emails you the
    // file, you replay it. Nothing else travels.
    const rwhost::RecordResult rec = rwhost::record_run(config(7));
    REQUIRE(rec.writer_ok);

    // A fixed name in the build directory rather than tmpnam(), which every
    // linker on earth warns about and which buys nothing here.
    const std::string path = "rewind_file_roundtrip.rwd";

    std::FILE* out = std::fopen(path.c_str(), "wb");
    REQUIRE(out != 0);
    CHECK(std::fwrite(rec.trace.data(), 1, rec.trace.size(), out) == rec.trace.size());
    std::fclose(out);

    std::vector<u8> loaded;
    std::FILE*      in = std::fopen(path.c_str(), "rb");
    REQUIRE(in != 0);
    u8 chunk[4096];
    std::size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), in)) > 0) {
        loaded.insert(loaded.end(), chunk, chunk + n);
    }
    std::fclose(in);
    std::remove(path.c_str());

    CHECK(loaded == rec.trace);

    const rwhost::ReplayResult rep = rwhost::replay_run(loaded, rwhost::RunConfig().main_iters);
    REQUIRE(rep.loaded);
    CHECK_FALSE(rep.diverged);
    CHECK(rep.fw.state_hash == rec.fw.state_hash);
}

TEST_CASE("replaying against the wrong firmware build is caught") {
    // Pick a seed where the two builds genuinely behave differently, which
    // means one where the race actually fires.
    std::vector<u8> failing;
    for (u32 seed = 1; seed <= kSeedSweep && failing.empty(); ++seed) {
        const rwhost::RecordResult rec = rwhost::record_run(config(seed, fw::kBuggy));
        if (rec.bug_triggered()) {
            failing = rec.trace;
        }
    }
    REQUIRE_FALSE(failing.empty());

    // The honest path: the header names the build, so replay_run picks it.
    const rwhost::ReplayResult right =
        rwhost::replay_run(failing, rwhost::RunConfig().main_iters);
    REQUIRE(right.loaded);
    CHECK_FALSE(right.diverged);

    // Forcing the other build. The fixed firmware does not lose the byte, so
    // it consumes one more than the recording did, makes an access the
    // recording does not have, and the mismatch is reported rather than
    // quietly producing a plausible wrong answer.
    const rwhost::ReplayResult wrong =
        rwhost::replay_run_as(failing, fw::kFixed, rwhost::RunConfig().main_iters);
    REQUIRE(wrong.loaded);
    CHECK(wrong.diverged);
    CHECK_FALSE(wrong.divergence.empty());
}

TEST_CASE("a trace from an unknown build is refused before anything runs") {
    rwhost::RecordResult rec = rwhost::record_run(config(4));
    REQUIRE(rec.writer_ok);

    // build_id sits in the header, which is outside any block CRC -- so this
    // is a well-formed trace that simply came from somewhere else.
    rwd::put_u64_le(rec.trace.data() + 8, 0xDEADBEEFDEADBEEFULL);

    const rwhost::ReplayResult rep =
        rwhost::replay_run(rec.trace, rwhost::RunConfig().main_iters);
    CHECK_FALSE(rep.loaded);
    CHECK(rep.error.find("unknown firmware build") != std::string::npos);
}

TEST_CASE("builds that behave identically on a trace are indistinguishable") {
    // A real limit of the technique, worth stating plainly rather than
    // discovering later.
    //
    // Replay detects divergence in *behaviour*. The two firmware variants
    // differ by a critical section and one arithmetic operation, and the
    // critical section touches no peripheral -- so on a seed where the race
    // never fires, both builds make exactly the same accesses in exactly the
    // same order. A cross-replay is clean, because there is genuinely
    // nothing to see.
    //
    // Nothing in the trace could catch this. That is what the build_id in
    // the header is for, and why replay_run() consults it instead of
    // trusting replay to notice.
    std::vector<u8> clean;
    for (u32 seed = 1; seed <= kSeedSweep && clean.empty(); ++seed) {
        const rwhost::RecordResult rec = rwhost::record_run(config(seed, fw::kBuggy));
        if (!rec.bug_triggered()) {
            clean = rec.trace;
        }
    }
    REQUIRE_FALSE(clean.empty());

    const rwhost::ReplayResult crossed =
        rwhost::replay_run_as(clean, fw::kFixed, rwhost::RunConfig().main_iters);
    REQUIRE(crossed.loaded);
    CHECK_FALSE(crossed.diverged);
}

TEST_CASE("running past the end of a trace is a divergence, not a guess") {
    const rwhost::RecordResult rec = rwhost::record_run(config(11));
    REQUIRE(rec.writer_ok);

    const u32 recorded_iters = rwhost::RunConfig().main_iters;
    const rwhost::ReplayResult rep = rwhost::replay_run(rec.trace, recorded_iters * 2u);

    REQUIRE(rep.loaded);
    CHECK(rep.diverged);
    CHECK(rep.divergence.find("past the end of the recording") != std::string::npos);
}

TEST_CASE("a corrupt trace is rejected before any firmware runs") {
    rwhost::RecordResult rec = rwhost::record_run(config(5));
    REQUIRE(rec.writer_ok);
    REQUIRE(rec.trace.size() > rwd::kTraceHeaderSize + 8u);

    rec.trace[rwd::kTraceHeaderSize + 6u] ^= 0x40u;

    const rwhost::ReplayResult rep = rwhost::replay_run(rec.trace, rwhost::RunConfig().main_iters);
    CHECK_FALSE(rep.loaded);
    CHECK_FALSE(rep.error.empty());
}

TEST_CASE("traces stay small enough to be worth shipping off a device") {
    const rwhost::RecordResult rec = rwhost::record_run(config(1));
    REQUIRE(rec.writer_ok);

    // Varint encoding earns its keep here: the check is bytes per event, and
    // a fixed-width encoding would be over thirteen.
    const double bytes_per_event =
        static_cast<double>(rec.trace.size()) / static_cast<double>(rec.events);
    CAPTURE(rec.trace.size());
    CAPTURE(rec.events);
    CAPTURE(bytes_per_event);
    CHECK(bytes_per_event < 9.0);
}
