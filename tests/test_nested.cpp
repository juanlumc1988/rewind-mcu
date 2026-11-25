// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Interrupts that preempt each other, by priority.
//
// Two layers, because they fail differently. The shim is checked alone, against
// a backend that does nothing but hold a set of pending vectors: who runs first,
// who preempts whom, how deep it goes, what masking and reset do, and that a
// backend that knows nothing of priorities still gets the old behaviour. Then
// the whole path -- the simulator with a timer beside the UART, a recording, a
// replay from the trace alone -- to show that a run in which a handler was
// interrupted comes back identically, and that replaying it with different
// priorities is noticed rather than quietly giving a different run.

#include "doctest.h"

#include <string>
#include <vector>

#include "rewind/hal.h"
#include "rewind/replay.h"
#include "rewind/trace_reader.h"
#include "rewind/trace_writer.h"
#include "sim/mcu.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

// -- a backend that is only a set of pending vectors --

struct Scripted {
    u32 pending;                   // bit v set: vector v is waiting
    u32 reads;                     // register reads so far
    std::vector<std::pair<u32, u32> > raise;   // at the Nth read, set these bits
    bool  knows_priorities;
    rwd::HalOps ops;

    Scripted() : pending(0), reads(0), knows_priorities(true) {
        ops.read32   = &Scripted::read32;
        ops.write32  = &Scripted::write32;
        ops.now      = &Scripted::now;
        ops.poll_irq = &Scripted::poll;
        ops.ctx      = this;
    }
    void use_priorities(bool yes) {
        knows_priorities = yes;
        ops.poll_irq_pri = yes ? &Scripted::poll_pri : 0;
    }

    static u32 read32(void* c, u32) {
        Scripted* s = static_cast<Scripted*>(c);
        ++s->reads;
        for (std::size_t i = 0; i < s->raise.size(); ++i) {
            if (s->raise[i].first == s->reads) {
                s->pending |= s->raise[i].second;
            }
        }
        return 0;
    }
    static void write32(void*, u32, u32) {}
    static u64 now(void* c) { return static_cast<Scripted*>(c)->reads * 3ULL; }

    // What a backend that predates priorities does: lowest vector first.
    static u32 poll(void* c) {
        Scripted* s = static_cast<Scripted*>(c);
        for (u32 v = 0; v < rwd::kMaxIrqVectors; ++v) {
            if ((s->pending & (1u << v)) != 0) {
                s->pending &= ~(1u << v);
                return v;
            }
        }
        return rwd::kNoIrq;
    }

    // The most urgent pending vector that is more urgent than `level`.
    static u32 poll_pri(void* c, const u8* prio, u32 level) {
        Scripted* s = static_cast<Scripted*>(c);
        u32 best = rwd::kNoIrq;
        for (u32 v = 0; v < rwd::kMaxIrqVectors; ++v) {
            if ((s->pending & (1u << v)) != 0 && (u32)prio[v] < level &&
                (best == rwd::kNoIrq || prio[v] < prio[best])) {
                best = v;
            }
        }
        if (best != rwd::kNoIrq) {
            s->pending &= ~(1u << best);
        }
        return best;
    }
};

std::string g_log;          // "1<" is vector 1's handler starting, "1>" its end
u32         g_max_depth;
u32         g_level_in[rwd::kMaxIrqVectors];

void note(u32 v, bool entering) {
    g_log += (char)('0' + v);
    g_log += entering ? '<' : '>';
}

// Each handler touches a register, because that is where the shim looks for
// something more urgent -- and is the only place it can.
template <u32 V>
void handler() {
    note(V, true);
    g_level_in[V] = rwd::hal_irq_level();
    if (rwd::hal_irq_depth() > g_max_depth) {
        g_max_depth = rwd::hal_irq_depth();
    }
    (void)rwd::mmio_read32(0x1000u + V);
    note(V, false);
}

void install(u32 p1, u32 p2, u32 p3) {
    rwd::hal_reset();
    g_log.clear();
    g_max_depth = 0;
    for (u32 i = 0; i < rwd::kMaxIrqVectors; ++i) {
        g_level_in[i] = 0xDEADu;
    }
    rwd::hal_set_isr(1, &handler<1>);
    rwd::hal_set_isr(2, &handler<2>);
    rwd::hal_set_isr(3, &handler<3>);
    rwd::hal_set_irq_priority(1, p1);
    rwd::hal_set_irq_priority(2, p2);
    rwd::hal_set_irq_priority(3, p3);
}

