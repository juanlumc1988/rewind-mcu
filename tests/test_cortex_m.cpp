// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The parts of the Cortex-M backend that are arithmetic on a stack frame
// rather than register access, and so run on the host. The naked trampoline
// that finds the frame in the first place is ARM assembly and cannot be
// tested here; it is built by the cross job and run in an emulator by hand.

#include "doctest.h"

#include <vector>

#include "rewind/cortex_m.h"
#include "rewind/hal.h"
#include "rewind/replay.h"
#include "rewind/trace_reader.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

// R0 R1 R2 R3 R12 LR PC xPSR, as the core pushes them.
const u32 kFrame[8] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u,
    0x0000000Cu, 0x080001A5u, 0x08000500u, 0x61000000u
};

u64 fake_now(void* ctx) {
    u64* clock = static_cast<u64*>(ctx);
    *clock += 10;
    return *clock;
}

u32 g_isr_calls = 0;
void isr_counting() { ++g_isr_calls; }

std::vector<rwd::Event> record_entry(const u32* frame, u32 vector) {
    u64                clock = 0;
    rwd::HalOps        ops   = { 0, 0, &fake_now, 0, &clock };
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[rwd::kMinBlockBuffer];

    g_isr_calls = 0;
    rwd::hal_reset();
    rwd::hal_attach(&ops);
    rwd::hal_set_isr(vector, &isr_counting);
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block,
                         sizeof(block), 1, 2, 0));
    rwd::hal_mode_record(&writer);

    rwd_cortex_m_isr_entry(vector, frame);

    REQUIRE(writer.finish(rwd::hal_now()));
    rwd::hal_reset();

    std::vector<rwd::Event> out;
    rwd::TraceReader        reader;
    REQUIRE(reader.open(sink.bytes.data(), (u32)sink.bytes.size()));
    rwd::Event ev;
    while (reader.next(&ev)) {
        out.push_back(ev);
    }
    CHECK(reader.ok());
    return out;
}

} // namespace

TEST_CASE("the return address is the seventh word of the frame") {
    CHECK(rwd::cortex_m::kFramePcIndex == 6u);
    CHECK(rwd::cortex_m::stacked_pc(kFrame) == 0x08000500u);
}

TEST_CASE("EXC_RETURN bit 2 picks the stack the frame was pushed to") {
    const u32 msp_frame[8] = { 0 };
    const u32 psp_frame[8] = { 0 };

    // Thread mode on MSP, handler mode (nested), thread mode on PSP; the
    // FPU-frame variants differ in bit 4 only and must not change the answer.
    CHECK(rwd::cortex_m::exception_frame(0xFFFFFFF9u, msp_frame, psp_frame) == msp_frame);
    CHECK(rwd::cortex_m::exception_frame(0xFFFFFFF1u, msp_frame, psp_frame) == msp_frame);
    CHECK(rwd::cortex_m::exception_frame(0xFFFFFFE9u, msp_frame, psp_frame) == msp_frame);
    CHECK(rwd::cortex_m::exception_frame(0xFFFFFFFDu, msp_frame, psp_frame) == psp_frame);
    CHECK(rwd::cortex_m::exception_frame(0xFFFFFFEDu, msp_frame, psp_frame) == psp_frame);
}

TEST_CASE("a vector entry records where the interrupt landed, then runs the handler") {
    const std::vector<rwd::Event> events = record_entry(kFrame, 1u);

    CHECK(g_isr_calls == 1u);
    REQUIRE(events.size() == 2u);
    CHECK(events[0].type  == rwd::EV_IRQ_AT);
    CHECK(events[0].addr  == 1u);
    CHECK(events[0].value == 0x08000500u);
    CHECK(events[1].type  == rwd::EV_STOP);
}

TEST_CASE("a vector entry without a frame still records the interrupt") {
    const std::vector<rwd::Event> events = record_entry(0, 2u);

    CHECK(g_isr_calls == 1u);
    REQUIRE(events.size() == 2u);
    CHECK(events[0].type  == rwd::EV_IRQ_ENTER);
    CHECK(events[0].addr  == 2u);
    CHECK(events[0].value == 0u);
}
