// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// A recording that begins in the middle of a run.
//
// Replay re-executes the firmware, so it can only be faithful from the state the
// recording started in. From reset that is given. From later it has to travel in
// the trace: the firmware names its state (hal_state_region), the recorder writes
// it before the first event (hal_snapshot, EV_RAM), and replay puts it back.
//
//   1. The event: the format, and that it can only be first.
//   2. The shim: what a state region is and what a snapshot writes.
//   3. Replay: the image is read, not served, and the run starts after it.
//   4. End to end: the example firmware, recorded from the middle of a run,
//      replays to exactly the state the same run has at the same point, over a
//      sweep of seeds and start points -- and can be walked backwards.
//   5. A firmware that is not the example, and one that cannot take the image.

#include "doctest.h"

#include <cstring>
#include <string>
#include <vector>

#include "support/blinker.h"
#include "rewind/crc32.h"
#include "rewind/describe.h"
#include "rewind/lanes.h"
#include "rewind/hal.h"
#include "rewind/replay.h"
#include "rewind/session.h"
#include "rewind/timeline.h"
#include "rewind/trace_reader.h"
#include "rewind/trace_writer.h"
#include "rewind/tracedoc.h"
#include "sim/mcu.h"

using rwd::u8;
using rwd::u16;
using rwd::u32;
using rwd::u64;

namespace {

std::vector<rwd::Event> decode_all(const std::vector<u8>& bytes, std::string* error = 0) {
    rwd::TraceReader reader;
    REQUIRE(reader.open(bytes.data(), (u32)bytes.size()));
    std::vector<rwd::Event> out;
    rwd::Event ev;
    while (reader.next(&ev)) {
        out.push_back(ev);
    }
    if (error != 0) {
        *error = reader.ok() ? "" : reader.error();
    }
    return out;
}

struct Rec {
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[rwd::kMinBlockBuffer];
    explicit Rec(u16 flags = 0) {
        REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block, sizeof(block), 1, 2, flags));
    }
};

std::vector<u8> handmade(const std::vector<u8>& payload, u16 version = rwd::kTraceVersion) {
    std::vector<u8> trace(rwd::kTraceHeaderSize, 0);
    rwd::put_u32_le(trace.data() + 0, rwd::kTraceMagic);
    rwd::put_u16_le(trace.data() + 4, version);
    u8 frame[8];
    rwd::put_u32_le(frame + 0, (u32)payload.size());
    rwd::put_u32_le(frame + 4, 0u);
    trace.insert(trace.end(), frame, frame + 8);
    trace.insert(trace.end(), payload.begin(), payload.end());
    u32 crc = rwd::crc32_update(rwd::crc32_init(), frame + 4, 4);
    crc     = rwd::crc32_update(crc, payload.data(), (u32)payload.size());
    u8 tail[4];
    rwd::put_u32_le(tail, rwd::crc32_final(crc));
    trace.insert(trace.end(), tail, tail + 4);
    rwd::put_u32_le(tail, 0u);
    trace.insert(trace.end(), tail, tail + 4);
    return trace;
}

void varint(std::vector<u8>* out, u64 v) {
    u8        buf[rwd::kVarintMaxBytes];
    const u32 n = rwd::varint_encode(v, buf);
    out->insert(out->end(), buf, buf + n);
}

std::string read_error(const std::vector<u8>& payload) {
    const std::vector<u8> trace = handmade(payload);
    rwd::TraceReader      reader;
    REQUIRE(reader.open(trace.data(), (u32)trace.size()));
    rwd::Event ev;
    while (reader.next(&ev)) {
    }
    CHECK_FALSE(reader.ok());
    return reader.error();
}

} // namespace

// ---------------------------------------------------------------------------
// 1. The event
// ---------------------------------------------------------------------------

