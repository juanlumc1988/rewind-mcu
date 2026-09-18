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
void hal_record_irq_entry(u32 vector);

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

#endif // REWIND_HAL_H
