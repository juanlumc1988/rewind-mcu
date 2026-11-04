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
//   2. Each ISR that matters reports itself. The shim cannot observe an
//      interrupt on hardware -- the NVIC dispatches without asking -- so
//      poll_irq here always returns kNoIrq. Either put an
//      RWD_CORTEX_M_ISR trampoline (below) in the vector table, which also
//      records the return address, or call hal_record_irq_entry(vector) in
//      the handler's prologue, which does not.

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

// -- the exception frame --
//
// On exception entry the core pushes eight words onto the stack in use at
// the time of the interrupt, lowest address first (ARMv7-M ARM, B1.5.6):
//
//   [0] R0  [1] R1  [2] R2  [3] R3  [4] R12  [5] LR  [6] PC  [7] xPSR
//
// [6] is the address execution resumes at when the handler returns: the
// instruction the interrupt landed in front of. With the FPU in use, floating
// point state is pushed *after* these eight, so their offsets do not move.
// Stack alignment padding is added above the frame, not below it.
const u32 kFramePcIndex = 6;

// Bit 2 of EXC_RETURN (the value LR holds on handler entry) says which stack
// the frame went to: clear = main stack (MSP), set = process stack (PSP).
const u32 kExcReturnPsp = 0x00000004u;

inline const u32* exception_frame(u32 exc_return, const u32* msp, const u32* psp) {
    return (exc_return & kExcReturnPsp) != 0 ? psp : msp;
}

inline u32 stacked_pc(const u32* frame) {
    return frame[kFramePcIndex];
}

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

// -- hardware vector table entries that record where they landed --
//
// What an ISR needs to record its return address is the frame pointer, and
// the frame pointer is only visible before the compiler has touched SP, so it
// cannot be written in C++. RWD_CORTEX_M_ISR(name, vector) defines `name` as a
// tiny naked function: pick MSP or PSP from EXC_RETURN, pass the frame and the
// shim vector number on, and tail-branch to rwd_cortex_m_isr_entry. LR is
// left holding EXC_RETURN, so the C++ function's own return is the exception
// return -- exactly as it would be for a hand-written C handler.
//
// Put `name` in the vector table, and register the real handler with
// hal_set_isr(vector, fn). The handler then runs exactly as it does in the
// simulator, and the trace gets an EV_IRQ_AT for it.
//
//     RWD_CORTEX_M_ISR(USART1_IRQHandler, 1)
//
// Defined for ARM only; elsewhere the macro is not available, because there
// is no exception frame to read.
extern "C" void rwd_cortex_m_isr_entry(rwd::u32 vector, const rwd::u32* frame);

#if defined(__arm__)
#define RWD_CORTEX_M_ISR(name, vector)                                      \
    extern "C" __attribute__((naked)) void name() {                         \
        __asm__ volatile(                                                   \
            "tst   lr, #4                 \n"                               \
            "ite   eq                     \n"                               \
            "mrseq r1, msp                \n"                               \
            "mrsne r1, psp                \n"                               \
            "movs  r0, #" #vector "       \n"                               \
            "b     rwd_cortex_m_isr_entry \n");                             \
    }
#endif

#endif // REWIND_CORTEX_M_H