TEST_CASE("a memory image round-trips through writer and reader, ahead of the run") {
    Rec r;
    const u8 a[5] = { 1, 2, 3, 4, 5 };
    u8       full[rwd::kMaxRamChunk];
    for (u32 i = 0; i < rwd::kMaxRamChunk; ++i) {
        full[i] = (u8)(0xA0u + i);
    }
    REQUIRE(r.writer.ram(100, 0, 0, full, rwd::kMaxRamChunk));
    REQUIRE(r.writer.ram(100, 0, 16, a, 5));
    REQUIRE(r.writer.ram(100, 7, 0xFFFFFFF0u, a, 1));
    REQUIRE(r.writer.mmio_read(105, 0x40001000u, 9u));
    REQUIRE(r.writer.dma(110, 0, 0, a, 2));
    REQUIRE(r.writer.finish(120));

    const std::vector<rwd::Event> ev = decode_all(r.sink.bytes);
    REQUIRE(ev.size() == 6u);
    CHECK(ev[0].type == rwd::EV_RAM);
    CHECK(ev[0].ts == 100u);
    CHECK(ev[0].addr == 0u);
    CHECK(ev[0].value == 0u);
    CHECK(ev[0].len == rwd::kMaxRamChunk);
    CHECK(std::memcmp(ev[0].data, full, rwd::kMaxRamChunk) == 0);
    CHECK(ev[1].value == 16u);
    CHECK(ev[1].len == 5u);
    CHECK(std::memcmp(ev[1].data, a, 5) == 0);
    CHECK(ev[2].addr == 7u);
    CHECK(ev[2].value == 0xFFFFFFF0u);

    // Not a register access: the address predictor carries on from the read.
    CHECK(ev[3].type == rwd::EV_MMIO_READ);
    CHECK(ev[3].addr == 0x40001000u);
    CHECK(ev[4].type == rwd::EV_DMA);
}

TEST_CASE("the writer takes an image only before the run begins") {
    Rec r;
    const u8 b[2] = { 1, 2 };
    REQUIRE(r.writer.ram(1, 0, 0, b, 2));
    REQUIRE(r.writer.site(2, 5));                     // the run has begun...
    CHECK_FALSE(r.writer.ram(3, 0, 2, b, 2));         // ...and an image now is not of its start
    CHECK_FALSE(r.writer.ok());
    CHECK(std::string(r.writer.error()).find("image") != std::string::npos);
}

TEST_CASE("the writer refuses an image chunk it cannot hold") {
    {
        Rec r;
        u8 bytes[rwd::kMaxRamChunk + 1] = { 0 };
        CHECK_FALSE(r.writer.ram(1, 0, 0, bytes, 0));
        CHECK(std::string(r.writer.error()).find("kMaxRamChunk") != std::string::npos);
    }
    {
        Rec r;
        u8 bytes[rwd::kMaxRamChunk + 1] = { 0 };
        CHECK_FALSE(r.writer.ram(1, 0, 0, bytes, rwd::kMaxRamChunk + 1));
    }
    {
        Rec r;
        CHECK_FALSE(r.writer.ram(1, 0, 0, 0, 4));    // no bytes
    }
    {
        Rec r;
        const u8 b[1] = { 0 };
        REQUIRE(r.writer.ram(10, 0, 0, b, 1));
        CHECK_FALSE(r.writer.ram(9, 0, 1, b, 1));    // time does not go backwards
        CHECK(std::string(r.writer.error()).find("monotonic") != std::string::npos);
    }
}

TEST_CASE("the reader takes an image only first") {
    // The trace is well-formed in every other way: valid header, CRC, terminator.
    std::vector<u8> p;
    p.push_back((u8)rwd::EV_SITE);
    varint(&p, 1);          // delta_ts
    varint(&p, 4);          // site id
    p.push_back((u8)rwd::EV_RAM);
    varint(&p, 0);
    varint(&p, 0);
    varint(&p, 0);
    p.push_back(1);
    p.push_back(0xAA);

    const std::vector<u8> trace = handmade(p);
    rwd::TraceReader reader;
    REQUIRE(reader.open(trace.data(), (u32)trace.size()));
    rwd::Event ev;
    REQUIRE(reader.next(&ev));
    CHECK(ev.type == rwd::EV_SITE);
    CHECK_FALSE(reader.next(&ev));
    CHECK_FALSE(reader.ok());
    CHECK(std::string(reader.error()).find("image after the run began") != std::string::npos);
}

