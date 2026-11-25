// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/describe.h"

#include <cstdio>

#include "sim/mcu.h"

namespace rwhost {

const char* register_name(u32 addr) {
    switch (addr) {
        case sim::kSystickCnt: return "SYSTICK.CNT";
        case sim::kUart0Sr:    return "UART0.SR";
        case sim::kUart0Dr:    return "UART0.DR";
        case sim::kGpio0Odr:   return "GPIO0.ODR";
        case sim::kDma0Count:  return "DMA0.COUNT";
        default:               return "";
    }
}

const char* event_kind(u8 type) {
    switch (type) {
        case rwd::EV_MMIO_READ:  return "read";
        case rwd::EV_MMIO_WRITE: return "write";
        case rwd::EV_IRQ_ENTER:
        case rwd::EV_IRQ_AT:     return "irq";
        case rwd::EV_SITE:       return "site";
        case rwd::EV_DMA:        return "dma";
        case rwd::EV_RAM:        return "ram";
        case rwd::EV_STOP:       return "stop";
        default:                 return "?";
    }
}

const char* irq_name(u32 vector) {
    switch (vector) {
        case sim::kIrqUartRx: return "UART_RX";
        case sim::kIrqTimer:  return "TIMER";
        default:              return "";
    }
}

std::string describe_event(const rwd::Event& e) {
    char buf[160];
    switch (e.type) {
        case rwd::EV_MMIO_READ:
        case rwd::EV_MMIO_WRITE: {
            const char* name = register_name(e.addr);
            std::snprintf(buf, sizeof(buf), "%s 0x%08X%s%s %s 0x%08X", event_kind(e.type), e.addr,
                          name[0] ? " " : "", name,
                          e.type == rwd::EV_MMIO_READ ? "->" : "<-", e.value);
            break;
        }
        case rwd::EV_IRQ_ENTER:
            std::snprintf(buf, sizeof(buf), "irq vector %u%s%s", e.addr,
                          irq_name(e.addr)[0] ? " " : "", irq_name(e.addr));
            break;
        case rwd::EV_IRQ_AT:
            std::snprintf(buf, sizeof(buf), "irq vector %u%s%s returns to 0x%08X", e.addr,
                          irq_name(e.addr)[0] ? " " : "", irq_name(e.addr), e.value);
            break;
        case rwd::EV_SITE:
            std::snprintf(buf, sizeof(buf), "site %u", e.addr);
            break;
        case rwd::EV_RAM:
        case rwd::EV_DMA: {
            int n = std::snprintf(buf, sizeof(buf), "%s region %u +%u len %u ", event_kind(e.type),
                                  e.addr, e.value, (unsigned)e.len);
            for (u32 i = 0; i < (u32)e.len && n > 0 && n + 3 < (int)sizeof(buf); ++i) {
                n += std::snprintf(buf + n, sizeof(buf) - (std::size_t)n, "%02X", (unsigned)e.data[i]);
            }
            break;
        }
        case rwd::EV_STOP:
            std::snprintf(buf, sizeof(buf), "stop");
            break;
        default:
            std::snprintf(buf, sizeof(buf), "? (type 0x%02X)", (unsigned)e.type);
            break;
    }
    return buf;
}

} // namespace rwhost
