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
      irq_latched_(false) {
    ops_.read32   = &Mcu::s_read32;
    ops_.write32  = &Mcu::s_write32;
    ops_.now      = &Mcu::s_now;
    ops_.poll_irq = &Mcu::s_poll_irq;
    ops_.ctx      = this;

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

    // A new byte only arrives once the previous one has been read, so the
    // model has no overrun case. Real UARTs do; modelling that here would
    // add a second failure mode on top of the race we are demonstrating.
    if (!rx_pending_ && tx_sent_ < tx_total_ && cycles_ >= next_rx_cycle_) {
        rx_byte_     = (u8)(next_random() & 0xFFu);
        rx_pending_  = true;
        irq_latched_ = true;
        tx_checksum_ = tx_checksum_ * 31u + (u32)rx_byte_;
        ++tx_sent_;

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

u32 Mcu::poll_irq() {
    if (irq_latched_) {
        irq_latched_ = false;
        return kIrqUartRx;
    }
    return rwd::kNoIrq;
}

u32  Mcu::s_read32(void* ctx, u32 addr) { return static_cast<Mcu*>(ctx)->read32(addr); }
void Mcu::s_write32(void* ctx, u32 addr, u32 v) { static_cast<Mcu*>(ctx)->write32(addr, v); }
u64  Mcu::s_now(void* ctx) { return static_cast<Mcu*>(ctx)->cycles_; }
u32  Mcu::s_poll_irq(void* ctx) { return static_cast<Mcu*>(ctx)->poll_irq(); }

} // namespace sim
