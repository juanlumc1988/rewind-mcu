// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The device-side path end to end: firmware records through a ring buffer
// and a trickling transport, exactly as it would on hardware, and the result
// has to be a trace that replays.

#include "doctest.h"

#include <string>
#include <vector>

#include "fw/rx_pump.h"
#include "rewind/recorder.h"
#include "rewind/session.h"
#include "rewind/trace_reader.h"
#include "sim/mcu.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

// Runs the example firmware under a Recorder, interleaving drains with
// execution the way a main loop would. Thin wrapper over the same
// record_buffered() the CLI uses, so the two cannot drift apart.
struct Outcome {
    std::vector<u8> trace;
    u32  blocks_lost = 0;
    u32  ring_drops  = 0;
    u32  high_water  = 0;
    u32  events      = 0;
    bool healthy     = false;
    u64  state_hash  = 0;
    u32  consumed    = 0;
};

Outcome record_through_ring(u64 seed, u32 ring_capacity, u32 drain_budget,
                            u32 link_per_call = 0xFFFFFFFFu) {
    rwhost::BufferedConfig cfg;
    cfg.run.seed       = seed;
    cfg.run.variant    = fw::kBuggy;
    cfg.ring_capacity  = ring_capacity;
    cfg.drain_budget   = drain_budget;
    cfg.link_per_call  = link_per_call;

    const rwhost::BufferedResult got = rwhost::record_buffered(cfg);
    REQUIRE(got.began);

    Outcome out;
    out.trace       = got.trace;
    out.blocks_lost = got.blocks_lost;
    out.ring_drops  = got.ring_drops;
    out.high_water  = got.high_water;
    out.events      = got.events;
    out.healthy     = got.healthy;
    out.state_hash  = got.fw.state_hash;
    out.consumed    = got.fw.consumed;
    return out;
}

} // namespace

TEST_CASE("a trace recorded through the ring buffer replays") {
    // Generous ring, generous drain budget: nothing should be lost, and the
    // result must be indistinguishable from recording straight to memory.
    const Outcome out = record_through_ring(7, 8192, 4096);

    CHECK(out.healthy);
    CHECK(out.blocks_lost == 0u);
    CHECK(out.ring_drops == 0u);
    REQUIRE_FALSE(out.trace.empty());

    const rwhost::ReplayResult rep = rwhost::replay_run(out.trace, 6000);
    REQUIRE(rep.loaded);
    CHECK(rep.error.empty());
    CHECK_FALSE(rep.diverged);
    CHECK(rep.fw.state_hash == out.state_hash);
    CHECK(rep.fw.consumed == out.consumed);
}

TEST_CASE("the ring path produces the same bytes as recording to memory") {
    // Whether the trace goes through a ring buffer or straight into a vector
    // must not change a single byte of it.
    rwhost::RunConfig cfg;
    cfg.seed    = 7;
    cfg.variant = fw::kBuggy;
    const rwhost::RecordResult direct = rwhost::record_run(cfg);
    REQUIRE(direct.writer_ok);

    const Outcome buffered = record_through_ring(7, 8192, 4096);
    CHECK(buffered.trace == direct.trace);
}

TEST_CASE("a transport that dribbles still delivers the whole trace") {
    // A UART with a four-deep FIFO accepts four bytes and asks you to come
    // back later. drain() has to cope without losing or duplicating any.
    const Outcome out = record_through_ring(7, 8192, 4096, 4);

    CHECK(out.healthy);
    const Outcome reference = record_through_ring(7, 8192, 4096);
    CHECK(out.trace == reference.trace);

    const rwhost::ReplayResult rep = rwhost::replay_run(out.trace, 6000);
    REQUIRE(rep.loaded);
    CHECK_FALSE(rep.diverged);
}

