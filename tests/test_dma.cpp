// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// DMA: memory that something other than the CPU writes.
//
// Nothing on a part can observe when a DMA controller deposits a byte, but the
// moment the firmware looks at the buffer is an access like any other. So the
// firmware reads DMA buffers through the shim, each read is recorded with the
// bytes it saw, and replay hands those bytes back. These tests cover the
// format, the shim, the three backends and the example firmware's DMA receiver.

#include "doctest.h"

#include <cstring>
#include <string>
#include <vector>

#include "rewind/cortex_m.h"
#include "rewind/crc32.h"
#include "rewind/hal.h"
#include "rewind/replay.h"
#include "rewind/session.h"
#include "rewind/timeline.h"
#include "rewind/trace_reader.h"
#include "rewind/trace_writer.h"
#include "sim/mcu.h"

using rwd::u8;
using rwd::u16;
using rwd::u32;
using rwd::u64;

namespace {

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

// A backend with two DMA regions, a clock that moves on every access, and a
// queue of interrupts to deliver.
struct FakeDma {
    std::vector<u8>  region[2];
    u64              clock = 0;
    std::vector<u32> irqs;
    u32              reads = 0;

    FakeDma() {
        for (u32 r = 0; r < 2; ++r) {
            region[r].resize(64);
            for (u32 i = 0; i < 64; ++i) {
                region[r][i] = (u8)(r * 100u + i);
            }
        }
        ops_.read32   = &FakeDma::s_read32;
        ops_.write32  = &FakeDma::s_write32;
        ops_.now      = &FakeDma::s_now;
        ops_.poll_irq = &FakeDma::s_poll_irq;
        ops_.dma_read = &FakeDma::s_dma_read;
        ops_.ctx      = this;
    }
    const rwd::HalOps* ops() const { return &ops_; }

    static u32 s_read32(void* c, u32) { static_cast<FakeDma*>(c)->clock += 10; return 7; }
    static void s_write32(void* c, u32, u32) { static_cast<FakeDma*>(c)->clock += 10; }
    static u64 s_now(void* c) { return static_cast<FakeDma*>(c)->clock; }
    static u32 s_poll_irq(void* c) {
        FakeDma* self = static_cast<FakeDma*>(c);
        if (self->irqs.empty()) {
            return rwd::kNoIrq;
        }
        const u32 v = self->irqs.front();
        self->irqs.erase(self->irqs.begin());
        return v;
    }
    static void s_dma_read(void* c, u32 region, u32 offset, u8* dst, u32 len) {
        FakeDma* self = static_cast<FakeDma*>(c);
        self->clock += 5;
        ++self->reads;
        for (u32 i = 0; i < len; ++i) {
            dst[i] = (region < 2 && offset + i < 64) ? self->region[region][offset + i] : 0;
        }
    }

private:
    rwd::HalOps ops_;
};

u32 g_isr_calls = 0;
void isr_count() { ++g_isr_calls; }

struct Recorded {
    std::vector<u8>         trace;
    std::vector<rwd::Event> events;
};

// Records `body` against a FakeDma.
template <typename Body>
Recorded record_with(FakeDma& hal, Body body) {
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[rwd::kMinBlockBuffer];

    g_isr_calls = 0;
    rwd::hal_reset();
    rwd::hal_attach(hal.ops());
    rwd::hal_set_isr(3u, &isr_count);
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block, sizeof(block),
                         1, 2, rwd::kFlagRecordWrites));
    rwd::hal_mode_record(&writer);
    body();
    REQUIRE(writer.finish(rwd::hal_now()));
    REQUIRE(writer.blocks_lost() == 0u);
    rwd::hal_reset();

    Recorded rec;
    rec.trace  = sink.bytes;
    rec.events = decode(sink.bytes);
    return rec;
}

} // namespace

// -- the trace format --

