// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Runs the Cortex-M interrupt trampoline (RWD_CORTEX_M_ISR) on an emulated
// Cortex-M4, which the host unit tests cannot do: it is ARM assembly that
// reads the exception frame the core pushed.
//
// It pends an interrupt with a store, lets it fire, and checks that the
// recorded trace holds an EV_IRQ_AT whose return address lies where the
// architecture says it must, with the frame on the main stack and again on
// the process stack. Output goes out through semihosting; the exit status is
// the verdict. See run.sh.
//
// Not a model of a real part: QEMU's NVIC has no DWT->CYCCNT, so the clock
// here is a counter, and nothing in this file says anything about timing.

#include "rewind/cortex_m.h"
#include "rewind/hal.h"
#include "rewind/trace_reader.h"
#include "rewind/trace_writer.h"

using rwd::u8; using rwd::u32; using rwd::u64;

extern "C" {
extern u32 _estack, _sbss, _ebss, _sdata, _edata, _sidata;
extern char landing_label[];
extern char after_store[];
void reset_handler();
void IRQ0_Handler();
void default_handler() { for (;;) {} }
}

typedef void (*Vec)();
__attribute__((section(".vectors"), used)) const Vec g_vectors[17] = {
    (Vec)&_estack, reset_handler,
    default_handler, default_handler, default_handler, default_handler,
    default_handler, default_handler, default_handler, default_handler,
    default_handler, default_handler, default_handler, default_handler,
    default_handler, default_handler,
    IRQ0_Handler
};

// -- semihosting --
static int semihost(int op, const void* arg) {
    register int r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void say(const char* s) { semihost(0x04, s); }
static void say_hex(u32 v) {
    char b[11]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; ++i) { b[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 0xF]; }
    b[10] = 0; say(b);
}
static void finish_run(u32 code) {
    u32 blk[2] = { 0x20026u, code };
    semihost(0x20, blk);          // SYS_EXIT_EXTENDED
    for (;;) {}
}

// -- the run --
static volatile u32 g_isr_calls;
static u64 g_clock;
static u8  g_trace[512];
static u32 g_trace_len;
static u8  g_block[64];
__attribute__((aligned(8))) static u8 g_psp_stack[1024];

static bool sink(void*, const u8* d, u32 n) {
    if (g_trace_len + n > sizeof(g_trace)) return false;
    for (u32 i = 0; i < n; ++i) g_trace[g_trace_len + i] = d[i];
    g_trace_len += n; return true;
}
static u32  rd(void*, u32 a) { return rwd::cortex_m::raw_read32(a); }
static void wr(void*, u32 a, u32 v) { rwd::cortex_m::raw_write32(a, v); }
static u64  nw(void*) { return ++g_clock; }
static u32  pl(void*) { return rwd::kNoIrq; }
static void my_isr() { ++g_isr_calls; }

// Pends IRQ0 and lets it fire. By the architecture, after DSB+ISB the
// exception is taken before the instruction at landing_label executes, so
// the stacked PC must be landing_label.
extern "C" __attribute__((noinline)) void trigger() {
    // The store pends IRQ0. The architecture lets the exception be taken any
    // time after the store and guarantees it by the end of DSB+ISB, so the
    // stacked PC must lie in [after_store, landing_label].
    __asm__ volatile(
        "str   %0, [%1]              \n"
        ".global after_store         \n"
        "after_store:                \n"
        "dsb                         \n"
        "isb                         \n"
        ".global landing_label       \n"
        "landing_label:              \n"
        "nop                         \n"
        :: "r"(1u), "r"(0xE000E200u) : "memory");     // NVIC_ISPR0
}

// fn is r0 and psp_top is r1; the asm below is the whole function.
__attribute__((naked)) void run_on_psp(__attribute__((unused)) void (*fn)(),
                                       __attribute__((unused)) u32 psp_top) {
    __asm__ volatile(
        "push {r4, lr}   \n"
        "mov  r4, r0     \n"
        "msr  psp, r1    \n"
        "movs r2, #2     \n"
        "msr  control, r2\n"
        "isb             \n"
        "blx  r4         \n"
        "movs r2, #0     \n"
        "msr  control, r2\n"
        "isb             \n"
        "pop  {r4, pc}   \n");
}

static bool one_run(const char* name, bool psp) {
    static const rwd::HalOps ops = { &rd, &wr, &nw, &pl, 0 };
    rwd::TraceWriter writer;
    g_trace_len = 0; g_clock = 0; g_isr_calls = 0;

    rwd::hal_reset();
    rwd::hal_attach(&ops);
    rwd::hal_set_isr(1u, &my_isr);
    if (!writer.begin(&sink, 0, g_block, sizeof(g_block), 1, 2, 0)) return false;
    rwd::hal_mode_record(&writer);

    rwd::cortex_m::raw_write32(0xE000E100u, 1u);           // NVIC_ISER0: enable IRQ0
    __asm__ volatile("cpsie i" ::: "memory");
    if (psp) run_on_psp(&trigger, (u32)(g_psp_stack + sizeof(g_psp_stack)));
    else     trigger();

    writer.finish(rwd::hal_now());
    rwd::hal_reset();

    say(name); say(": isr_calls=");  say_hex(g_isr_calls);
    rwd::TraceReader r; rwd::Event ev; bool found = false; u32 pc = 0, vec = 0;
    if (r.open(g_trace, g_trace_len)) {
        while (r.next(&ev)) if (ev.type == rwd::EV_IRQ_AT) { found = true; pc = ev.value; vec = ev.addr; }
    }
    const u32 lo = (u32)after_store, hi = (u32)landing_label;
    say("  vector="); say_hex(vec); say("  recorded_pc="); say_hex(pc);
    say("  window=["); say_hex(lo); say(","); say_hex(hi); say("]");
    const bool ok = found && g_isr_calls == 1u && vec == 1u && pc >= lo && pc <= hi;
    say(ok ? "  OK\n" : "  FAIL\n");
    return ok;
}

extern "C" void reset_handler() {
    // Enable the FPU (CPACR CP10/CP11 full access), as every M4F startup does.
    *(volatile u32*)0xE000ED88u |= (0xFu << 20);
    __asm__ volatile("dsb\n isb" ::: "memory");
    for (u32* p = &_sbss; p < &_ebss; ++p) *p = 0;
    for (u32 *s = &_sidata, *d = &_sdata; d < &_edata; ) *d++ = *s++;
    const bool a = one_run("MSP", false);
    const bool b = one_run("PSP", true);
    finish_run(a && b ? 0 : 1);
}

// The libraries contain compiler-generated memset calls; a real firmware gets
// them from its libc. Built with -fno-tree-loop-distribute-patterns so this
// loop is not turned back into a call to itself.
extern "C" void* memset(void* d, int c, unsigned n) {
    u8* p = (u8*)d; while (n--) *p++ = (u8)c; return d;
}
