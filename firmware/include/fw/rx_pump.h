// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Example firmware: drain a UART into a ring buffer from an ISR, consume it
// from the main loop. Two variants of the same code, differing in four lines.
//
// kBuggy contains the single most common concurrency bug in embedded C++:
// the main loop reads a counter the ISR also writes, does work, and then
// *clears* the counter rather than subtracting what it actually handled.
// Any interrupt that lands in between is erased. It is a one-line mistake,
// it passes review, it passes every test that does not happen to interleave
// the right way, and in the field it shows up months later as a device that
// occasionally falls behind and never recovers.
//
// kFixed does the read and the decrement inside a critical section.
//
// The state below is file-global on purpose: an interrupt vector is a
// function of no arguments, so firmware shares data with its ISRs through
// static storage. That is also precisely what makes the bug possible, and
// hiding it behind an object would hide the bug with it.

#ifndef FW_RX_PUMP_H
#define FW_RX_PUMP_H

#include "rewind/rw_types.h"

namespace fw {

enum Variant {
    kBuggy = 0,
    kFixed = 1
};

const rwd::u32 kRingSize = 16;

struct State {
    rwd::u8  ring[kRingSize];
    rwd::u32 head;       // written by the ISR
    rwd::u32 tail;       // written by the main loop
    rwd::u32 pending;    // written by both -- the contended one
    rwd::u32 consumed;
    rwd::u32 checksum;      // folded over consumed bytes, same recipe as the
                            // simulator's tx_checksum
    rwd::u32 last_stamp;    // cycle count when the last byte was handled
    rwd::u32 overflows;
};

void rx_pump_reset();

// Registers the UART RX handler with the shim. Call after hal_reset().
void rx_pump_install();

// Runs the main loop for `main_iters` iterations. Interrupts are delivered
// by the shim at MMIO accesses, so iteration count is the firmware's notion
// of time here.
void rx_pump_run(Variant variant, rwd::u32 main_iters);

const State& rx_pump_state();

// Overwrites the firmware's state wholesale.
//
// This is what makes reverse execution cheap. Restoring a checkpoint in a
// general-purpose debugger means restoring a process -- which is why rr
// forks. Here the entire state of the system under replay is this struct,
// forty-odd bytes of it, because that is what bare-metal firmware is: no
// heap, no dynamic objects, everything in static storage and visible from
// the outside. Snapshotting it is a copy.
//
// Only meaningful between main-loop iterations, where no call is in
// progress and the shim's interrupt mask is balanced.
void rx_pump_restore(const State& state);

// FNV-1a over every field, field by field rather than over the struct, so
// padding bytes cannot make two identical states hash differently.
rwd::u64 rx_pump_state_hash();

} // namespace fw

#endif // FW_RX_PUMP_H