TEST_CASE("a DMA read round-trips through writer and reader") {
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[rwd::kMinBlockBuffer];
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block, sizeof(block), 1, 2, 0));

    const u8 a[5] = { 1, 2, 3, 4, 5 };
    u8       full[rwd::kMaxDmaChunk];
    for (u32 i = 0; i < rwd::kMaxDmaChunk; ++i) {
        full[i] = (u8)(0xF0u + i);
    }
    const u8 one[1] = { 0xAB };

    REQUIRE(writer.mmio_read(5, 0x40001000u, 9u));
    REQUIRE(writer.dma(10, 0u, 3u, a, 5));
    REQUIRE(writer.dma(20, 7u, 0xFFFFFFF0u, full, rwd::kMaxDmaChunk));
    REQUIRE(writer.dma(20, 1u, 0u, one, 1));
    REQUIRE(writer.mmio_read(30, 0x40001004u, 3u));
    REQUIRE(writer.finish(40));

    const std::vector<rwd::Event> ev = decode(sink.bytes);
    REQUIRE(ev.size() == 6u);

    CHECK(ev[1].type == rwd::EV_DMA);
    CHECK(ev[1].ts == 10u);
    CHECK(ev[1].addr == 0u);      // region
    CHECK(ev[1].value == 3u);     // offset
    CHECK(ev[1].len == 5u);
    CHECK(std::memcmp(ev[1].data, a, 5) == 0);

    CHECK(ev[2].addr == 7u);
    CHECK(ev[2].value == 0xFFFFFFF0u);
    CHECK(ev[2].len == rwd::kMaxDmaChunk);
    CHECK(std::memcmp(ev[2].data, full, rwd::kMaxDmaChunk) == 0);

    CHECK(ev[3].len == 1u);
    CHECK(ev[3].data[0] == 0xAB);

    // Not a register access: the address predictor carries on from the read
    // before it, and ordinary events carry no bytes.
    CHECK(ev[4].addr == 0x40001004u);
    CHECK(ev[0].len == 0u);
    CHECK(ev[4].len == 0u);
}

TEST_CASE("the writer refuses a DMA chunk it cannot hold") {
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[rwd::kMinBlockBuffer];
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block, sizeof(block), 1, 2, 0));
    u8 bytes[rwd::kMaxDmaChunk + 1] = { 0 };

    CHECK_FALSE(writer.dma(1, 0, 0, bytes, 0));
    CHECK_FALSE(writer.ok());
    CHECK(std::string(writer.error()).find("kMaxDmaChunk") != std::string::npos);
}

TEST_CASE("the smallest block buffer holds a full DMA chunk with the widest fields") {
    // The worst case for one event: a type byte, a wide timestamp delta, a
    // five-byte region and offset, a length and a full chunk.
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[rwd::kMinBlockBuffer];
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block, sizeof(block), 1, 2, 0));

    u8 bytes[rwd::kMaxDmaChunk];
    for (u32 i = 0; i < rwd::kMaxDmaChunk; ++i) {
        bytes[i] = (u8)i;
    }
    u64 ts = 0;
    for (u32 i = 0; i < 20; ++i) {
        ts += 0x7FFFFFFFFFFFull;   // a delta that needs many varint bytes
        REQUIRE(writer.dma(ts, 0xFFFFFFFFu, 0xFFFFFFFFu, bytes, rwd::kMaxDmaChunk));
    }
    REQUIRE(writer.finish(ts + 1));
    REQUIRE(writer.blocks_lost() == 0u);
    CHECK(decode(sink.bytes).size() == 21u);
}

TEST_CASE("EV_DMA inside an older trace is refused") {
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[rwd::kMinBlockBuffer];
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block, sizeof(block), 1, 2, 0));
    const u8 b[2] = { 1, 2 };
    REQUIRE(writer.dma(1, 0, 0, b, 2));
    REQUIRE(writer.finish(2));

    for (u16 v = 2; v <= 4; ++v) {
        CAPTURE(v);
        std::vector<u8> trace = sink.bytes;
        rwd::put_u16_le(trace.data() + 4, v);
        rwd::TraceReader reader;
        REQUIRE(reader.open(trace.data(), (u32)trace.size()));
        rwd::Event ev;
        while (reader.next(&ev)) {
        }
        CHECK_FALSE(reader.ok());
        CHECK(std::string(reader.error()).find("unknown event type") != std::string::npos);
    }
}