TEST_CASE("an image in an older trace is refused") {
    Rec r;
    const u8 b[2] = { 1, 2 };
    REQUIRE(r.writer.ram(1, 0, 0, b, 2));
    REQUIRE(r.writer.finish(2));

    for (u16 v = 2; v <= 5; ++v) {
        CAPTURE(v);
        std::vector<u8> trace = r.sink.bytes;
        rwd::put_u16_le(trace.data() + 4, v);
        std::string error;
        decode_all(trace, &error);
        CHECK(error.find("unknown event type") != std::string::npos);
    }
}

TEST_CASE("a malformed image event is rejected, and named for what it is") {
    SUBCASE("cut short before the region") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_RAM);
        varint(&p, 0);
        CHECK(read_error(p).find("ram region") != std::string::npos);
    }
    SUBCASE("region wider than 32 bits") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_RAM);
        varint(&p, 0);
        varint(&p, 0x100000000ULL);
        CHECK(read_error(p).find("ram region does not fit") != std::string::npos);
    }
    SUBCASE("cut short before the offset") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_RAM);
        varint(&p, 0);
        varint(&p, 1);
        CHECK(read_error(p).find("ram offset") != std::string::npos);
    }
    SUBCASE("length zero") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_RAM);
        varint(&p, 0);
        varint(&p, 1);
        varint(&p, 0);
        p.push_back(0);
        CHECK(read_error(p).find("kMaxRamChunk") != std::string::npos);
    }
    SUBCASE("length over the limit") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_RAM);
        varint(&p, 0);
        varint(&p, 1);
        varint(&p, 0);
        p.push_back((u8)(rwd::kMaxRamChunk + 1));
        CHECK(read_error(p).find("kMaxRamChunk") != std::string::npos);
    }
    SUBCASE("cut short before the bytes") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_RAM);
        varint(&p, 0);
        varint(&p, 1);
        varint(&p, 0);
        p.push_back(8);
        p.push_back(1);
        CHECK(read_error(p).find("ram event cut short") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 2. The shim
// ---------------------------------------------------------------------------

namespace {

struct Quiet {
    rwd::HalOps ops;
    u64         t;
    Quiet() : t(1000) {
        ops.now = &Quiet::now;
        ops.ctx = this;
    }
    static u64 now(void* c) { return static_cast<Quiet*>(c)->t; }
};

} // namespace

TEST_CASE("state regions are validated, and reset forgets them") {
    rwd::hal_reset();
    u8 a[8], b[8];
    CHECK_FALSE(rwd::hal_state_region(0, 0, 8));            // no memory
    CHECK_FALSE(rwd::hal_state_region(0, a, 0));            // nothing in it
    CHECK(rwd::hal_state_region(0, a, sizeof(a)));
    CHECK_FALSE(rwd::hal_state_region(0, b, sizeof(b)));    // the id is taken
    CHECK(rwd::hal_state_region(1, b, sizeof(b)));
    u8 c[1], d[1], e[1];
    CHECK(rwd::hal_state_region(2, c, 1));
    CHECK(rwd::hal_state_region(3, d, 1));
    CHECK_FALSE(rwd::hal_state_region(4, e, 1));            // kMaxStateRegions of them

    rwd::hal_reset();
    CHECK(rwd::hal_state_region(0, a, sizeof(a)));          // and room again
    rwd::hal_reset();
}

TEST_CASE("a snapshot writes the regions in chunks, in the order they were registered") {
    Quiet q;
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[rwd::kMinBlockBuffer];
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block, sizeof(block), 1, 2, 0));

    rwd::hal_reset();
    rwd::hal_attach(&q.ops);

    u8 big[40], small[3];
    for (u32 i = 0; i < sizeof(big); ++i) { big[i] = (u8)(i + 1); }
    small[0] = 0xE1; small[1] = 0xE2; small[2] = 0xE3;
    REQUIRE(rwd::hal_state_region(5, big, sizeof(big)));
    REQUIRE(rwd::hal_state_region(2, small, sizeof(small)));

    CHECK_FALSE(rwd::hal_snapshot());                       // not recording
    rwd::hal_mode_record(&writer);
    REQUIRE(rwd::hal_snapshot());
    REQUIRE(writer.finish(2000));
    rwd::hal_reset();

    const std::vector<rwd::Event> ev = decode_all(sink.bytes);
    REQUIRE(ev.size() == 5u);                               // 16 + 16 + 8, 3, stop
    CHECK(ev[0].addr == 5u);  CHECK(ev[0].value == 0u);   CHECK(ev[0].len == 16u);
    CHECK(ev[1].addr == 5u);  CHECK(ev[1].value == 16u);  CHECK(ev[1].len == 16u);
    CHECK(ev[2].addr == 5u);  CHECK(ev[2].value == 32u);  CHECK(ev[2].len == 8u);
    CHECK(ev[3].addr == 2u);  CHECK(ev[3].value == 0u);   CHECK(ev[3].len == 3u);
    CHECK(ev[0].ts == 1000u);                               // when the shim's clock said
    CHECK(ev[3].ts == 1000u);
    CHECK(ev[0].data[0] == 1u);
    CHECK(ev[2].data[7] == 40u);
    CHECK(ev[3].data[2] == 0xE3);
}

