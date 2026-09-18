// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "doctest.h"

#include <vector>

#include "rewind/replay.h"
#include "rewind/trace_reader.h"
#include "rewind/trace_writer.h"

using rwd::u8;
using rwd::u16;
using rwd::u32;
using rwd::u64;

namespace {

// Deliberately the smallest buffer the writer accepts, so every test here
// crosses block boundaries constantly instead of by luck.
const u32 kTinyBlock = rwd::kMinBlockBuffer;

struct Recorded {
    std::vector<u8>            bytes;
    std::vector<rwd::Event> expected;
    u32                        events = 0;
};

// Writes `count` events of rotating type with plausible timestamps.
Recorded make_trace(u32 count, u32 block_bytes = kTinyBlock,
                    u16 flags = rwd::kFlagRecordWrites,
                    u64 build_id = 0x1234ABCDULL, u64 seed = 99ULL) {
    Recorded out;
    rwhost::VectorSink  sink;
    rwd::TraceWriter writer;
    std::vector<u8>     block(block_bytes);

    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block.data(),
                         block_bytes, build_id, seed, flags));

    u64 ts = 0;
    for (u32 i = 0; i < count; ++i) {
        ts += 1u + (i % 7u);   // small, varied deltas: the varint's best case
        rwd::Event ev;
        ev.ts = ts;

        switch (i % 3u) {
            case 0:
                ev.type  = rwd::EV_MMIO_READ;
                ev.addr  = 0x40001000u + (i % 4u) * 4u;
                ev.value = i * 2654435761u;
                REQUIRE(writer.mmio_read(ev.ts, ev.addr, ev.value));
                break;
            case 1:
                ev.type  = rwd::EV_MMIO_WRITE;
                ev.addr  = 0x40002000u;
                ev.value = i;
                REQUIRE(writer.mmio_write(ev.ts, ev.addr, ev.value));
                break;
            default:
                ev.type  = rwd::EV_IRQ_ENTER;
                ev.addr  = i % rwd::kMaxIrqVectors;
                ev.value = 0;
                REQUIRE(writer.irq_enter(ev.ts, ev.addr));
                break;
        }
        out.expected.push_back(ev);
    }

    rwd::Event stop;
    stop.type  = rwd::EV_STOP;
    stop.ts    = ts + 1u;
    stop.addr  = 0;
    stop.value = 0;
    REQUIRE(writer.finish(stop.ts));
    out.expected.push_back(stop);

    CHECK(writer.ok());
    out.bytes  = sink.bytes;
    out.events = writer.events();
    return out;
}

// Byte range of each block's frame, header word included, in order.
struct Span {
    u32 off;
    u32 len;   // whole frame: payload_len + seq + payload + crc
};

std::vector<Span> block_spans(const std::vector<u8>& trace) {
    std::vector<Span> spans;
    u32 off = rwd::kTraceHeaderSize;
    while (off + 4u <= trace.size()) {
        const u32 payload_len = rwd::get_u32_le(trace.data() + off);
        if (payload_len == 0) {
            break;
        }
        Span span;
        span.off = off;
        span.len = rwd::kBlockOverhead + payload_len;
        if (span.off + span.len > trace.size()) {
            break;
        }
        spans.push_back(span);
        off += span.len;
    }
    return spans;
}

u32 count_blocks(const std::vector<u8>& trace) {
    return (u32)block_spans(trace).size();
}

// Offset of the first payload byte of the first block, past payload_len
// and seq.
u32 first_payload_offset() {
    return rwd::kTraceHeaderSize + 8u;
}

} // namespace

TEST_CASE("header round-trips through writer and reader") {
    const Recorded rec = make_trace(4, kTinyBlock, rwd::kFlagRecordWrites,
                                    0xFEEDFACECAFEBEEFULL, 0x0123456789ABCDEFULL);

    rwd::TraceReader reader;
    REQUIRE(reader.open(rec.bytes.data(), (u32)rec.bytes.size()));

    CHECK(reader.header().magic    == rwd::kTraceMagic);
    CHECK(reader.header().version  == rwd::kTraceVersion);
    CHECK(reader.header().flags    == rwd::kFlagRecordWrites);
    CHECK(reader.header().build_id == 0xFEEDFACECAFEBEEFULL);
    CHECK(reader.header().seed     == 0x0123456789ABCDEFULL);
}