namespace {

// A trace around a hand-built payload, with a valid header, CRC and terminator.
std::vector<u8> handmade(const std::vector<u8>& payload) {
    std::vector<u8> trace(rwd::kTraceHeaderSize, 0);
    rwd::put_u32_le(trace.data() + 0, rwd::kTraceMagic);
    rwd::put_u16_le(trace.data() + 4, rwd::kTraceVersion);
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
    CHECK_FALSE(reader.next(&ev));
    CHECK_FALSE(reader.ok());
    return reader.error();
}

} // namespace

TEST_CASE("a malformed DMA event is rejected") {
    SUBCASE("cut short before the region") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_DMA);
        varint(&p, 0);
        CHECK(read_error(p).find("region") != std::string::npos);
    }
    SUBCASE("region wider than 32 bits") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_DMA);
        varint(&p, 0);
        varint(&p, 0x100000000ULL);
        CHECK(read_error(p).find("32 bits") != std::string::npos);
    }
    SUBCASE("cut short before the offset") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_DMA);
        varint(&p, 0);
        varint(&p, 1);
        CHECK(read_error(p).find("offset") != std::string::npos);
    }
    SUBCASE("offset wider than 32 bits") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_DMA);
        varint(&p, 0);
        varint(&p, 1);
        varint(&p, 0x100000000ULL);
        CHECK(read_error(p).find("32 bits") != std::string::npos);
    }
    SUBCASE("cut short before the length") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_DMA);
        varint(&p, 0);
        varint(&p, 1);
        varint(&p, 2);
        CHECK(read_error(p).find("length") != std::string::npos);
    }
    SUBCASE("length zero") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_DMA);
        varint(&p, 0); varint(&p, 1); varint(&p, 2);
        p.push_back(0);
        CHECK(read_error(p).find("length") != std::string::npos);
    }
    SUBCASE("length over the chunk limit") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_DMA);
        varint(&p, 0); varint(&p, 1); varint(&p, 2);
        p.push_back((u8)(rwd::kMaxDmaChunk + 1));
        for (u32 i = 0; i <= rwd::kMaxDmaChunk; ++i) {
            p.push_back(0);
        }
        CHECK(read_error(p).find("length") != std::string::npos);
    }
    SUBCASE("fewer data bytes than the length says") {
        std::vector<u8> p;
        p.push_back((u8)rwd::EV_DMA);
        varint(&p, 0); varint(&p, 1); varint(&p, 2);
        p.push_back(8);
        p.push_back(1);
        p.push_back(2);   // two of eight
        CHECK(read_error(p).find("data") != std::string::npos);
    }
}

TEST_CASE("every version from the oldest supported to the current one still opens") {
    for (u16 v = rwd::kTraceMinVersion; v <= rwd::kTraceVersion; ++v) {
        CAPTURE(v);
        std::vector<u8> payload;
        payload.push_back((u8)rwd::EV_STOP);
        varint(&payload, 0);
        std::vector<u8> trace = handmade(payload);
        rwd::put_u16_le(trace.data() + 4, v);
        rwd::TraceReader reader;
        CHECK(reader.open(trace.data(), (u32)trace.size()));
    }
}

// -- the shim --

TEST_CASE("a long DMA read is recorded as chunks, in order, with the bytes seen") {
    FakeDma hal;
    u8      got[40];

    const Recorded rec = record_with(hal, [&]() {
        rwd::mmio_read32(0x40000000u);
        rwd::hal_dma_read(1u, 4u, got, sizeof(got));      // 16 + 16 + 8
        rwd::mmio_read32(0x40000000u);
    });

    // The caller gets the backend's bytes.
    for (u32 i = 0; i < sizeof(got); ++i) {
        CHECK(got[i] == (u8)(100u + 4u + i));
    }

    REQUIRE(rec.events.size() == 6u);   // read, three chunks, read, stop
    CHECK(rec.events[1].type == rwd::EV_DMA);
    CHECK(rec.events[1].addr == 1u);
    CHECK(rec.events[1].value == 4u);
    CHECK(rec.events[1].len == 16u);
    CHECK(rec.events[2].value == 20u);
    CHECK(rec.events[2].len == 16u);
    CHECK(rec.events[3].value == 36u);
    CHECK(rec.events[3].len == 8u);
    CHECK(rec.events[3].data[7] == (u8)(100u + 36u + 7u));
    CHECK(hal.reads == 3u);
}

