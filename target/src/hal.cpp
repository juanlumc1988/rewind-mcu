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
u8            g_prio[kMaxIrqVectors];
u32           g_mask_depth = 0;
u32           g_depth      = 0;            // handlers running, nested
u32           g_level      = kThreadLevel; // urgency of the innermost one
const char*   g_divergence = 0;

// Drains pending interrupts. Called after every MMIO access, because that is
// where this model lets an interrupt land: real silicon can interrupt at any
// instruction boundary, we can only observe the ones that bracket a
// peripheral access. Coarser than hardware, and enough to catch the entire
// class of bugs where an ISR lands inside a read-modify-write on state it
// shares with the main loop.
//
// A handler's own accesses come back through here. With a backend that models
// priorities, one more urgent than the handler running may be taken then, and
// runs to the end before the interrupted handler goes on: the recursion is as
// deep as there are priority levels, which is at most kMaxIrqVectors. With one
// that does not, a handler is never interrupted, as before.
void pump_irqs() {
    if (g_ops == 0 || (g_ops->poll_irq == 0 && g_ops->poll_irq_pri == 0)) {
        return;
    }
    if (g_mask_depth > 0) {
        return;
    }
    const bool nests = g_ops->poll_irq_pri != 0;
    if (!nests && g_depth > 0) {
        return;
    }

    for (;;) {
        const u32 vector = nests ? g_ops->poll_irq_pri(g_ops->ctx, g_prio, g_level)
                                 : g_ops->poll_irq(g_ops->ctx);
        if (vector == kNoIrq) {
            break;
        }
        if (g_mode == MODE_RECORD && g_writer != 0) {
            g_writer->irq_enter(g_ops->now(g_ops->ctx), vector);
        }

        const u32 outer = g_level;
        g_level = vector < kMaxIrqVectors ? (u32)g_prio[vector] : 0u;
        ++g_depth;
        if (vector < kMaxIrqVectors && g_isr[vector] != 0) {
            g_isr[vector]();
        }
        --g_depth;
        g_level = outer;
    }
}

} // namespace

void hal_reset() {
    g_ops        = 0;
    g_writer     = 0;
    g_mode       = MODE_OFF;
    g_mask_depth = 0;
    g_depth      = 0;
    g_level      = kThreadLevel;
    g_divergence = 0;
    for (u32 i = 0; i < kMaxIrqVectors; ++i) {
        g_isr[i]  = 0;
        g_prio[i] = 0;
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

void hal_set_irq_priority(u32 vector, u32 priority) {
    if (vector < kMaxIrqVectors) {
        g_prio[vector] = (u8)(priority > kMaxPriority ? kMaxPriority : priority);
    }
}

u32 hal_irq_level() {
    return g_level;
}

u32 hal_irq_depth() {
    return g_depth;
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

void hal_record_irq_entry_at(u32 vector, u32 return_pc) {
    if (g_mode == MODE_RECORD && g_writer != 0 && g_ops != 0 && g_ops->now != 0) {
        g_writer->irq_enter_at(g_ops->now(g_ops->ctx), vector, return_pc);
    }
}

void hal_run_isr(u32 vector) {
    if (vector < kMaxIrqVectors && g_isr[vector] != 0) {
        g_isr[vector]();
    }
}

void hal_site(u32 id) {
    if (g_ops == 0) {
        return;
    }
    // Same order as an MMIO access: the backend moves first, the event is
    // stamped with where it left the clock, and only then may an interrupt
    // be delivered.
    if (g_ops->site != 0) {
        g_ops->site(g_ops->ctx, id);
    }
    if (g_mode == MODE_RECORD && g_writer != 0 && g_ops->now != 0) {
        g_writer->site(g_ops->now(g_ops->ctx), id);
    }
    pump_irqs();
}

void hal_dma_read(u32 region, u32 offset, void* dst, u32 len) {
    u8* out = static_cast<u8*>(dst);

    while (len > 0) {
        const u32 n = len < kMaxDmaChunk ? len : kMaxDmaChunk;

        if (g_ops == 0 || g_ops->dma_read == 0) {
            for (u32 i = 0; i < n; ++i) {
                out[i] = 0;
            }
        } else {
            g_ops->dma_read(g_ops->ctx, region, offset, out, n);
            if (g_mode == MODE_RECORD && g_writer != 0 && g_ops->now != 0) {
                g_writer->dma(g_ops->now(g_ops->ctx), region, offset, out, n);
            }
            pump_irqs();
        }
        out    += n;
        offset += n;
        len    -= n;
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
