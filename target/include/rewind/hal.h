// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The shim that makes the whole idea work.
//
// Firmware never touches a peripheral register directly; it goes through
// mmio_read32/mmio_write32 and lets interrupts arrive through this layer.
// That turns the firmware into a pure function of a small, enumerable set of
// inputs -- register reads, interrupt arrivals -- and a pure function can be
// replayed.
//
// The same shim serves all three modes, and that symmetry is the point:
//
//   MODE_OFF     ops -> real hardware, nothing recorded. Production.
//   MODE_RECORD  ops -> real hardware, every input appended to a trace.
//   MODE_REPLAY  ops -> a trace. The hardware is not merely unused, it is
//                unreachable, because the backing HalOps is the trace.
//
// Record and replay run identical firmware down identical code paths. The
// only difference is where the bytes come from, so any deviation in the
// firmware's behaviour shows up immediately as a mismatched address or an
// event arriving out of turn -- a divergence, reported loudly rather than
// papered over.

#ifndef REWIND_HAL_H
#define REWIND_HAL_H

#include "rewind/rw_types.h"
#include "rewind/trace_writer.h"

namespace rwd {

// Returned by HalOps::poll_irq when nothing is pending.
const u32 kNoIrq         = 0xFFFFFFFFu;
const u32 kMaxIrqVectors = 8;

// Interrupt priorities, Cortex-M style: a smaller number is more urgent, and an
// interrupt preempts a running handler only if it is strictly more urgent. Every
// vector starts at 0, so by default nothing preempts anything. The level of
// "no handler is running" is kThreadLevel, less urgent than any priority.
const u32 kThreadLevel   = 255;
const u32 kMaxPriority   = 254;

typedef void (*IsrFn)();

// The backend behind the shim: real MMIO on the target, a simulator on the
// host, or a recorded trace during replay. Function pointers rather than an
// abstract base -- no vtable in flash, and trivially fakeable in a test.
struct HalOps {
    u32  (*read32)(void* ctx, u32 addr);
    void (*write32)(void* ctx, u32 addr, u32 value);
    u64  (*now)(void* ctx);
    // Returns a pending interrupt vector, or kNoIrq. Called after every MMIO
    // access, which is where this model allows interrupts to land.
    u32  (*poll_irq)(void* ctx);
    void* ctx;
    // Optional (may be null): the firmware crossed the probe point `id`. The
    // simulator charges the cycles that stretch of code would take; replay
    // consumes the matching event from the trace. Real hardware has nothing
    // to do.
    void (*site)(void* ctx, u32 id);
    // Optional (may be null): fill `dst` with `len` bytes (1..kMaxDmaChunk)
    // read from DMA region `region` at `offset`. On a part this is a copy out
    // of RAM that a DMA controller writes; in the simulator, out of its model
    // of that RAM; in replay, out of the trace. Never called with more than
    // kMaxDmaChunk bytes: hal_dma_read() splits longer reads.
    void (*dma_read)(void* ctx, u32 region, u32 offset, u8* dst, u32 len);
    // Optional (may be null): poll_irq for a backend that models priorities.
    // Returns the most urgent pending vector v with priority[v] < level, and
    // takes it off the pending set; vectors that are not that urgent stay
    // pending, to be returned when the handler in the way has finished. Ties
    // go to the lower vector, as on a Cortex-M. `priority` has kMaxIrqVectors
    // entries. With this the shim lets a handler be preempted; without it,
    // poll_irq is called only outside handlers and nothing nests.
    u32  (*poll_irq_pri)(void* ctx, const u8* priority, u32 level);