TEST_CASE("a DMA read with no backend, or none that does DMA, reads zeros") {
    u8 got[20];
    std::memset(got, 0xEE, sizeof(got));

    rwd::hal_reset();                        // no backend at all
    rwd::hal_dma_read(0, 0, got, sizeof(got));
    for (u32 i = 0; i < sizeof(got); ++i) {
        CHECK(got[i] == 0u);
    }

    FakeDma    hal;
    rwd::HalOps bare = *hal.ops();           // a backend that has never heard of DMA
    bare.dma_read    = 0;
    std::memset(got, 0xEE, sizeof(got));
    rwd::hal_attach(&bare);
    rwd::hal_dma_read(0, 0, got, sizeof(got));
    for (u32 i = 0; i < sizeof(got); ++i) {
        CHECK(got[i] == 0u);
    }
    rwd::hal_reset();
}

TEST_CASE("a DMA read outside MODE_RECORD returns the bytes and records nothing") {
    FakeDma hal;
    rwd::hal_reset();
    rwd::hal_attach(hal.ops());
    rwd::hal_mode_off();

    u8 got[4];
    rwd::hal_dma_read(0, 8, got, 4);
    CHECK(got[0] == 8u);
    CHECK(got[3] == 11u);
    rwd::hal_reset();
}

TEST_CASE("an interrupt may land between the pieces of a long read, and replays there") {
    FakeDma hal;
    hal.irqs.push_back(3u);

    u8 got[40];
    const Recorded rec = record_with(hal, [&]() {
        rwd::hal_dma_read(0u, 0u, got, sizeof(got));
    });
    CHECK(g_isr_calls == 1u);

    // The interrupt came after the first piece, which is where it was
    // delivered: right after an access, as always.
    REQUIRE(rec.events.size() >= 5u);
    CHECK(rec.events[0].type == rwd::EV_DMA);
    CHECK(rec.events[1].type == rwd::EV_IRQ_ENTER);
    CHECK(rec.events[2].type == rwd::EV_DMA);

    rwhost::ReplayHal replay;
    REQUIRE(replay.load(rec.trace.data(), rec.trace.size()));
    g_isr_calls = 0;
    rwd::hal_reset();
    rwd::hal_attach(replay.ops());
    rwd::hal_mode_replay();
    rwd::hal_set_isr(3u, &isr_count);

    u8 again[40];
    rwd::hal_dma_read(0u, 0u, again, sizeof(again));
    CHECK_FALSE(replay.diverged());
    CHECK(g_isr_calls == 1u);
    CHECK(std::memcmp(got, again, sizeof(got)) == 0);
    rwd::hal_reset();
}

TEST_CASE("replay hands back the bytes the device saw, whatever memory holds now") {
    FakeDma hal;
    u8      seen[10];
    const Recorded rec = record_with(hal, [&]() {
        rwd::hal_dma_read(0u, 20u, seen, sizeof(seen));
    });

    // From here on only the trace exists: the backend and its memory are gone.
    rwhost::ReplayHal replay;
    REQUIRE(replay.load(rec.trace.data(), rec.trace.size()));
    rwd::hal_reset();
    rwd::hal_attach(replay.ops());
    rwd::hal_mode_replay();

    u8 again[10];
    std::memset(again, 0xEE, sizeof(again));
    rwd::hal_dma_read(0u, 20u, again, sizeof(again));
    CHECK_FALSE(replay.diverged());
    CHECK(std::memcmp(seen, again, sizeof(seen)) == 0);
    rwd::hal_reset();
}