TEST_CASE("magic is stored as the bytes R W N D") {
    const Recorded rec = make_trace(1);
    REQUIRE(rec.bytes.size() >= 4u);
    CHECK(rec.bytes[0] == 'R');
    CHECK(rec.bytes[1] == 'W');
    CHECK(rec.bytes[2] == 'N');
    CHECK(rec.bytes[3] == 'D');
}

TEST_CASE("every event survives a round trip across many blocks") {
    const u32      kEvents = 500;
    const Recorded rec     = make_trace(kEvents);

    // The point of using kMinBlockBuffer: this must be many blocks, not one.
    CHECK(count_blocks(rec.bytes) > 20u);
    CHECK(rec.events == kEvents + 1u);   // + EV_STOP

    rwd::TraceReader reader;
    REQUIRE(reader.open(rec.bytes.data(), (u32)rec.bytes.size()));

    std::vector<rwd::Event> got;
    rwd::Event              ev;
    while (reader.next(&ev)) {
        got.push_back(ev);
    }

    CHECK(reader.ok());
    CHECK(reader.at_end());
    REQUIRE(got.size() == rec.expected.size());

    for (std::size_t i = 0; i < got.size(); ++i) {
        CAPTURE(i);
        CHECK(got[i].type  == rec.expected[i].type);
        CHECK(got[i].ts    == rec.expected[i].ts);
        CHECK(got[i].addr  == rec.expected[i].addr);
        CHECK(got[i].value == rec.expected[i].value);
    }
}

TEST_CASE("timestamps accumulate correctly across block boundaries") {
    // Deltas are relative to the previous event across the whole stream, not
    // per block. If the reader ever reset its accumulator at a block start,
    // timestamps would silently collapse -- and nothing else would notice.
    const Recorded rec = make_trace(300);

    rwd::TraceReader reader;
    REQUIRE(reader.open(rec.bytes.data(), (u32)rec.bytes.size()));

    u64           previous = 0;
    rwd::Event ev;
    u32           seen = 0;
    while (reader.next(&ev)) {
        CHECK(ev.ts > previous);
        previous = ev.ts;
        ++seen;
    }
    CHECK(reader.ok());
    CHECK(seen == rec.expected.size());
    CHECK(previous == rec.expected.back().ts);
}

TEST_CASE("clearing kFlagRecordWrites drops writes from the trace") {
    const Recorded with    = make_trace(90, kTinyBlock, rwd::kFlagRecordWrites);
    const Recorded without = make_trace(90, kTinyBlock, 0);

    CHECK(without.bytes.size() < with.bytes.size());

    rwd::TraceReader reader;
    REQUIRE(reader.open(without.bytes.data(), (u32)without.bytes.size()));

    rwd::Event ev;
    u32           writes = 0;
    while (reader.next(&ev)) {
        if (ev.type == rwd::EV_MMIO_WRITE) {
            ++writes;
        }
    }
    CHECK(reader.ok());
    CHECK(writes == 0u);
}

TEST_CASE("a corrupt block is reported, not decoded") {
    // A trace that arrives over a UART is going to get corrupted eventually.
    // Handing out plausible-looking garbage would send someone chasing a bug
    // that never happened, which is worse than reporting nothing at all.
    for (u32 bit = 0; bit < 8u; ++bit) {
        Recorded rec = make_trace(200);
        REQUIRE(rec.bytes.size() > first_payload_offset());
        rec.bytes[first_payload_offset()] ^= (u8)(1u << bit);

        rwd::TraceReader reader;
        REQUIRE(reader.open(rec.bytes.data(), (u32)rec.bytes.size()));

        rwd::Event ev;
        while (reader.next(&ev)) {
            // Drain until the reader stops.
        }
        CAPTURE(bit);
        CHECK_FALSE(reader.ok());
    }
}

TEST_CASE("a truncated trace yields its intact blocks and then stops") {
    // The brown-out case: a device stops transmitting mid-trace. Whole blocks
    // already sent are still good, and they are the ones that matter.
    const Recorded full = make_trace(400);

    rwd::TraceReader whole;
    REQUIRE(whole.open(full.bytes.data(), (u32)full.bytes.size()));
    u32           total = 0;
    rwd::Event ev;
    while (whole.next(&ev)) {
        ++total;
    }
    REQUIRE(total > 0u);

    std::vector<u8> cut(full.bytes.begin(),
                        full.bytes.begin() + (long)(full.bytes.size() / 2));

    rwd::TraceReader reader;
    REQUIRE(reader.open(cut.data(), (u32)cut.size()));
    u32 recovered = 0;
    while (reader.next(&ev)) {
        ++recovered;
    }

    CHECK(recovered > 0u);
    CHECK(recovered < total);
    CHECK(reader.at_end());
}

