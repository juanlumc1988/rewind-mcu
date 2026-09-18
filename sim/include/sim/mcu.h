// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// A toy MCU: a cycle counter, a UART that receives bytes at pseudo-random
// intervals, and a GPIO port. Enough peripherals to host a firmware bug
// worth catching, few enough to read in one sitting.
//
// The simulator exists only to *produce* traces. Replay never instantiates
// one -- that is the property the golden test in tests/test_determinism.cpp
// enforces by destroying the Mcu before replaying its trace.

#ifndef SIM_MCU_H
#define SIM_MCU_H

#include "rewind/hal.h"

namespace sim {

using rwd::u8;
using rwd::u32;
using rwd::u64;

// -- memory map --

const u32 kSystickBase = 0x40000000u;
const u32 kSystickCnt  = kSystickBase + 0x00u;   // RO, free-running cycles

const u32 kUart0Base = 0x40001000u;
const u32 kUart0Sr   = kUart0Base + 0x00u;       // RO status
const u32 kUart0Dr   = kUart0Base + 0x04u;       // RO data; reading consumes

const u32 kGpio0Base = 0x40002000u;
const u32 kGpio0Odr  = kGpio0Base + 0x00u;       // RW output data

const u32 kSrRxne = 0x1u;   // receive register not empty
const u32 kSrTxe  = 0x2u;   // transmit register empty (always set here)

const u32 kIrqUartRx = 1u;

// -- timing --

// Cycles charged per MMIO access. Coarse, but it is the only clock the
// firmware and the arrival schedule share, which is all that matters.
const u32 kMmioCost = 3u;

// Arrival schedule.
//
// Bytes arrive in frames, because that is how a real link behaves: a burst
// of back-to-back characters, then a gap while the other end thinks. Equally
// spaced arrivals would be a kinder model and a useless one -- the race in
// the example firmware needs a byte to land while the main loop is still
// draining the previous one, and that only happens inside a burst.
//
// Two further details matter, and both are about honesty:
//
//   * Arrivals are scheduled from the previous ARRIVAL, not from the moment
//     the firmware got round to reading DR. Scheduling from consumption ties
//     the wire to the driver's own progress, which would make the race
//     unreachable by construction and hand a sweep a clean bill of health
//     for code that is not clean.
//
//   * The intra-frame gap overlaps the drain loop's duration rather than
//     sitting safely outside it. Whether a given byte lands inside the loop
//     or just after it comes down to the jitter, which is what makes the bug
//     intermittent across seeds instead of absent or certain.
const u32 kFrameLen       = 3u;     // bytes per frame
const u32 kRxByteGap      = 6u;     // min cycles between bytes in a frame
const u32 kRxByteJitter   = 200u;
const u32 kRxFrameGap     = 600u;   // min cycles between frames
const u32 kRxFrameJitter  = 300u;

class Mcu {
public:
    // `tx_total` bytes are delivered over the run, then the UART goes quiet.
    Mcu(u64 seed, u32 tx_total);

    // HalOps bound to this instance, for rwd::hal_attach().
    const rwd::HalOps* ops() const { return &ops_; }

    u64 cycles() const { return cycles_; }
    u32 tx_sent() const { return tx_sent_; }

    // Checksum over every byte the UART delivered, folded the same way the
    // example firmware folds the bytes it consumes. Equal checksums mean the
    // firmware saw all of the data, in order.
    u32 tx_checksum() const { return tx_checksum_; }

    u32 gpio_odr() const { return odr_; }

private:
    u64  next_random();
    void advance(u32 dt);
    u32  read32(u32 addr);
    void write32(u32 addr, u32 value);
    u32  poll_irq();

    // Trampolines: HalOps is a C-style vtable, so member calls need a hop.
    static u32  s_read32(void* ctx, u32 addr);
    static void s_write32(void* ctx, u32 addr, u32 value);
    static u64  s_now(void* ctx);
    static u32  s_poll_irq(void* ctx);

    Mcu(const Mcu&);
    Mcu& operator=(const Mcu&);

    rwd::HalOps ops_;
    u64  rng_;
    u64  cycles_;
    u64  next_rx_cycle_;
    u32  frame_pos_;      // index of the next byte within its frame
    u32  tx_total_;
    u32  tx_sent_;
    u32  tx_checksum_;
    u32  odr_;
    u8   rx_byte_;
    bool rx_pending_;    // byte sitting in DR, not yet read
    bool irq_latched_;   // edge not yet delivered to the shim
};

} // namespace sim

#endif // SIM_MCU_H