TEST_CASE("a snapshot is refused when there is nothing to write or the run has begun") {
    Quiet q;
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[rwd::kMinBlockBuffer];
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block, sizeof(block), 1, 2, 0));

    rwd::hal_reset();
    rwd::hal_attach(&q.ops);
    rwd::hal_mode_record(&writer);
    CHECK_FALSE(rwd::hal_snapshot());                       // no regions registered

    u8 a[4] = { 1, 2, 3, 4 };
    REQUIRE(rwd::hal_state_region(0, a, sizeof(a)));
    REQUIRE(writer.site(1010, 3));                          // something recorded already
    CHECK_FALSE(rwd::hal_snapshot());
    rwd::hal_reset();
}

// ---------------------------------------------------------------------------
// 3. Replay
// ---------------------------------------------------------------------------

TEST_CASE("replay starts after the image and does not serve it") {
    Rec r;
    const u8 lo[4]  = { 1, 2, 3, 4 };
    const u8 hi[2]  = { 9, 8 };
    const u8 other[1] = { 0x55 };
    REQUIRE(r.writer.ram(500, 3, 0, lo, 4));
    REQUIRE(r.writer.ram(500, 3, 10, hi, 2));         // a gap of six bytes before it
    REQUIRE(r.writer.ram(500, 1, 0, other, 1));
    REQUIRE(r.writer.mmio_read(510, 0x1000u, 77u));
    REQUIRE(r.writer.finish(520));

    rwhost::ReplayHal hal;
    REQUIRE(hal.load(r.sink.bytes.data(), r.sink.bytes.size()));
    CHECK(hal.has_image());
    CHECK(hal.first_event() == 3u);
    CHECK(hal.event_count() == 5u);                   // indices are the reader's
    CHECK(hal.cursor() == 3u);
    CHECK(hal.now() == 500u);                         // the clock the image was taken at

    const std::map<u32, std::vector<u8> > img = hal.image();
    REQUIRE(img.size() == 2u);
    REQUIRE(img.count(3) == 1u);
    const std::vector<u8>& r3 = img.find(3)->second;
    REQUIRE(r3.size() == 12u);
    CHECK(r3[0] == 1u);  CHECK(r3[3] == 4u);
    CHECK(r3[4] == 0u);  CHECK(r3[9] == 0u);          // the gap reads as zero
    CHECK(r3[10] == 9u); CHECK(r3[11] == 8u);
    CHECK(img.find(1)->second.size() == 1u);

    // The first thing the firmware asks for is the first event after the image.
    rwd::hal_reset();
    rwd::hal_attach(hal.ops());
    rwd::hal_mode_replay();
    CHECK(rwd::mmio_read32(0x1000u) == 77u);
    CHECK_FALSE(hal.diverged());
    CHECK(hal.cursor() == 4u);

    // And starting over goes back there, not to the image.
    hal.restart();
    CHECK(hal.cursor() == 3u);
    CHECK(hal.now() == 500u);
    rwd::hal_reset();
}

