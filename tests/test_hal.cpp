// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Exercises the shim against a hand-built fake backend, without the
// simulator or the example firmware in the way.

#include "doctest.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "rewind/hal.h"
#include "rewind/replay.h"
#include "rewind/trace_reader.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

struct FakeHal {
    std::vector<u32>                 reads;
    std::vector<std::pair<u32, u32>> writes;
    std::deque<u32>                  irqs;   // vectors waiting to be delivered
    u64                              clock = 0;
    u32                              value = 0xA5A5A5A5u;

    FakeHal() {
        ops_.read32   = &FakeHal::s_read32;
        ops_.write32  = &FakeHal::s_write32;
        ops_.now      = &FakeHal::s_now;
        ops_.poll_irq = &FakeHal::s_poll_irq;
        ops_.ctx      = this;
    }

    const rwd::HalOps* ops() const { return &ops_; }

    static u32 s_read32(void* ctx, u32 addr) {
        FakeHal* self = static_cast<FakeHal*>(ctx);
        self->clock += 10;
        self->reads.push_back(addr);
        return self->value;
    }
    static void s_write32(void* ctx, u32 addr, u32 v) {
        FakeHal* self = static_cast<FakeHal*>(ctx);
        self->clock += 10;
        self->writes.push_back(std::make_pair(addr, v));
    }
    static u64 s_now(void* ctx) { return static_cast<FakeHal*>(ctx)->clock; }
    static u32 s_poll_irq(void* ctx) {
        FakeHal* self = static_cast<FakeHal*>(ctx);
        if (self->irqs.empty()) {
            return rwd::kNoIrq;
        }
        const u32 vector = self->irqs.front();
        self->irqs.pop_front();
        return vector;
    }

private:
    rwd::HalOps ops_;
};

u32 g_isr_calls    = 0;
u32 g_isr_nested   = 0;   // MMIO accesses made from inside the handler

void isr_counting() {
    ++g_isr_calls;
}

void isr_that_touches_mmio() {
    ++g_isr_calls;
    rwd::mmio_read32(0xDEAD0000u);
    ++g_isr_nested;
}

// Fresh shim, fresh counters.
void arm(FakeHal& hal) {
    g_isr_calls  = 0;
    g_isr_nested = 0;
    rwd::hal_reset();
    rwd::hal_attach(hal.ops());
}

std::vector<rwd::Event> decode(const std::vector<u8>& bytes) {
    std::vector<rwd::Event> out;
    rwd::TraceReader        reader;
    REQUIRE(reader.open(bytes.data(), (u32)bytes.size()));
    rwd::Event ev;
    while (reader.next(&ev)) {
        out.push_back(ev);
    }
    CHECK(reader.ok());
    return out;
}

} // namespace

TEST_CASE("MODE_OFF passes accesses through and records nothing") {
    FakeHal hal;
    arm(hal);
    rwd::hal_mode_off();

    CHECK(rwd::mmio_read32(0x40000000u) == 0xA5A5A5A5u);
    rwd::mmio_write32(0x40002000u, 7u);

    CHECK(hal.reads.size() == 1u);
    CHECK(hal.writes.size() == 1u);
    CHECK(rwd::hal_mode() == rwd::MODE_OFF);
    rwd::hal_reset();
}

TEST_CASE("MODE_RECORD captures reads, writes and interrupts in order") {
    FakeHal             hal;
    rwhost::VectorSink  sink;
    rwd::TraceWriter writer;
    u8                  block[rwd::kMinBlockBuffer];

    arm(hal);
    rwd::hal_set_isr(3u, &isr_counting);
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block,
                         sizeof(block), 1, 2, rwd::kFlagRecordWrites));
    rwd::hal_mode_record(&writer);

    hal.value = 0x1234u;
    rwd::mmio_read32(0x40000000u);   // no interrupt pending

    hal.irqs.push_back(3u);
    rwd::mmio_write32(0x40002000u, 0x99u);   // interrupt lands here

    REQUIRE(writer.finish(rwd::hal_now()));
    rwd::hal_reset();

    CHECK(g_isr_calls == 1u);

    const std::vector<rwd::Event> events = decode(sink.bytes);
    REQUIRE(events.size() == 4u);

    CHECK(events[0].type  == rwd::EV_MMIO_READ);
    CHECK(events[0].addr  == 0x40000000u);
    CHECK(events[0].value == 0x1234u);

    CHECK(events[1].type  == rwd::EV_MMIO_WRITE);
    CHECK(events[1].addr  == 0x40002000u);
    CHECK(events[1].value == 0x99u);

    // The interrupt is recorded after the access it followed, which is the
    // order replay will hand it back.
    CHECK(events[2].type == rwd::EV_IRQ_ENTER);
    CHECK(events[2].addr == 3u);

    CHECK(events[3].type == rwd::EV_STOP);
}

