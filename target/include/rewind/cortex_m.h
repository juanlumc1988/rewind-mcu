// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The HalOps backed by actual silicon: volatile MMIO and the DWT cycle
// counter.
//
// NOT YET RUN ON HARDWARE. It compiles under the same strict C++98 rules as
// the rest of the target side and the register definitions are from the ARM
// documentation, but nothing here has been executed on a part. Treat it as
// reviewed, not as tested.
//
// Two things the caller must arrange:
//
//   1. enable_cycle_counter() before recording, or every timestamp is zero.
//      On most Cortex-M parts DWT is gated behind DEMCR.TRCENA.
//
//   2. Each ISR that matters calls hal_record_irq_entry(vector) in its
//      prologue. The shim cannot observe an interrupt on hardware -- the
//      NVIC dispatches without asking -- so poll_irq here always returns
//      kNoIrq and the handler reports itself.

#ifndef REWIND_CORTEX_M_H
#define REWIND_CORTEX_M_H

#include "rewind/clock.h"
#include "rewind/hal.h"

namespace rwd {
namespace cortex_m {

// ARMv7-M debug and trace registers.
const u32 kDemcr       = 0xE000EDFCu;
const u32 kDemcrTrcena = 0x01000000u;   // DEMCR bit 24

const u32 kDwtCtrl        = 0xE0001000u;
const u32 kDwtCyccnt      = 0xE0001004u;
const u32 kDwtCtrlCyccnt  = 0x00000001u;   // DWT_CTRL bit 0, CYCCNTENA
const u32 kDwtCtrlNocyc   = 0x02000000u;   // DWT_CTRL bit 25, NOCYCCNT

inline u32 raw_read32(u32 addr) {
    return *reinterpret_cast<volatile u32*>(addr);
}

inline void raw_write32(u32 addr, u32 value) {
    *reinterpret_cast<volatile u32*>(addr) = value;
}

// Turns on the DWT cycle counter. Returns false when the part reports it
// does not implement one (DWT_CTRL.NOCYCCNT), in which case timestamps need
// another source -- a general-purpose timer works, at coarser resolution.
bool enable_cycle_counter();

class HardwareHal {
public:
    HardwareHal();

    const HalOps* ops() const { return &ops_; }
    ClockExtender& clock() { return clock_; }

private:
    static u32  s_read32(void* ctx, u32 addr);
    static void s_write32(void* ctx, u32 addr, u32 value);
    static u64  s_now(void* ctx);
    static u32  s_poll_irq(void* ctx);

    HardwareHal(const HardwareHal&);
    HardwareHal& operator=(const HardwareHal&);

    HalOps        ops_;
    ClockExtender clock_;
};

} // namespace cortex_m
} // namespace rwd

#endif // REWIND_CORTEX_M_H