TEST_CASE("a trace with no image replays from its first event, as before") {
    Rec r;
    REQUIRE(r.writer.mmio_read(10, 0x1000u, 5u));
    REQUIRE(r.writer.finish(20));
    rwhost::ReplayHal hal;
    REQUIRE(hal.load(r.sink.bytes.data(), r.sink.bytes.size()));
    CHECK_FALSE(hal.has_image());
    CHECK(hal.first_event() == 0u);
    CHECK(hal.cursor() == 0u);
    CHECK(hal.now() == 0u);
    CHECK(hal.image().empty());
}

// ---------------------------------------------------------------------------
// 4. End to end, with the example firmware
// ---------------------------------------------------------------------------

namespace {

rwhost::RecordResult run(u64 seed, fw::Variant v, u32 skip, u32 iters) {
    rwhost::RunConfig cfg;
    cfg.seed       = seed;
    cfg.variant    = v;
    cfg.skip_iters = skip;
    cfg.main_iters = iters;
    return rwhost::record_run(cfg);
}

} // namespace

TEST_CASE("a run recorded from the middle replays to the state the whole run has there") {
    // For each seed and start point: the run recorded from `skip` onwards replays
    // from its trace alone to the state the same run, unrecorded to there and
    // recorded after, ended in -- and to the state a recording from reset has at
    // the same point. Two ways of saying that the image is the state.
    const fw::Variant variants[] = { fw::kBuggy, fw::kFixed, fw::kUnguarded };
    u32 checked = 0;
    for (u64 seed = 1; seed <= 12; ++seed) {
        for (u32 vi = 0; vi < 3; ++vi) {
            for (u32 skip = 700; skip <= 4200; skip += 1750) {
                const u32 total = 6000;
                const rwhost::RecordResult whole = run(seed, variants[vi], 0, total);
                const rwhost::RecordResult part  = run(seed, variants[vi], skip, total - skip);
                CAPTURE(seed);
                CAPTURE(vi);
                CAPTURE(skip);
                REQUIRE(part.writer_ok);

                // Not recording the first stretch does not change the run.
                REQUIRE(part.fw.state_hash == whole.fw.state_hash);

                // The trace is shorter, by what was not recorded, plus its image.
                REQUIRE(part.trace.size() < whole.trace.size());

                const rwhost::ReplayResult rep = rwhost::replay_run(part.trace, total - skip);
                REQUIRE(rep.loaded);
                CHECK_FALSE(rep.diverged);
                CHECK(rep.divergence.empty());
                CHECK(rep.events_consumed + 1 == rep.events_total);
                REQUIRE(rep.fw.state_hash == whole.fw.state_hash);
                ++checked;
            }
        }
    }
    CHECK(checked == 12u * 3u * 3u);
}

TEST_CASE("an image holds exactly the firmware's state at the point it was taken") {
    const u32 skip = 2500;
    const rwhost::RecordResult upto = run(4, fw::kBuggy, 0, skip);          // from reset, to there
    const rwhost::RecordResult part = run(4, fw::kBuggy, skip, 1000);

    rwhost::TraceDoc doc;
    REQUIRE(doc.open(part.trace, 1000));
    REQUIRE(doc.has_firmware());

    // The state the firmware has at the first event it asks for is the state the
    // unrecorded stretch ended in, byte for byte.
    rwhost::Snapshot s;
    REQUIRE(doc.snapshot_at(0, &s));                 // 0 is before the image: the start is the same
    CHECK(s.state_hash == upto.fw.state_hash);
    CHECK(s.fw.consumed == upto.fw.consumed);
    CHECK(s.fw.checksum == upto.fw.checksum);
    CHECK(s.event_index == 4u);                      // a 52-byte state, in four chunks

    rwhost::Snapshot s4;
    REQUIRE(doc.snapshot_at(4, &s4));
    CHECK(s4.state_hash == s.state_hash);
    CHECK(s4.event_index == s.event_index);
}