TEST_CASE("a masked interrupt is neither dispatched nor recorded") {
    FakeHal             hal;
    rwhost::VectorSink  sink;
    rwd::TraceWriter writer;
    u8                  block[rwd::kMinBlockBuffer];

    arm(hal);
    rwd::hal_set_isr(1u, &isr_counting);
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block,
                         sizeof(block), 1, 2, rwd::kFlagRecordWrites));
    rwd::hal_mode_record(&writer);

    rwd::hal_irq_disable();
    hal.irqs.push_back(1u);
    rwd::mmio_read32(0x40000000u);
    CHECK(g_isr_calls == 0u);

    rwd::hal_irq_enable();
    rwd::mmio_read32(0x40000000u);   // now it gets through
    CHECK(g_isr_calls == 1u);

    REQUIRE(writer.finish(rwd::hal_now()));
    rwd::hal_reset();

    const std::vector<rwd::Event> events = decode(sink.bytes);
    u32 irq_events = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].type == rwd::EV_IRQ_ENTER) {
            ++irq_events;
        }
    }
    // Exactly one: a critical section must look the same in the trace as it
    // did on the wire, or replay would deliver an interrupt the recorded run
    // never took.
    CHECK(irq_events == 1u);
}

TEST_CASE("interrupt masking nests") {
    FakeHal hal;
    arm(hal);
    rwd::hal_set_isr(1u, &isr_counting);
    rwd::hal_mode_off();

    rwd::hal_irq_disable();
    rwd::hal_irq_disable();
    hal.irqs.push_back(1u);

    rwd::mmio_read32(0u);
    CHECK(g_isr_calls == 0u);

    rwd::hal_irq_enable();       // still masked: depth 1
    rwd::mmio_read32(0u);
    CHECK(g_isr_calls == 0u);

    rwd::hal_irq_enable();       // unmasked
    rwd::mmio_read32(0u);
    CHECK(g_isr_calls == 1u);
    rwd::hal_reset();
}

TEST_CASE("handlers do not re-enter through their own MMIO accesses") {
    FakeHal hal;
    arm(hal);
    rwd::hal_set_isr(2u, &isr_that_touches_mmio);
    rwd::hal_mode_off();

    // Two pending interrupts and a handler that itself does MMIO. Without
    // the in-handler guard the second would be dispatched from inside the
    // first, and a deep enough queue would blow the stack -- on a part with
    // 8 KiB of RAM, that is a reset, not a stack trace.
    hal.irqs.push_back(2u);
    hal.irqs.push_back(2u);
    rwd::mmio_read32(0x40000000u);

    CHECK(g_isr_calls == 2u);
    CHECK(g_isr_nested == 2u);   // both handlers ran to completion
    rwd::hal_reset();
}

TEST_CASE("an unhandled vector is still recorded") {
    FakeHal             hal;
    rwhost::VectorSink  sink;
    rwd::TraceWriter writer;
    u8                  block[rwd::kMinBlockBuffer];

    arm(hal);
    // No ISR installed for vector 5.
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block,
                         sizeof(block), 1, 2, 0));
    rwd::hal_mode_record(&writer);

    hal.irqs.push_back(5u);
    rwd::mmio_read32(0x40000000u);

    REQUIRE(writer.finish(rwd::hal_now()));
    rwd::hal_reset();

    const std::vector<rwd::Event> events = decode(sink.bytes);
    bool found = false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].type == rwd::EV_IRQ_ENTER && events[i].addr == 5u) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("accesses with no backend attached are inert") {
    rwd::hal_reset();
    CHECK(rwd::mmio_read32(0x40000000u) == 0u);
    rwd::mmio_write32(0x40000000u, 1u);   // must not crash
    CHECK(rwd::hal_now() == 0u);
}

TEST_CASE("divergence latches the first report") {
    rwd::hal_reset();
    CHECK_FALSE(rwd::hal_diverged());

    rwd::hal_set_divergence("first");
    rwd::hal_set_divergence("second");

    CHECK(rwd::hal_diverged());
    CHECK(std::string(rwd::hal_divergence()) == "first");

    rwd::hal_reset();
    CHECK_FALSE(rwd::hal_diverged());
    CHECK(std::string(rwd::hal_divergence()) == "");
}
