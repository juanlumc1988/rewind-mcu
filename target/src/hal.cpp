// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/hal.h"

namespace rwd {
namespace {

// A part has exactly one HAL, so this state is file-static rather than an
// object the firmware has to thread through every call. It also keeps the
// shim's cost at the call site down to a load and a branch.
const HalOps* g_ops        = 0;
TraceWriter*  g_writer     = 0;
Mode          g_mode       = MODE_OFF;
IsrFn         g_isr[kMaxIrqVectors];
u32           g_mask_depth = 0;
bool          g_in_isr     = false;
const char*   g_divergence = 0;

// Drains pending interrupts. Called after every MMIO access, because that is
// where this model lets an interrupt land: real silicon can interrupt at any
// instruction boundary, we can only observe the ones that bracket a
// peripheral access. Coarser than hardware, and enough to catch the entire
// class of bugs where an ISR lands inside a read-modify-write on state it
// shares with the main loop.
void pump_irqs() {
    if (g_ops == 0 || g_ops->poll_irq == 0) {
        return;
    }
    // Masked, or already inside a handler. Real hardware would nest by
    // priority; we keep handlers non-reentrant, which is the common
    // configuration and avoids modelling a priority scheme we do not have.
    if (g_mask_depth > 0 || g_in_isr) {
        return;
    }

    g_in_isr = true;
    for (;;) {
        const u32 vector = g_ops->poll_irq(g_ops->ctx);
        if (vector == kNoIrq) {
            break;
        }
        if (g_mode == MODE_RECORD && g_writer != 0) {
            g_writer->irq_enter(g_ops->now(g_ops->ctx), vector);
        }
        if (vector < kMaxIrqVectors && g_isr[vector] != 0) {
            g_isr[vector]();
        }
    }
    g_in_isr = false;
}

} // namespace

void hal_reset() {
    g_ops        = 0;
    g_writer     = 0;
    g_mode       = MODE_OFF;
    g_mask_depth = 0;
    g_in_isr     = false;
    g_divergence = 0;
    for (u32 i = 0; i < kMaxIrqVectors; ++i) {
        g_isr[i] = 0;
    }
}

void hal_attach(const HalOps* ops) {
    g_ops = ops;
}

void hal_set_isr(u32 vector, IsrFn fn) {
    if (vector < kMaxIrqVectors) {
        g_isr[vector] = fn;
    }
}

void hal_mode_off() {
    g_mode   = MODE_OFF;
    g_writer = 0;
}

void hal_mode_record(TraceWriter* writer) {
    g_mode   = MODE_RECORD;
    g_writer = writer;
}

void hal_mode_replay() {
    g_mode   = MODE_REPLAY;
    g_writer = 0;
}

Mode hal_mode() {
    return g_mode;
}

u32 mmio_read32(u32 addr) {
    if (g_ops == 0 || g_ops->read32 == 0) {
        return 0;
    }

    const u32 value = g_ops->read32(g_ops->ctx, addr);
    if (g_mode == MODE_RECORD && g_writer != 0) {
        g_writer->mmio_read(g_ops->now(g_ops->ctx), addr, value);
    }
    pump_irqs();
    return value;
}

void mmio_write32(u32 addr, u32 value) {
    if (g_ops == 0 || g_ops->write32 == 0) {
        return;
    }

    g_ops->write32(g_ops->ctx, addr, value);
    if (g_mode == MODE_RECORD && g_writer != 0) {
        g_writer->mmio_write(g_ops->now(g_ops->ctx), addr, value);
    }
    pump_irqs();
}

void hal_record_irq_entry(u32 vector) {
    if (g_mode == MODE_RECORD && g_writer != 0 && g_ops != 0 && g_ops->now != 0) {
        g_writer->irq_enter(g_ops->now(g_ops->ctx), vector);
    }
}

u64 hal_now() {
    if (g_ops == 0 || g_ops->now == 0) {
        return 0;
    }
    return g_ops->now(g_ops->ctx);
}

void hal_irq_disable() {
    ++g_mask_depth;
}

void hal_irq_enable() {
    if (g_mask_depth > 0) {
        --g_mask_depth;
    }
}

bool hal_diverged() {
    return g_divergence != 0;
}

const char* hal_divergence() {
    return g_divergence ? g_divergence : "";
}

void hal_set_divergence(const char* why) {
    if (g_divergence == 0) {
        g_divergence = why;
    }
}

} // namespace rwd