TEST_CASE("a run that began mid-run can be walked backwards to where it began") {
    const rwhost::RecordResult upto = run(6, fw::kBuggy, 0, 3000);
    const rwhost::RecordResult part = run(6, fw::kBuggy, 3000, 3000);
    const rwhost::RecordResult whole = run(6, fw::kBuggy, 0, 6000);

    rwhost::Timeline tl;
    REQUIRE(tl.open(part.trace, 3000, 64));
    CHECK(tl.checkpoints() >= 40u);
    CHECK(tl.event_count() + 1 == part.events);      // the stop is never served
    CHECK(part.events > 1000u);

    rwhost::Snapshot end, start, back;
    REQUIRE(tl.seek(tl.event_count(), &end));
    CHECK(end.state_hash == whole.fw.state_hash);

    // Backwards, one stride at a time, every state is one a fresh replay reaches,
    // and the walk ends at the image.
    std::size_t at = tl.event_count();
    u32 consumed_before = end.fw.consumed;
    while (at > 7) {
        at -= 7;
        REQUIRE(tl.seek(at, &back));
        CHECK(back.fw.consumed <= consumed_before);
        consumed_before = back.fw.consumed;
    }
    REQUIRE(tl.seek(0, &start));
    CHECK(start.state_hash == upto.fw.state_hash);
    CHECK(start.event_index == 4u);

    // And bisection works from the image on: when did it consume one more byte?
    struct P {
        static bool one_more(const rwhost::Snapshot& s, void* c) {
            return s.fw.consumed > *static_cast<u32*>(c);
        }
    };
    u32 base = upto.fw.consumed;
    rwhost::Snapshot found;
    if (end.fw.consumed > base) {
        REQUIRE(tl.find_first(&P::one_more, &base, &found));
        CHECK(found.fw.consumed == base + 1u);
        CHECK(found.event_index > 4u);
    }
}

TEST_CASE("replaying a mid-run trace with the wrong iteration count is noticed") {
    const rwhost::RecordResult part = run(3, fw::kBuggy, 2000, 2000);
    // The sum, which is what one would pass if main_iters counted from reset.
    const rwhost::ReplayResult rep = rwhost::replay_run(part.trace, 4000);
    REQUIRE(rep.loaded);
    CHECK(rep.diverged);
    CHECK(rep.divergence.find("past the end") != std::string::npos);
}

TEST_CASE("a recording from the middle still finds interrupts where they landed") {
    // The unguarded variant's race lives in a probe point; the state it needs to
    // have is in the image, and the interrupt that lands inside the window is in
    // the trace. Over a sweep of seeds the replay agrees every time.
    u32 lost = 0;
    for (u64 seed = 1; seed <= 40; ++seed) {
        const rwhost::RecordResult part = run(seed, fw::kUnguarded, 1500, 4500);
        REQUIRE(part.writer_ok);
        const rwhost::ReplayResult rep = rwhost::replay_run(part.trace, 4500);
        CAPTURE(seed);
        CHECK_FALSE(rep.diverged);
        CHECK(rep.fw.state_hash == part.fw.state_hash);
        lost += part.bug_triggered() ? 1u : 0u;
    }
    CHECK(lost > 0u);              // the race is there to be replayed
}

// ---------------------------------------------------------------------------
// 5. Another firmware, and one that cannot take the image
// ---------------------------------------------------------------------------