TEST_CASE("an undersized ring loses blocks, and says so") {
    // The failure this whole mechanism exists for: recording faster than the
    // link can carry, on a buffer too small to absorb the difference.
    const Outcome out = record_through_ring(7, 512, 64);

    CHECK_FALSE(out.healthy);
    CHECK(out.blocks_lost > 0u);
    CHECK(out.ring_drops > 0u);
    REQUIRE_FALSE(out.trace.empty());

    // Crucially, the surviving blocks are not garbage -- each one still has
    // a valid CRC. Without sequence numbers this trace would parse cleanly
    // and replay would diverge somewhere downstream for reasons that look
    // like a firmware bug and are not.
    rwd::TraceReader reader;
    REQUIRE(reader.open(out.trace.data(), (u32)out.trace.size()));

    rwd::Event ev;
    u32        recovered = 0;
    while (reader.next(&ev)) {
        ++recovered;
    }

    CHECK(recovered > 0u);            // the blocks before the hole are usable
    CHECK_FALSE(reader.ok());         // and the hole is reported, not hidden
    CHECK(reader.blocks_lost() > 0u);
    CHECK(std::string(reader.error()).find("gap in trace") != std::string::npos);
}

TEST_CASE("replay refuses a trace with a hole rather than diverging later") {
    const Outcome out = record_through_ring(7, 512, 64);
    REQUIRE(out.blocks_lost > 0u);

    const rwhost::ReplayResult rep = rwhost::replay_run(out.trace, 6000);

    // Rejected at load, before a single instruction of firmware runs. The
    // alternative -- replaying up to the hole and then diverging -- would
    // point the investigation at the firmware instead of at the buffer.
    CHECK_FALSE(rep.loaded);
    CHECK(rep.error.find("gap in trace") != std::string::npos);
}

TEST_CASE("high water mark shows how much ring was actually needed") {
    // The number to size a buffer by. With a generous ring and frequent
    // drains it should sit well below capacity.
    const Outcome roomy = record_through_ring(7, 8192, 4096);
    CHECK(roomy.high_water > 0u);
    CHECK(roomy.high_water < 8192u);

    // Draining less often needs more buffer for the same traffic.
    const Outcome starved = record_through_ring(7, 8192, 256);
    CHECK(starved.high_water > roomy.high_water);
}

TEST_CASE("recorder rejects buffers it cannot work with") {
    rwd::Recorder recorder;
    u8            ring[256];
    u8            block[rwd::kMinBlockBuffer];

    CHECK_FALSE(recorder.begin(ring, 100, block, sizeof(block), 1, 2, 0));
    CHECK_FALSE(recorder.begin(ring, 256, block, rwd::kMinBlockBuffer - 1u, 1, 2, 0));
    CHECK(recorder.begin(ring, 256, block, sizeof(block), 1, 2, 0));
    rwd::hal_reset();
}

TEST_CASE("drain is safe when there is nothing to send") {
    // Exercises Recorder::drain directly, below record_buffered().
    struct Sink {
        std::vector<u8> got;
        static u32 write(void* ctx, const u8* data, u32 len) {
            Sink* self = static_cast<Sink*>(ctx);
            self->got.insert(self->got.end(), data, data + len);
            return len;
        }
    };

    rwd::Recorder recorder;
    u8            ring[256];
    u8            block[rwd::kMinBlockBuffer];
    Sink          sink;

    REQUIRE(recorder.begin(ring, 256, block, sizeof(block), 1, 2, 0));

    // The header is already queued; drain it, then drain an empty ring.
    CHECK(recorder.drain(&Sink::write, &sink, 4096) == rwd::kTraceHeaderSize);
    CHECK(sink.got.size() == rwd::kTraceHeaderSize);
    CHECK(recorder.drain(&Sink::write, &sink, 4096) == 0u);
    CHECK(recorder.drain(0, &sink, 4096) == 0u);
    CHECK(recorder.drain(&Sink::write, &sink, 0) == 0u);
    rwd::hal_reset();
}
