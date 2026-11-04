// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/cortex_m.h"

namespace rwd {
namespace cortex_m {

bool enable_cycle_counter() {
    raw_write32(kDemcr, raw_read32(kDemcr) | kDemcrTrcena);

    if ((raw_read32(kDwtCtrl) & kDwtCtrlNocyc) != 0) {
        return false;   // part has no cycle counter
    }

    raw_write32(kDwtCyccnt, 0);
    raw_write32(kDwtCtrl, raw_read32(kDwtCtrl) | kDwtCtrlCyccnt);
    return true;
}

HardwareHal::HardwareHal() {
    ops_.read32   = &HardwareHal::s_read32;
    ops_.write32  = &HardwareHal::s_write32;
    ops_.now      = &HardwareHal::s_now;
    ops_.poll_irq = &HardwareHal::s_poll_irq;
    ops_.ctx      = this;
}

u32 HardwareHal::s_read32(void*, u32 addr) {
    return raw_read32(addr);
}

void HardwareHal::s_write32(void*, u32 addr, u32 value) {
    raw_write32(addr, value);
}

u64 HardwareHal::s_now(void* ctx) {
    return static_cast<HardwareHal*>(ctx)->clock_.extend(raw_read32(kDwtCyccnt));
}

u32 HardwareHal::s_poll_irq(void*) {
    // The NVIC dispatches; there is nothing for the shim to poll. Real ISRs
    // announce themselves with hal_record_irq_entry().
    return kNoIrq;
}

} // namespace cortex_m
} // namespace rwd

// Entered from an RWD_CORTEX_M_ISR trampoline, with the handler's frame.
// A null frame (it cannot happen from the trampoline; it can from a caller
// that has no frame to give) still records the entry, with no address.
extern "C" void rwd_cortex_m_isr_entry(rwd::u32 vector, const rwd::u32* frame) {
    if (frame != 0) {
        rwd::hal_record_irq_entry_at(vector, rwd::cortex_m::stacked_pc(frame));
    } else {
        rwd::hal_record_irq_entry(vector);
    }
    rwd::hal_run_isr(vector);
}
