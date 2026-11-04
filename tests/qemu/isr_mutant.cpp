// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// A deliberately broken trampoline: it always reads MSP, as one that ignores
// EXC_RETURN would. run.sh builds it in place of the real one and requires the
// check to FAIL, which is what shows the process-stack case can fail at all.

#include "rewind/cortex_m.h"

extern "C" __attribute__((naked)) void IRQ0_Handler() {
    __asm__ volatile(
        "mrs r1, msp                  \n"
        "movs r0, #1                  \n"
        "b   rwd_cortex_m_isr_entry   \n");
}