// Main loop: a few reads, one of which raises vector 1; the handlers raise the
// others as they run.
void run_main(Scripted* s) {
    rwd::hal_attach(&s->ops);
    for (int i = 0; i < 4; ++i) {
        (void)rwd::mmio_read32(0x2000u);
    }
}

// Read 1 (main) raises vector 1; read 2 (inside 1) raises 2; read 3 (inside 2)
// raises 3.
void chain(Scripted* s) {
    s->raise.push_back(std::make_pair(1u, 1u << 1));
    s->raise.push_back(std::make_pair(2u, 1u << 2));
    s->raise.push_back(std::make_pair(3u, 1u << 3));
}

} // namespace

TEST_CASE("by default nothing preempts anything") {
    Scripted s;
    s.use_priorities(true);
    chain(&s);
    install(0, 0, 0);
    run_main(&s);
    // Each runs to the end before the next starts, in the order they arrived.
    CHECK(g_log == "1<1>2<2>3<3>");
    CHECK(g_max_depth == 1u);
    rwd::hal_reset();
}

TEST_CASE("a more urgent interrupt preempts a handler, at any depth") {
    Scripted s;
    s.use_priorities(true);
    chain(&s);
    install(3, 2, 1);                   // 3 is the most urgent
    run_main(&s);
    // 1 is running when 2 arrives; 2 when 3 does; 3 finishes first.
    CHECK(g_log == "1<2<3<3>2>1>");
    CHECK(g_max_depth == 3u);
    CHECK(g_level_in[1] == 3u);
    CHECK(g_level_in[2] == 2u);
    CHECK(g_level_in[3] == 1u);
    // And when it is all over, there is no handler.
    CHECK(rwd::hal_irq_depth() == 0u);
    CHECK(rwd::hal_irq_level() == rwd::kThreadLevel);
    rwd::hal_reset();
}

TEST_CASE("one that is no more urgent waits for the handler to finish") {
    // 2 arrives while 1 runs, at the same priority: it must wait. 3 arrives
    // while 2 runs, and is less urgent: it waits too.
    {
        Scripted s;
        s.use_priorities(true);
        chain(&s);
        install(2, 2, 5);
        run_main(&s);
        CHECK(g_log == "1<1>2<2>3<3>");
        rwd::hal_reset();
    }
    // 2 is more urgent than 1 and preempts it; 3 is less urgent than 2 and
    // waits for it, then also for 1, which it does not preempt either.
    {
        Scripted s;
        s.use_priorities(true);
        chain(&s);
        install(2, 1, 4);
        run_main(&s);
        CHECK(g_log == "1<2<2>1>3<3>");
        rwd::hal_reset();
    }
}

TEST_CASE("pending interrupts run most urgent first, then by vector number") {
    Scripted s;
    s.use_priorities(true);
    s.raise.push_back(std::make_pair(1u, (1u << 1) | (1u << 2) | (1u << 3)));
    install(3, 1, 2);
    run_main(&s);
    CHECK(g_log == "2<2>3<3>1<1>");
    rwd::hal_reset();

    // Equal priority: the lower vector goes first, as on a Cortex-M.
    Scripted t;
    t.use_priorities(true);
    t.raise.push_back(std::make_pair(1u, (1u << 1) | (1u << 2) | (1u << 3)));
    install(1, 1, 1);
    run_main(&t);
    CHECK(g_log == "1<1>2<2>3<3>");
    rwd::hal_reset();
}

TEST_CASE("a backend that knows nothing of priorities is served as before") {
    Scripted s;
    s.use_priorities(false);
    chain(&s);
    install(3, 2, 1);                   // set, and not used: nothing can act on them
    run_main(&s);
    CHECK(g_log == "1<1>2<2>3<3>");
    CHECK(g_max_depth == 1u);
    rwd::hal_reset();
}

