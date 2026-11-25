// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// A model of the thermostat's hardware, to run the firmware against on a
// workstation: a room that warms when the heater is on and cools when it is
// not, and a button pressed now and then. It is test scaffolding -- the real
// recording comes from the part -- and the replay does not use it at all.

#ifndef THERMOSTAT_DEVICE_H
#define THERMOSTAT_DEVICE_H

#include "rewind/hal.h"
#include "thermostat.h"

namespace thermostat {

class Device {
public:
    explicit Device(u32 seed)
        : cycles_(0), temp_(180), heater_(0), rng_(seed ? seed : 1u), next_press_(900),
          press_pending_(false), next_tick_(40) {
        ops_.read32   = &Device::s_read;
        ops_.write32  = &Device::s_write;
        ops_.now      = &Device::s_now;
        ops_.poll_irq = &Device::s_poll;
        ops_.ctx      = this;
    }
    const rwd::HalOps* ops() const { return &ops_; }

private:
    u32 next() {
        rng_ = rng_ * 1664525u + 1013904223u;
        return rng_ >> 8;
    }
    void advance(u32 dt) {
        cycles_ += dt;
        while (cycles_ >= next_tick_) {            // the room, every 40 cycles
            next_tick_ += 40;
            if (heater_ != 0) {
                temp_ += (next() % 3);             // warms 0..2 tenths
            } else if (temp_ > 0 && next() % 3 == 0) {
                temp_ -= 1;                        // cools more slowly
            }
        }
        if (cycles_ >= next_press_) {
            press_pending_ = true;
            next_press_ += 1500 + next() % 1500;
        }
    }
    static u32 s_read(void* c, u32 addr) {
        Device* d = static_cast<Device*>(c);
        d->advance(3);
        if (addr == kTempReg) {
            return d->temp_;
        }
        return 0;
    }
    static void s_write(void* c, u32 addr, u32 v) {
        Device* d = static_cast<Device*>(c);
        d->advance(3);
        if (addr == kHeaterReg) {
            d->heater_ = v;
        }
    }
    static rwd::u64 s_now(void* c) { return static_cast<Device*>(c)->cycles_; }
    static u32 s_poll(void* c) {
        Device* d = static_cast<Device*>(c);
        if (d->press_pending_) {
            d->press_pending_ = false;
            return kIrqButton;
        }
        return rwd::kNoIrq;
    }

    rwd::HalOps ops_;
    rwd::u64    cycles_;
    u32         temp_;
    u32         heater_;
    u32         rng_;
    rwd::u64    next_press_;
    bool        press_pending_;
    rwd::u64    next_tick_;
};

} // namespace thermostat

#endif // THERMOSTAT_DEVICE_H
