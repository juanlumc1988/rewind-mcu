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

// dma_ is not cleared: entries past dma_count_ are never read. And ops_.dma_read
// is left null until a region is registered, so a firmware that does no DMA does
// not pull s_dma_read into flash -- --gc-sections can drop it.
HardwareHal::HardwareHal() : dma_count_(0), ref_read_(0), ref_ctx_(0) {
    ops_.read32   = &HardwareHal::s_read32;
    ops_.write32  = &HardwareHal::s_write32;
    ops_.now      = &HardwareHal::s_now;
    ops_.poll_irq = &HardwareHal::s_poll_irq;
    ops_.ctx      = this;
    // ops_.site stays null: nothing to account for on real silicon.
}

u32 HardwareHal::s_read32(void*, u32 addr) {
    return raw_read32(addr);
}

void HardwareHal::s_write32(void*, u32 addr, u32 value) {
    raw_write32(addr, value);
}

u64 HardwareHal::s_now(void* ctx) {
    return static_cast<HardwareHal*>(ctx)->extend_cycles(raw_read32(kDwtCyccnt));
}

// The two readings should be taken at the same moment; they are a few cycles
// apart, which is nothing against a reference tick of thousands.
u64 HardwareHal::extend_cycles(u32 raw) {
    if (ref_read_ != 0) {
        return clock_.extend(raw, ref_read_(ref_ctx_));
    }
    return clock_.extend(raw);
}

bool HardwareHal::set_reference_clock(ReferenceFn read, void* ctx, u32 cycles_per_tick) {
    if (read == 0 || !clock_.set_reference(cycles_per_tick)) {
        return false;
    }
    ref_read_ = read;
    ref_ctx_  = ctx;
    return true;
}

bool HardwareHal::add_dma_region(u32 id, const volatile u8* base, u32 size) {
    if (base == 0 || size == 0 || dma_count_ >= kMaxDmaRegions) {
        return false;
    }
    for (u32 i = 0; i < dma_count_; ++i) {
        if (dma_[i].id == id) {
            return false;
        }
    }
    dma_[dma_count_].id   = id;
    dma_[dma_count_].base = base;
    dma_[dma_count_].size = size;
    ++dma_count_;
    ops_.dma_read = &HardwareHal::s_dma_read;
    return true;
}

// Byte by byte through a volatile pointer: the memory changes behind the
// compiler's back, so it must not be cached, merged or reordered. A region the
// firmware never registered, or a read past its end, gives zeros -- the same
// thing an unknown register gives -- and the trace records those zeros, so a
// replay shows the firmware what it saw.
void HardwareHal::s_dma_read(void* ctx, u32 region, u32 offset, u8* dst, u32 len) {
    const HardwareHal* self = static_cast<HardwareHal*>(ctx);

    const DmaRegion* r = 0;
    for (u32 i = 0; i < self->dma_count_; ++i) {
        if (self->dma_[i].id == region) {
            r = &self->dma_[i];
        }
    }
    // Written as subtractions so a hostile offset cannot wrap the sum.
    const bool ok = (r != 0) && offset <= r->size && len <= r->size - offset;
    for (u32 i = 0; i < len; ++i) {
        dst[i] = ok ? r->base[offset + i] : (u8)0;
    }
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