TEST_CASE("masking holds back preemption, and it arrives when the mask is lifted") {
    Scripted s;
    s.use_priorities(true);
    s.raise.push_back(std::make_pair(1u, 1u << 1));
    s.raise.push_back(std::make_pair(2u, 1u << 2));
    install(2, 1, 0);

    // A handler for 1 that masks around its first access.
    struct Masked {
        static void h1() {
            note(1, true);
            rwd::hal_irq_disable();
            (void)rwd::mmio_read32(0x1001u);          // 2 raised, but masked
            rwd::hal_irq_enable();
            note(1, false);                           // ... so 1 finishes first
        }
    };
    rwd::hal_set_isr(1, &Masked::h1);
    run_main(&s);
    CHECK(g_log == "1<1>2<2>");
    rwd::hal_reset();
}

TEST_CASE("priorities are clamped, ignored for vectors that do not exist, and reset") {
    Scripted s;
    s.use_priorities(true);
    s.raise.push_back(std::make_pair(1u, 1u << 1));
    install(1000, 0, 0);                // taken as the least urgent a priority can be
    rwd::hal_set_irq_priority(99, 1);   // no such vector: nothing happens
    run_main(&s);
    CHECK(g_log == "1<1>");             // still dispatched from the main loop
    CHECK(g_level_in[1] == rwd::kMaxPriority);
    CHECK(g_level_in[1] < rwd::kThreadLevel);

    // After a reset they are all back to zero, and nothing nests.
    rwd::hal_reset();
    Scripted t;
    t.use_priorities(true);
    chain(&t);
    install(0, 0, 0);
    rwd::hal_reset();                   // clears them and the handlers
    rwd::hal_set_isr(1, &handler<1>);
    rwd::hal_set_isr(2, &handler<2>);
    rwd::hal_set_isr(3, &handler<3>);
    g_log.clear();
    run_main(&t);
    CHECK(g_log == "1<1>2<2>3<3>");
    rwd::hal_reset();
}

// ---------------------------------------------------------------------------
// The whole path: simulator, recording, replay.
// ---------------------------------------------------------------------------

namespace {

struct Counters {
    u32 uart_bytes;
    u32 timer_ticks;
    u32 checksum;
    Counters() : uart_bytes(0), timer_ticks(0), checksum(0) {}
};
Counters g_c;

// The receive handler is slow -- several accesses -- so the timer has time to
// go off in the middle of it.
void uart_isr() {
    note(1, true);
    for (int i = 0; i < 4; ++i) {
        (void)rwd::mmio_read32(sim::kSystickCnt);
    }
    const u32 byte = rwd::mmio_read32(sim::kUart0Dr);
    ++g_c.uart_bytes;
    g_c.checksum = g_c.checksum * 31u + byte;
    note(1, false);
}

void timer_isr() {
    note(2, true);
    ++g_c.timer_ticks;
    g_c.checksum = g_c.checksum * 17u + (rwd::mmio_read32(sim::kSystickCnt) & 0xFFu);
    note(2, false);
}

void setup(u32 uart_priority, u32 timer_priority) {
    rwd::hal_reset();
    g_log.clear();
    g_c = Counters();
    rwd::hal_set_isr(sim::kIrqUartRx, &uart_isr);
    rwd::hal_set_isr(sim::kIrqTimer, &timer_isr);
    rwd::hal_set_irq_priority(sim::kIrqUartRx, uart_priority);
    rwd::hal_set_irq_priority(sim::kIrqTimer, timer_priority);
}

void firmware_main(u32 iters) {
    for (u32 i = 0; i < iters; ++i) {
        (void)rwd::mmio_read32(sim::kUart0Sr);
        (void)rwd::mmio_read32(sim::kSystickCnt);
    }
}

const u32 kIters      = 1500;
const u32 kTimerEvery = 40;

struct Recorded {
    std::vector<u8> trace;
    std::string     log;
    Counters        c;
    u32             uart_sent;
};

Recorded record(u64 seed, u32 uart_priority, u32 timer_priority) {
    sim::Mcu mcu(seed, 16);
    mcu.enable_timer(kTimerEvery);
    rwhost::VectorSink sink;
    rwd::TraceWriter   writer;
    u8                 block[256];

    setup(uart_priority, timer_priority);
    rwd::hal_attach(mcu.ops());
    REQUIRE(writer.begin(&rwhost::VectorSink::write, &sink, block, sizeof(block), 0xE57ULL, seed,
                         rwd::kFlagRecordWrites));
    rwd::hal_mode_record(&writer);
    firmware_main(kIters);
    writer.finish(rwd::hal_now());
    rwd::hal_mode_off();
    REQUIRE(writer.ok());

    Recorded r;
    r.trace     = sink.bytes;
    r.log       = g_log;
    r.c         = g_c;
    r.uart_sent = mcu.tx_sent();
    rwd::hal_reset();
    return r;
}

// True if the log has a UART handler interrupted by the timer's.
bool nested(const std::string& log) {
    return log.find("1<2<2>1>") != std::string::npos;
}

} // namespace