TEST_CASE("replay checks what the firmware reads against what was recorded") {
    FakeDma hal;
    u8      scratch[8];
    const Recorded rec = record_with(hal, [&]() {
        rwd::mmio_read32(0x40000000u);
        rwd::hal_dma_read(1u, 12u, scratch, 8);
    });

    rwhost::ReplayHal replay;
    REQUIRE(replay.load(rec.trace.data(), rec.trace.size()));

    auto fresh = [&]() {
        rwd::hal_reset();
        replay.restart();
        rwd::hal_attach(replay.ops());
        rwd::hal_mode_replay();
        rwd::mmio_read32(0x40000000u);
    };
    u8 got[8];

    SUBCASE("the right read replays") {
        fresh();
        rwd::hal_dma_read(1u, 12u, got, 8);
        CHECK_FALSE(replay.diverged());
    }
    SUBCASE("another region") {
        fresh();
        rwd::hal_dma_read(0u, 12u, got, 8);
        REQUIRE(replay.diverged());
        CHECK(replay.divergence().find("region 0") != std::string::npos);
        CHECK(replay.divergence().find("recorded 8 bytes of region 1 at 12") != std::string::npos);
        for (u32 i = 0; i < 8; ++i) {
            CHECK(got[i] == 0u);   // nothing real is handed back on a divergence
        }
    }
    SUBCASE("another offset") {
        fresh();
        rwd::hal_dma_read(1u, 13u, got, 8);
        CHECK(replay.diverged());
    }
    SUBCASE("another length") {
        fresh();
        rwd::hal_dma_read(1u, 12u, got, 6);
        CHECK(replay.diverged());
    }
    SUBCASE("a register read where the trace has a DMA read") {
        fresh();
        rwd::mmio_read32(0x40000000u);
        REQUIRE(replay.diverged());
        CHECK(replay.divergence().find("trace has DMA") != std::string::npos);
    }
    SUBCASE("a DMA read where the trace has a register read") {
        rwd::hal_reset();
        replay.restart();
        rwd::hal_attach(replay.ops());
        rwd::hal_mode_replay();
        rwd::hal_dma_read(1u, 12u, got, 8);     // the trace starts with a register read
        REQUIRE(replay.diverged());
        CHECK(replay.divergence().find("trace has MMIO_READ") != std::string::npos);
    }
    SUBCASE("a read past the end of the recording") {
        fresh();
        rwd::hal_dma_read(1u, 12u, got, 8);
        rwd::hal_dma_read(1u, 12u, got, 8);
        REQUIRE(replay.diverged());
        CHECK(replay.divergence().find("stopped") != std::string::npos);
    }
    rwd::hal_reset();
}

// -- the backends --

TEST_CASE("the device backend reads registered regions, and nothing else") {
    rwd::cortex_m::HardwareHal hw;

    volatile u8 ring[16];
    for (u32 i = 0; i < 16; ++i) {
        ring[i] = (u8)(0x40u + i);
    }
    volatile u8 other[4] = { 9, 8, 7, 6 };

    CHECK_FALSE(hw.add_dma_region(0, 0, 16));      // null base
    CHECK_FALSE(hw.add_dma_region(0, ring, 0));    // empty
    CHECK(hw.add_dma_region(5, ring, 16));
    CHECK_FALSE(hw.add_dma_region(5, other, 4));   // duplicate id
    CHECK(hw.add_dma_region(6, other, 4));
    CHECK(hw.add_dma_region(7, other, 4));
    CHECK(hw.add_dma_region(8, other, 4));
    CHECK_FALSE(hw.add_dma_region(9, other, 4));   // no room: kMaxDmaRegions

    const rwd::HalOps* ops = hw.ops();
    REQUIRE(ops->dma_read != 0);

    u8 got[4];
    ops->dma_read(ops->ctx, 5, 3, got, 4);
    CHECK(got[0] == 0x43u);
    CHECK(got[3] == 0x46u);

    ops->dma_read(ops->ctx, 6, 0, got, 4);
    CHECK(got[0] == 9u);
    CHECK(got[3] == 6u);

    // Unknown region, past the end, and an offset built to wrap the sum: zeros.
    std::memset(got, 0xEE, sizeof(got));
    ops->dma_read(ops->ctx, 99, 0, got, 4);
    CHECK(got[0] == 0u);
    std::memset(got, 0xEE, sizeof(got));
    ops->dma_read(ops->ctx, 5, 14, got, 4);
    CHECK(got[0] == 0u);
    std::memset(got, 0xEE, sizeof(got));
    ops->dma_read(ops->ctx, 5, 0xFFFFFFFEu, got, 4);
    CHECK(got[3] == 0u);
    // The last bytes are still reachable.
    ops->dma_read(ops->ctx, 5, 12, got, 4);
    CHECK(got[3] == 0x4Fu);
}