TEST_CASE("a firmware that is not the example starts from an image too") {
    using namespace blinker;
    const u32 skip = 250, iters = 350;

    Device d1(11), d2(11);
    Blinker f1, f2;
    const rwhost::FirmwareRecording whole =
        rwhost::record_firmware(f1, d1.ops(), kBlinkerBuild, 11, rwd::kFlagRecordWrites, skip + iters);
    const rwhost::FirmwareRecording part =
        rwhost::record_firmware(f2, d2.ops(), kBlinkerBuild, 11, rwd::kFlagRecordWrites, iters, skip);
    REQUIRE(part.writer_ok);
    CHECK(part.state_hash == whole.state_hash);

    Blinker fresh;
    const rwhost::ReplayResult rep = rwhost::replay_firmware(fresh, part.trace, iters);
    REQUIRE(rep.loaded);
    CHECK_FALSE(rep.diverged);
    CHECK(rep.fw.state_hash == whole.state_hash);

    // And through a timeline, from the image.
    rwhost::Timeline tl;
    Blinker walker;
    REQUIRE(tl.open(part.trace, &walker, iters, 32));
    rwhost::Snapshot s0, s1;
    REQUIRE(tl.seek(0, &s0));
    CHECK(decode(s0).presses > 0u);                  // not a firmware at power-on
    REQUIRE(tl.seek(tl.event_count(), &s1));
    CHECK(s1.state_hash == whole.state_hash);
}

namespace {

// A firmware that does what Blinker does but has no way to take the image.
class Stubborn : public blinker::Blinker {
public:
    bool restore_region(u32, const u8*, std::size_t) override { return false; }
};

} // namespace

TEST_CASE("a firmware that cannot take the image is told so, everywhere it would start") {
    using namespace blinker;
    Device d(11);
    Blinker f;
    const rwhost::FirmwareRecording part =
        rwhost::record_firmware(f, d.ops(), kBlinkerBuild, 11, rwd::kFlagRecordWrites, 300, 200);
    REQUIRE(part.writer_ok);

    Stubborn s;
    const rwhost::ReplayResult rep = rwhost::replay_firmware(s, part.trace, 300);
    CHECK_FALSE(rep.loaded);
    CHECK(rep.error.find("memory image") != std::string::npos);

    rwhost::Timeline tl;
    CHECK_FALSE(tl.open(part.trace, &s, 300, 32));
    CHECK(tl.error().find("memory image") != std::string::npos);

    const rwhost::LocateReport loc = rwhost::locate_firmware(s, part.trace, 300);
    CHECK_FALSE(loc.loaded);
    CHECK(loc.error.find("memory image") != std::string::npos);
}

TEST_CASE("asking for an image from a firmware that has no state region fails, and says why") {
    // A firmware that never registers one: nothing to write.
    class Bare : public blinker::Blinker {
    public:
        void install() override { rwd::hal_set_isr(blinker::kIrqButton, &blinker::button_isr); }
    };
    blinker::Device d(3);
    Bare b;
    const rwhost::FirmwareRecording r =
        rwhost::record_firmware(b, d.ops(), blinker::kBlinkerBuild, 3, 0, 100, 50);
    CHECK_FALSE(r.writer_ok);
    CHECK(r.writer_error.find("state region") != std::string::npos);
    CHECK(r.trace.empty());
}

TEST_CASE("an image is described, and is not drawn on a timeline") {
    rwd::Event e;
    e.type  = rwd::EV_RAM;
    e.ts    = 5;
    e.addr  = 2;
    e.value = 16;
    e.len   = 3;
    for (u32 i = 0; i < rwd::kMaxDmaChunk; ++i) { e.data[i] = 0; }
    e.data[0] = 0xAA; e.data[1] = 0xBB; e.data[2] = 0xCC;

    CHECK(rwhost::describe_event(e) == "ram region 2 +16 len 3 AABBCC");
    CHECK(std::string(rwhost::event_kind(rwd::EV_RAM)) == "ram");

    rwhost::Lane lane = rwhost::kLaneCount;
    CHECK_FALSE(rwhost::lane_of(rwd::EV_RAM, &lane));

    CHECK(std::string(rwhost::irq_name(sim::kIrqTimer)) == "TIMER");
    CHECK(std::string(rwhost::irq_name(sim::kIrqUartRx)) == "UART_RX");
    CHECK(std::string(rwhost::irq_name(6)) == "");
}
