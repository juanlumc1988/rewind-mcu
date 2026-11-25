// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "sim/mcu.h"

namespace sim {

Mcu::Mcu(u64 seed, u32 tx_total)
    : rng_(seed ? seed : 0x9E3779B97F4A7C15ULL),
      cycles_(0),
      next_rx_cycle_(0),
      frame_pos_(0),
      tx_total_(tx_total),
      tx_sent_(0),
      tx_checksum_(0),
      odr_(0),
      rx_byte_(0),
      rx_pending_(false),
      irq_latched_(false),
      dma_(false),
      dma_count_(0),
      timer_period_(0),
      next_timer_(0),
      timer_latched_(false),
      timer_fired_(0) {
    for (u32 i = 0; i < kDmaRingSize; ++i) {
        dma_ring_[i] = 0;
    }
    ops_.read32   = &Mcu::s_read32;
    ops_.write32  = &Mcu::s_write32;
    ops_.now      = &Mcu::s_now;
    ops_.poll_irq = &Mcu::s_poll_irq;
    ops_.ctx      = this;
    ops_.site     = &Mcu::s_site;
    ops_.dma_read = &Mcu::s_dma_read;
    ops_.poll_irq_pri = &Mcu::s_poll_irq_pri;

    // Warm the generator so nearby seeds diverge immediately instead of
    // producing correlated first arrivals.
    next_random();
    next_rx_cycle_ = kRxFrameGap + (next_random() % kRxFrameJitter);
}

// xorshift64. Not cryptographic, not trying to be: it needs to be identical
// on every host, cheap, and free of the state-size surprises that make
// rand() useless for reproducible work.
u64 Mcu::next_random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return rng_;
}

void Mcu::advance(u32 dt) {
    cycles_ += (u64)dt;

    if (timer_period_ != 0 && cycles_ >= next_timer_) {
        timer_latched_ = true;
        ++timer_fired_;
        // On the period's own grid, skipping any the firmware was too slow to see.
        next_timer_ += (u64)timer_period_ * (1u + (cycles_ - next_timer_) / (u64)timer_period_);
    }

    // A new byte only arrives once the previous one has been read, so the
    // model has no overrun case. Real UARTs do; modelling that here would
    // add a second failure mode on top of the race we are demonstrating.
    // With DMA there is no data register to wait on: the controller writes the
    // ring whenever a byte arrives, whether or not the firmware has caught up.
    if ((dma_ || !rx_pending_) && tx_sent_ < tx_total_ && cycles_ >= next_rx_cycle_) {
        rx_byte_     = (u8)(next_random() & 0xFFu);
        tx_checksum_ = tx_checksum_ * 31u + (u32)rx_byte_;
        ++tx_sent_;
        if (dma_) {
            dma_ring_[dma_count_ % kDmaRingSize] = rx_byte_;
            ++dma_count_;
        } else {
            rx_pending_  = true;
            irq_latched_ = true;
        }

        // Schedule the next arrival from here, so the wire keeps its own
        // time regardless of what the firmware is doing.
        ++frame_pos_;
        if (frame_pos_ >= kFrameLen) {
            frame_pos_     = 0;
            next_rx_cycle_ = cycles_ + (u64)kRxFrameGap
                           + (next_random() % (u64)kRxFrameJitter);
        } else {
            next_rx_cycle_ = cycles_ + (u64)kRxByteGap
                           + (next_random() % (u64)kRxByteJitter);
        }
    }
}

u32 Mcu::read32(u32 addr) {
    advance(kMmioCost);

    switch (addr) {
        case kSystickCnt:
            return (u32)(cycles_ & 0xFFFFFFFFu);

        case kUart0Sr:
            return (rx_pending_ ? kSrRxne : 0u) | kSrTxe;

        case kUart0Dr: {
            // Reading DR frees the receive register; it does not influence
            // when the next byte shows up.
            const u32 value = (u32)rx_byte_;
            rx_pending_ = false;
            return value;
        }

        case kGpio0Odr:
            return odr_;

        case kDma0Count:
            return dma_count_;

        default:
            // Unmapped reads return zero, as a great many parts do.
            return 0u;
    }
}

void Mcu::write32(u32 addr, u32 value) {
    advance(kMmioCost);
    if (addr == kGpio0Odr) {
        odr_ = value;
    }
}

// The firmware reading the DMA ring. A read outside it, or of another region,
// gives zeros, as an unmapped address would.
void Mcu::dma_read(u32 region, u32 offset, u8* dst, u32 len) {
    advance(kDmaReadCost);
    const bool ok = region == kDmaRegion && offset <= kDmaRingSize
                 && len <= kDmaRingSize - offset;
    for (u32 i = 0; i < len; ++i) {
        dst[i] = ok ? dma_ring_[offset + i] : (u8)0;
    }
}

void Mcu::site(u32) {
    advance(kSiteCost);
}

u32 Mcu::poll_irq() {
    if (irq_latched_) {
        irq_latched_ = false;
        return kIrqUartRx;
    }
    if (timer_latched_) {
        timer_latched_ = false;
        return kIrqTimer;
    }
    return rwd::kNoIrq;
}

// The pending vector that is more urgent than `level`, most urgent first and
// the lower vector number among equals; the others stay pending.
u32 Mcu::poll_irq_pri(const u8* priority, u32 level) {
    u32 best = rwd::kNoIrq;
    if (irq_latched_ && (u32)priority[kIrqUartRx] < level) {
        best = kIrqUartRx;
    }
    if (timer_latched_ && (u32)priority[kIrqTimer] < level &&
        (best == rwd::kNoIrq || priority[kIrqTimer] < priority[best])) {
        best = kIrqTimer;
    }
    if (best == kIrqUartRx) {
        irq_latched_ = false;
    } else if (best == kIrqTimer) {
        timer_latched_ = false;
    }
    return best;
}

u32  Mcu::s_read32(void* ctx, u32 addr) { return static_cast<Mcu*>(ctx)->read32(addr); }
void Mcu::s_write32(void* ctx, u32 addr, u32 v) { static_cast<Mcu*>(ctx)->write32(addr, v); }
u64  Mcu::s_now(void* ctx) { return static_cast<Mcu*>(ctx)->cycles_; }
u32  Mcu::s_poll_irq(void* ctx) { return static_cast<Mcu*>(ctx)->poll_irq(); }
u32  Mcu::s_poll_irq_pri(void* ctx, const u8* priority, u32 level) {
    return static_cast<Mcu*>(ctx)->poll_irq_pri(priority, level);
}
void Mcu::s_site(void* ctx, u32 id) { static_cast<Mcu*>(ctx)->site(id); }
void Mcu::s_dma_read(void* ctx, u32 region, u32 offset, u8* dst, u32 len) {
    static_cast<Mcu*>(ctx)->dma_read(region, offset, dst, len);
}

} // namespace sim