TEST_CASE("the simulator deposits into its ring, and the firmware finds out by the count") {
    sim::Mcu         off(1, 24);
    sim::Mcu         dma(1, 24);
    dma.enable_dma();

    rwd::hal_reset();
    rwd::hal_attach(dma.ops());
    u32 count = 0;
    // Each read costs three cycles and 24 bytes take several thousand to arrive.
    for (u32 i = 0; i < 6000 && count < 24; ++i) {
        count = rwd::mmio_read32(sim::kDma0Count);
    }
    rwd::hal_reset();
    CHECK(count == 24u);
    CHECK(dma.dma_deposited() == 24u);
    CHECK(dma.tx_sent() == 24u);

    // The ring holds the last kDmaRingSize bytes deposited; bytes before that
    // were overwritten.
    rwd::hal_attach(dma.ops());
    u8 ring[sim::kDmaRingSize];
    rwd::hal_dma_read(sim::kDmaRegion, 0, ring, sim::kDmaRingSize);
    rwd::hal_reset();
    bool all_zero = true;
    for (u32 i = 0; i < sim::kDmaRingSize; ++i) {
        all_zero = all_zero && ring[i] == 0;
    }
    CHECK_FALSE(all_zero);

    // Without enable_dma() there is no DMA: no count, no deposits.
    rwd::hal_attach(off.ops());
    for (u32 i = 0; i < 400; ++i) {
        CHECK(rwd::mmio_read32(sim::kDma0Count) == 0u);
    }
    rwd::hal_reset();
    CHECK(off.dma_deposited() == 0u);
}

// -- the example firmware's DMA receiver --

namespace {

rwhost::RunConfig dma_config(u64 seed) {
    rwhost::RunConfig cfg;
    cfg.seed    = seed;
    cfg.variant = fw::kDma;
    return cfg;
}

} // namespace

TEST_CASE("the DMA receiver loses nothing, and its trace carries the bytes it read") {
    for (u32 seed = 1; seed <= 40; ++seed) {
        CAPTURE(seed);
        const rwhost::RecordResult rec = rwhost::record_run(dma_config(seed));
        REQUIRE(rec.writer_ok);
        CHECK_FALSE(rec.bug_triggered());
        CHECK(rec.fw.consumed == rec.tx_sent);

        u32 dma_events = 0;
        u32 dma_bytes  = 0;
        for (const rwd::Event& e : decode(rec.trace)) {
            if (e.type == rwd::EV_DMA) {
                ++dma_events;
                dma_bytes += e.len;
                CHECK(e.addr == sim::kDmaRegion);
                CHECK(e.value + e.len <= sim::kDmaRingSize);
            }
        }
        CHECK(dma_events > 0u);
        CHECK(dma_bytes == rec.tx_sent);   // every byte read exactly once
    }
}

TEST_CASE("a DMA run replays exactly from its trace, over a seed sweep") {
    for (u32 seed = 1; seed <= 60; ++seed) {
        CAPTURE(seed);
        const rwhost::RunConfig    cfg = dma_config(seed);
        const rwhost::RecordResult rec = rwhost::record_run(cfg);
        REQUIRE(rec.writer_ok);

        // The simulator is gone; the trace is all there is.
        const rwhost::ReplayResult rep = rwhost::replay_run(rec.trace, cfg.main_iters);
        REQUIRE(rep.loaded);
        CHECK_FALSE(rep.diverged);
        CHECK(rep.fw.state_hash == rec.fw.state_hash);
        CHECK(rep.fw.consumed   == rec.fw.consumed);
        CHECK(rep.fw.checksum   == rec.fw.checksum);
        CHECK(rep.events_consumed == rep.events_total - 1u);
    }
}

TEST_CASE("replaying a DMA trace as a build that does not use DMA diverges") {
    const rwhost::RunConfig    cfg = dma_config(3);
    const rwhost::RecordResult rec = rwhost::record_run(cfg);
    const rwhost::ReplayResult rep =
        rwhost::replay_run_as(rec.trace, fw::kBuggy, cfg.main_iters);
    CHECK(rep.diverged);
}