TEST_CASE("reader rejects a buffer that is not a trace") {
    rwd::TraceReader reader;

    SUBCASE("too short for a header") {
        const u8 tiny[8] = { 'R', 'W', 'N', 'D', 1, 0, 0, 0 };
        CHECK_FALSE(reader.open(tiny, 8));
        CHECK_FALSE(reader.ok());
    }
    SUBCASE("wrong magic") {
        Recorded rec = make_trace(4);
        rec.bytes[0] = 'X';
        CHECK_FALSE(reader.open(rec.bytes.data(), (u32)rec.bytes.size()));
    }
    SUBCASE("future version") {
        Recorded rec = make_trace(4);
        rwd::put_u16_le(rec.bytes.data() + 4, rwd::kTraceVersion + 1u);
        CHECK_FALSE(reader.open(rec.bytes.data(), (u32)rec.bytes.size()));
    }
    SUBCASE("null buffer") {
        CHECK_FALSE(reader.open(0, 0));
    }
}

TEST_CASE("writer refuses a buffer too small for a worst-case event") {
    rwhost::VectorSink  sink;
    rwd::TraceWriter writer;
    u8                  block[rwd::kMinBlockBuffer];

    CHECK_FALSE(writer.begin(&rwhost::VectorSink::write, &sink, block,
                             rwd::kMinBlockBuffer - 1u, 0, 0, 0));
    CHECK_FALSE(writer.ok());
    // The buffer has to hold the 12-byte frame and still fit a worst-case
    // event in what is left.
    CHECK(rwd::kMinBlockBuffer >= rwd::kBlockOverhead + rwd::kMaxEventBytes);
}

TEST_CASE("writer latches an error on a non-monotonic timestamp") {
    // A backwards timestamp means the clock source wrapped without being
    // extended. Every delta after it would be wrong, so we stop instead.
    rwhost::VectorSink  sink;
    rwd::TraceWriter writer;
    u8                  block[rwd::kMinBlockBuffer];

    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block,
                         sizeof(block), 0, 0, rwd::kFlagRecordWrites));
    CHECK(writer.mmio_read(100, 0x40000000u, 1));
    CHECK_FALSE(writer.mmio_read(99, 0x40000000u, 2));
    CHECK_FALSE(writer.ok());
    CHECK(std::string(writer.error()) == "non-monotonic timestamp");

    // Once latched, the writer stays shut.
    CHECK_FALSE(writer.mmio_read(200, 0x40000000u, 3));
    CHECK(writer.events() == 1u);
}

TEST_CASE("writer reports a failing sink") {
    struct Failing {
        static bool write(void*, const u8*, u32) { return false; }
    };
    rwd::TraceWriter writer;
    u8                  block[rwd::kMinBlockBuffer];

    // The header write is the first thing through the sink, so begin() fails.
    CHECK_FALSE(writer.begin(&Failing::write, 0, block, sizeof(block), 0, 0, 0));
    CHECK_FALSE(writer.ok());
    CHECK(std::string(writer.error()) == "sink rejected data");
}

TEST_CASE("an empty trace is well formed") {
    rwhost::VectorSink  sink;
    rwd::TraceWriter writer;
    u8                  block[rwd::kMinBlockBuffer];

    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block,
                         sizeof(block), 7, 8, 0));
    REQUIRE(writer.finish(0));

    rwd::TraceReader reader;
    REQUIRE(reader.open(sink.bytes.data(), (u32)sink.bytes.size()));

    rwd::Event ev;
    REQUIRE(reader.next(&ev));
    CHECK(ev.type == rwd::EV_STOP);
    CHECK_FALSE(reader.next(&ev));
    CHECK(reader.ok());
    CHECK(reader.at_end());
}


