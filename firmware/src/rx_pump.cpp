// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "fw/rx_pump.h"
#include "rewind/hal.h"
#include "sim/mcu.h"

namespace fw {
namespace {

using rwd::u8;
using rwd::u32;
using rwd::u64;

State g_state;

void isr_uart_rx() {
    const u32 sr = rwd::mmio_read32(sim::kUart0Sr);
    if ((sr & sim::kSrRxne) == 0) {
        return;
    }

    const u8 byte = (u8)(rwd::mmio_read32(sim::kUart0Dr) & 0xFFu);

    const u32 next = (g_state.head + 1u) % kRingSize;
    if (next == g_state.tail) {
        ++g_state.overflows;
        return;
    }

    g_state.ring[g_state.head] = byte;
    g_state.head    = next;
    g_state.pending = g_state.pending + 1u;
}

// Consumes one byte from the ring: stamp it, fold it into the checksum, show
// the running count on the port. Perfectly ordinary work -- and the two
// peripheral accesses are what hold the drain loop open long enough for an
// interrupt to land in the middle of it.
void consume_one() {
    const u8 byte = g_state.ring[g_state.tail];
    g_state.tail     = (g_state.tail + 1u) % kRingSize;
    g_state.checksum = g_state.checksum * 31u + (u32)byte;
    ++g_state.consumed;

    g_state.last_stamp = rwd::mmio_read32(sim::kSystickCnt);
    rwd::mmio_write32(sim::kGpio0Odr, g_state.consumed);
}

inline u64 fnv_byte(u64 h, u8 b) {
    h ^= (u64)b;
    h *= 1099511628211ULL;
    return h;
}

u64 fnv_u32(u64 h, u32 v) {
    for (u32 i = 0; i < 4u; ++i) {
        h = fnv_byte(h, (u8)((v >> (i * 8u)) & 0xFFu));
    }
    return h;
}

} // namespace

void rx_pump_reset() {
    for (u32 i = 0; i < kRingSize; ++i) {
        g_state.ring[i] = 0;
    }
    g_state.head      = 0;
    g_state.tail      = 0;
    g_state.pending   = 0;
    g_state.consumed  = 0;
    g_state.checksum   = 0;
    g_state.last_stamp = 0;
    g_state.overflows  = 0;
}

void rx_pump_install() {
    rwd::hal_set_isr(sim::kIrqUartRx, &isr_uart_rx);
}

void rx_pump_run(Variant variant, u32 main_iters) {
    for (u32 iter = 0; iter < main_iters; ++iter) {
        u32 n;

        if (variant == kFixed) {
            rwd::hal_irq_disable();
            n = g_state.pending;
            rwd::hal_irq_enable();
        } else {
            n = g_state.pending;
        }

        if (n == 0) {
            // Idle poll. Costs a peripheral access, which keeps the clock
            // moving and gives interrupts somewhere harmless to land.
            rwd::mmio_read32(sim::kSystickCnt);
            continue;
        }

        for (u32 i = 0; i < n; ++i) {
            consume_one();
        }

        if (variant == kFixed) {
            rwd::hal_irq_disable();
            g_state.pending -= n;      // subtract what we handled
            rwd::hal_irq_enable();
        } else {
            g_state.pending = 0;       // BUG: erases anything the ISR added
                                       // while the drain loop was running
        }
    }
}

const State& rx_pump_state() {
    return g_state;
}

u64 rx_pump_state_hash() {
    u64 h = 14695981039346656037ULL;
    for (u32 i = 0; i < kRingSize; ++i) {
        h = fnv_byte(h, g_state.ring[i]);
    }
    h = fnv_u32(h, g_state.head);
    h = fnv_u32(h, g_state.tail);
    h = fnv_u32(h, g_state.pending);
    h = fnv_u32(h, g_state.consumed);
    h = fnv_u32(h, g_state.checksum);
    h = fnv_u32(h, g_state.last_stamp);
    h = fnv_u32(h, g_state.overflows);
    return h;
}

} // namespace fw