TEST_CASE("the timeline restores a DMA run's state, DMA fields included") {
    const rwhost::RunConfig    cfg = dma_config(7);
    const rwhost::RecordResult rec = rwhost::record_run(cfg);
    const rwhost::ReplayResult rep = rwhost::replay_run(rec.trace, cfg.main_iters);
    REQUIRE(rep.loaded);

    rwhost::Timeline fine, coarse;
    REQUIRE(fine.open(rec.trace, cfg.main_iters, 64));
    REQUIRE(coarse.open(rec.trace, cfg.main_iters, 1000000));

    rwhost::Snapshot last;
    REQUIRE(fine.seek(fine.event_count(), &last));
    CHECK(last.state_hash == rep.fw.state_hash);
    CHECK(last.fw.dma_tail == rec.tx_sent);

    // A seek from a nearby checkpoint is indistinguishable from re-executing
    // from the start, at points all along the run.
    for (std::size_t i = 1; i <= 20; ++i) {
        const std::size_t at = fine.event_count() * i / 21u;
        rwhost::Snapshot a, b;
        REQUIRE(fine.seek(at, &a));
        REQUIRE(coarse.seek(at, &b));
        CAPTURE(at);
        CHECK(a.state_hash == b.state_hash);
        CHECK(a.fw.dma_tail == b.fw.dma_tail);
    }
}

namespace {

// A receiver that polls far too rarely, so the DMA laps it. The same ring and
// registers as the example firmware, with the unread bytes counted as lost.
struct SlowState {
    u32 tail = 0;
    u32 lost = 0;
    u32 sum  = 0;
};
SlowState g_slow;

void slow_receiver(u32 passes, u32 idle_between) {
    for (u32 p = 0; p < passes; ++p) {
        for (u32 i = 0; i < idle_between; ++i) {
            rwd::mmio_read32(sim::kSystickCnt);
        }
        const u32 count = rwd::mmio_read32(sim::kDma0Count);
        u32       avail = count - g_slow.tail;
        if (avail > sim::kDmaRingSize) {
            g_slow.lost += avail - sim::kDmaRingSize;
            g_slow.tail  = count - sim::kDmaRingSize;
            avail        = sim::kDmaRingSize;
        }
        u8        buf[sim::kDmaRingSize];
        const u32 index = g_slow.tail % sim::kDmaRingSize;
        const u32 first = avail < sim::kDmaRingSize - index ? avail : sim::kDmaRingSize - index;
        if (first > 0) {
            rwd::hal_dma_read(sim::kDmaRegion, index, buf, first);
        }
        if (avail > first) {
            rwd::hal_dma_read(sim::kDmaRegion, 0, buf + first, avail - first);
        }
        for (u32 i = 0; i < avail; ++i) {
            g_slow.sum = g_slow.sum * 31u + buf[i];
        }
        g_slow.tail += avail;
    }
}

} // namespace

TEST_CASE("a DMA overrun is recorded and replays: the lost bytes are the same ones") {
    // Between two looks the loop idles for 2500 reads, about 7500 cycles, in
    // which dozens of bytes arrive into a ring of sixteen.
    const u32 kIdleBetween = 2500;
    u32 lapped = 0;
    for (u32 seed = 1; seed <= 30; ++seed) {
        CAPTURE(seed);
        sim::Mcu           mcu(seed, 60);
        mcu.enable_dma();
        rwhost::VectorSink sink;
        rwd::TraceWriter   writer;
        std::vector<u8>    block(256);

        g_slow = SlowState();
        rwd::hal_reset();
        rwd::hal_attach(mcu.ops());
        REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block.data(),
                             (u32)block.size(), 1, seed, rwd::kFlagRecordWrites));
        rwd::hal_mode_record(&writer);
        slow_receiver(8, kIdleBetween);
        REQUIRE(writer.finish(rwd::hal_now()));
        rwd::hal_reset();
        const SlowState recorded = g_slow;
        if (recorded.lost > 0u) {
            ++lapped;
        }

        rwhost::ReplayHal replay;
        REQUIRE(replay.load(sink.bytes.data(), sink.bytes.size()));
        g_slow = SlowState();
        rwd::hal_reset();
        rwd::hal_attach(replay.ops());
        rwd::hal_mode_replay();
        slow_receiver(8, kIdleBetween);
        rwd::hal_reset();

        CHECK_FALSE(replay.diverged());
        CHECK(g_slow.lost == recorded.lost);
        CHECK(g_slow.tail == recorded.tail);
        CHECK(g_slow.sum  == recorded.sum);
    }
    CHECK(lapped > 0u);   // the sweep did exercise the overrun
}