TEST_CASE("blocks are numbered from zero, without gaps") {
    const Recorded          rec   = make_trace(400);
    const std::vector<Span> spans = block_spans(rec.bytes);
    REQUIRE(spans.size() > 10u);

    for (std::size_t i = 0; i < spans.size(); ++i) {
        CAPTURE(i);
        CHECK(rwd::get_u32_le(rec.bytes.data() + spans[i].off + 4u) == (u32)i);
    }

    rwd::TraceReader reader;
    REQUIRE(reader.open(rec.bytes.data(), (u32)rec.bytes.size()));
    rwd::Event ev;
    while (reader.next(&ev)) {
    }
    CHECK(reader.ok());
    CHECK(reader.blocks_lost() == 0u);
    CHECK(reader.blocks_read() == spans.size());
}

TEST_CASE("a dropped block is reported as a gap, not decoded around") {
    // The failure this exists for: a device recording faster than it can
    // drain loses whole blocks. Every surviving block still has a valid CRC,
    // so without the sequence number the trace parses cleanly and replay
    // diverges later for reasons that look like a firmware bug.
    const Recorded          full  = make_trace(400);
    const std::vector<Span> spans = block_spans(full.bytes);
    REQUIRE(spans.size() > 6u);

    const Span& dropped = spans[3];
    std::vector<u8> holed(full.bytes.begin(), full.bytes.begin() + (long)dropped.off);
    holed.insert(holed.end(),
                 full.bytes.begin() + (long)(dropped.off + dropped.len),
                 full.bytes.end());

    rwd::TraceReader reader;
    REQUIRE(reader.open(holed.data(), (u32)holed.size()));

    rwd::Event ev;
    u32        seen = 0;
    while (reader.next(&ev)) {
        ++seen;
    }

    CHECK(seen > 0u);                 // blocks before the hole are fine
    CHECK_FALSE(reader.ok());         // and the hole itself is reported
    CHECK(reader.blocks_lost() == 1u);
    CHECK(std::string(reader.error()).find("gap in trace") != std::string::npos);
}

TEST_CASE("the size of a gap is reported") {
    const Recorded          full  = make_trace(600);
    const std::vector<Span> spans = block_spans(full.bytes);
    REQUIRE(spans.size() > 12u);

    // Excise blocks 4, 5 and 6 in one contiguous cut.
    const u32 from = spans[4].off;
    const u32 to   = spans[6].off + spans[6].len;
    std::vector<u8> holed(full.bytes.begin(), full.bytes.begin() + (long)from);
    holed.insert(holed.end(), full.bytes.begin() + (long)to, full.bytes.end());

    rwd::TraceReader reader;
    REQUIRE(reader.open(holed.data(), (u32)holed.size()));
    rwd::Event ev;
    while (reader.next(&ev)) {
    }
    CHECK_FALSE(reader.ok());
    CHECK(reader.blocks_lost() == 3u);
}

TEST_CASE("spliced traces are rejected") {
    // Two recordings concatenated: the second one's blocks start again at
    // zero, so the sequence goes backwards.
    const Recorded a = make_trace(120);
    const Recorded b = make_trace(120);

    const std::vector<Span> spans_a = block_spans(a.bytes);
    REQUIRE(spans_a.size() > 3u);
    const u32 cut = spans_a[2].off + spans_a[2].len;

    std::vector<u8> spliced(a.bytes.begin(), a.bytes.begin() + (long)cut);
    spliced.insert(spliced.end(),
                   b.bytes.begin() + (long)rwd::kTraceHeaderSize, b.bytes.end());

    rwd::TraceReader reader;
    REQUIRE(reader.open(spliced.data(), (u32)spliced.size()));
    rwd::Event ev;
    while (reader.next(&ev)) {
    }
    CHECK_FALSE(reader.ok());
    CHECK(std::string(reader.error()).find("backwards") != std::string::npos);
}

TEST_CASE("the CRC covers the sequence number") {
    // Otherwise a corrupted seq would masquerade as a gap, sending someone
    // to look at buffer sizing when the real problem is the link.
    Recorded                rec   = make_trace(200);
    const std::vector<Span> spans = block_spans(rec.bytes);
    REQUIRE(spans.size() > 3u);

    rwd::put_u32_le(rec.bytes.data() + spans[2].off + 4u, 9999u);

    rwd::TraceReader reader;
    REQUIRE(reader.open(rec.bytes.data(), (u32)rec.bytes.size()));
    rwd::Event ev;
    while (reader.next(&ev)) {
    }
    CHECK_FALSE(reader.ok());
    CHECK(std::string(reader.error()).find("CRC") != std::string::npos);
    CHECK(reader.blocks_lost() == 0u);
}