    // Everything starts null, so a backend that does not know about a hook
    // added later -- site was the first -- simply does not provide it, instead
    // of leaving an uninitialised pointer for the shim to call.
    HalOps() : read32(0), write32(0), now(0), poll_irq(0), ctx(0), site(0),
               dma_read(0), poll_irq_pri(0) {}
};

enum Mode {
    MODE_OFF    = 0,
    MODE_RECORD = 1,
    MODE_REPLAY = 2
};

// -- wiring --

// Clears mode, backend, ISR table, interrupt mask and divergence state.
void hal_reset();

void hal_attach(const HalOps* ops);   // `ops` must outlive the attachment
void hal_set_isr(u32 vector, IsrFn fn);

// Sets how urgent `vector` is (0 is the most urgent; kMaxPriority the least,
// and larger values are taken as that). Needed on the device and on the host
// alike: on hardware the NVIC's own priority registers decide, and the shim
// only needs the same numbers so that a replay nests what the recording did.
// Priorities take effect with a backend that provides poll_irq_pri.
void hal_set_irq_priority(u32 vector, u32 priority);

// How urgent the running handler is: kThreadLevel when none is, otherwise its
// vector's priority. And how many are running, nested.
u32  hal_irq_level();
u32  hal_irq_depth();

void hal_mode_off();
void hal_mode_record(TraceWriter* writer);
void hal_mode_replay();

Mode hal_mode();

// -- the firmware-facing API --

u32  mmio_read32(u32 addr);
void mmio_write32(u32 addr, u32 value);

// Records an interrupt entry from inside a real ISR prologue. No-op unless
// recording.
//
// On hardware the shim does not dispatch interrupts -- the NVIC does, and it
// does so at any instruction boundary. So a real ISR calls this itself, and
// HalOps::poll_irq returns kNoIrq forever.
//
// Under MODE_REPLAY the relationship inverts: poll_irq serves EV_IRQ_ENTER
// out of the trace and the shim dispatches, because there is no NVIC to do
// it. This is the model's central approximation and its central limitation:
// replay can only re-enter a handler after an MMIO access, whereas hardware
// could have entered it anywhere. A trace recorded on silicon whose
// interrupt landed mid-computation has no replayable point to land on, and
// will be reported as a divergence rather than replayed wrongly.
//
// Closing that gap needs the return address recorded alongside the vector
// and something to stop on it. It is the next real problem, not a detail.
// Recording the address is hal_record_irq_entry_at(); stopping on it is not
// done yet.
void hal_record_irq_entry(u32 vector);

// As hal_record_irq_entry(), plus the address the CPU returns to when the
// handler finishes -- the PC stacked in the exception frame, which is the
// instruction the interrupt landed in front of. `return_pc` belongs to the
// build that recorded it. cortex_m.h reads it out of the frame for you.
void hal_record_irq_entry_at(u32 vector, u32 return_pc);

// Calls the handler installed for `vector`, if any. For a hardware vector
// table entry that has already recorded the entry itself; the simulator and
// replay dispatch through HalOps::poll_irq instead and never call this.
void hal_run_isr(u32 vector);

// Probe points.
//
// An interrupt can only be replayed where the shim gets to run, and the shim
// only runs at a peripheral access. A race whose whole window is computation
// -- a shared counter loaded, adjusted and stored with no register touched in
// between -- therefore has nowhere to land. A probe point is a place the
// firmware declares in its own source that counts as one more crossing: it is
// recorded (EV_SITE), replay consumes it in order and checks the label, and an
// interrupt that the recording placed after it is delivered right there.
//
//     u32 tmp = g_shared;
//     RWD_SITE(7);              // an interrupt may land here
//     g_shared = tmp + 1;
//
// How exactly a replay can reproduce a race is how densely the sites are
// placed around the code that races: an interrupt that landed between two
// crossings is replayed after the earlier one. The label identifies the point;
// give each site its own. Compile with REWIND_DISABLE_SITES to make RWD_SITE
// vanish -- but a trace recorded with sites will not replay on a build without
// them, which the replay reports as a divergence, as for any other mismatch.
void hal_site(u32 id);

// Reads memory that something other than the CPU writes -- a DMA buffer.
//
// A DMA controller deposits data at times nothing on a part can observe, so a
// trace cannot record the deposit. It can record the moment the firmware looks:
// that is an access like any other, and recording the bytes it saw there is
// what makes the run replayable. So firmware reads DMA buffers through this,
// the way it reads registers through mmio_read32, instead of dereferencing the
// buffer:
//
//     const u32 count = mmio_read32(DMA_COUNT);   // how many bytes have landed
//     u8 buf[8];
//     hal_dma_read(RX_REGION, tail, buf, 8);      // and the bytes themselves
//
// `region` names the buffer. The firmware registers the same ids on the device
// (HardwareHal::add_dma_region) and on the host, because a device address means
// nothing on the host. A read longer than kMaxDmaChunk is split into several
// events, identically on record and replay, and an interrupt may be delivered
// between the pieces, as it may after any access. Each piece is a crossing, so
// there is nothing to locate: the trace says exactly what the firmware saw.
//
// With no backend, or one without DMA, the bytes read are zero.
void hal_dma_read(u32 region, u32 offset, void* dst, u32 len);

// Instrumentation only. Under MODE_REPLAY this reports the timestamp of the
// last consumed event, which tracks the recorded clock at every MMIO access
// but not between them. Firmware that needs the time must read its timer
// through mmio_read32, which is recorded.
u64 hal_now();

// Nestable critical section, the moral equivalent of PRIMASK. While masked,
// no interrupt is dispatched -- and none is recorded, so a critical section
// looks the same in a trace as it did on the wire.
void hal_irq_disable();
void hal_irq_enable();

// -- divergence --
//
// Set by a replay backend when the firmware asks for something the trace
// does not have. Once latched it stays latched: the first divergence is the
// interesting one, everything after it is noise.
bool        hal_diverged();
const char* hal_divergence();
void        hal_set_divergence(const char* why);

} // namespace rwd

#if defined(REWIND_DISABLE_SITES)
#define RWD_SITE(id) ((void)0)
#else
#define RWD_SITE(id) ::rwd::hal_site(id)
#endif

#endif // REWIND_HAL_H