TEST_CASE("a handler interrupted by a more urgent one is recorded, and replays alone") {
    // The timer is more urgent than the UART. Across some seeds it goes off
    // while a receive handler is in the middle of its accesses.
    u32 seeds_nested = 0;
    for (u64 seed = 1; seed <= 8; ++seed) {
        const Recorded rec = record(seed, 2, 1);
        CAPTURE(seed);
        CHECK(rec.c.uart_bytes == rec.uart_sent);       // nothing lost to the nesting
        CHECK(rec.c.timer_ticks > 20u);
        if (nested(rec.log)) {
            ++seeds_nested;
        }

        // Replayed from the trace alone, after the simulator is gone.
        rwhost::ReplayHal hal;
        REQUIRE(hal.load(rec.trace.data(), rec.trace.size()));
        setup(2, 1);
        rwd::hal_attach(hal.ops());
        rwd::hal_mode_replay();
        firmware_main(kIters);

        CHECK_FALSE(hal.diverged());
        CHECK(hal.divergence().empty());
        CHECK(hal.cursor() + 1 == hal.event_count());   // all of it, up to the stop
        CHECK(g_log == rec.log);
        CHECK(g_c.uart_bytes == rec.c.uart_bytes);
        CHECK(g_c.timer_ticks == rec.c.timer_ticks);
        CHECK(g_c.checksum == rec.c.checksum);
        rwd::hal_reset();
    }
    // If no seed ever nested, the rest of this proves nothing.
    CHECK(seeds_nested >= 4u);
}

TEST_CASE("the same firmware without priorities never nests") {
    for (u64 seed = 1; seed <= 8; ++seed) {
        const Recorded rec = record(seed, 0, 0);
        CAPTURE(seed);
        CHECK_FALSE(nested(rec.log));
        CHECK(rec.c.uart_bytes == rec.uart_sent);
    }
}

TEST_CASE("replaying with other priorities than the recording is noticed") {
    u32 noticed = 0, tried = 0;
    for (u64 seed = 1; seed <= 8; ++seed) {
        const Recorded rec = record(seed, 2, 1);
        if (!nested(rec.log)) {
            continue;
        }
        ++tried;

        // The wrong way round: now the UART is the more urgent, so the timer
        // cannot preempt it where the recording says it did.
        rwhost::ReplayHal hal;
        REQUIRE(hal.load(rec.trace.data(), rec.trace.size()));
        setup(1, 2);
        rwd::hal_attach(hal.ops());
        rwd::hal_mode_replay();
        firmware_main(kIters);
        if (hal.diverged()) {
            ++noticed;
        }
        rwd::hal_reset();
    }
    CHECK(tried >= 4u);
    CHECK(noticed == tried);
}

TEST_CASE("a trace recorded with nesting reads back as the events it was") {
    const Recorded rec = record(3, 2, 1);
    REQUIRE(nested(rec.log));

    rwd::TraceReader reader;
    REQUIRE(reader.open(rec.trace.data(), (u32)rec.trace.size()));
    rwd::Event ev;
    u32 irqs = 0, timers = 0, uarts = 0;
    while (reader.next(&ev)) {
        if (ev.type == rwd::EV_IRQ_ENTER) {
            ++irqs;
            timers += (ev.addr == sim::kIrqTimer) ? 1u : 0u;
            uarts  += (ev.addr == sim::kIrqUartRx) ? 1u : 0u;
        }
    }
    CHECK(reader.ok());
    CHECK(irqs == timers + uarts);
    CHECK(uarts == rec.uart_sent);
    CHECK(timers == rec.c.timer_ticks);
}
