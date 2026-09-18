// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The buffer between recording and transmitting.
//
// Recording produces bytes in bursts, at whatever rate the firmware happens
// to touch peripherals. A UART drains them at a fixed, much slower rate.
// This sits in between, and its job when it cannot keep up is to fail
// visibly rather than quietly.
//
// Design notes, all of them driven by where this runs:
//
//   No allocation. The caller owns the storage. On a part with no heap the
//   buffer lives in .bss and its size is a link-time decision.
//
//   Power-of-two capacity. Indices are free-running u32 counters masked on
//   use, so `used()` is a subtraction that stays correct across wraparound
//   and there is no full/empty ambiguity to resolve with a spare slot.
//
//   All-or-nothing pushes. A partially written block is worse than no block:
//   it would pass its length check, fail its CRC, and look like line noise.
//   A rejected push leaves the buffer untouched and increments a counter.
//
// Concurrency
// -----------
// One producer, one consumer, no locks: the producer only ever writes head_,
// the consumer only ever writes tail_, and on a 32-bit core an aligned word
// store is atomic. Both indices are volatile so the compiler cannot cache
// one across a point where the other side may change it.
//
// That covers the ordinary arrangement -- recording from the main loop,
// draining from the main loop, or the reverse. It does NOT cover two
// producers. Firmware records from its main loop *and* from inside ISRs, so
// a push can be interrupted by another push, and callers in that situation
// must mask interrupts around push(). RingSink in target/ does exactly that.
//
// This is enough for a single-core Cortex-M, where the only reordering that
// can bite is the compiler's. A port to anything SMP would need real atomics
// with acquire/release ordering, not volatile.

#ifndef REWIND_RING_BUFFER_H
#define REWIND_RING_BUFFER_H

#include "rewind/rw_types.h"

namespace rwd {

class RingBuffer {
public:
    RingBuffer();

    // `capacity` must be a power of two in [2, 2^31]. `storage` must hold at
    // least that many bytes and outlive the buffer. Returns false otherwise,
    // leaving the buffer unusable rather than half-configured.
    bool init(u8* storage, u32 capacity);

    bool valid() const { return store_ != 0; }

    // -- producer side --

    // Appends all `len` bytes, or none of them. Returns false when the bytes
    // do not fit, having changed nothing but the drop counters.
    // A zero-length push succeeds and does nothing, `data` notwithstanding.
    bool push(const u8* data, u32 len);

    // -- consumer side --

    // Points *out at the readable region and returns its length, which is
    // capped at the wrap point and so may be less than used(). Returns 0 and
    // sets *out to null when empty.
    u32 peek(const u8** out) const;

    // Releases `len` bytes from the front. `len` must not exceed used();
    // a larger value is ignored and leaves the buffer intact.
    bool consume(u32 len);

    // -- observation --

    u32 used() const;
    u32 space() const;
    u32 capacity() const { return cap_; }

    // Pushes rejected for want of room, and the bytes they carried. Non-zero
    // means the trace has a hole in it. The block sequence numbers in the
    // trace format are what let a reader find that hole later; these
    // counters are what let the device notice it at the time.
    u32 drops() const { return drops_; }
    u64 bytes_dropped() const { return bytes_dropped_; }

    // Largest occupancy ever reached. The number to look at when sizing the
    // buffer: if it never approaches capacity, the buffer is too big and the
    // .bss is paying for it.
    u32 high_water() const { return high_water_; }

    void reset();

private:
    RingBuffer(const RingBuffer&);
    RingBuffer& operator=(const RingBuffer&);

    u8*           store_;
    u32           cap_;
    u32           mask_;
    volatile u32  head_;   // producer writes, consumer reads
    volatile u32  tail_;   // consumer writes, producer reads
    u32           drops_;
    u64           bytes_dropped_;
    u32           high_water_;
};

} // namespace rwd

#endif // REWIND_RING_BUFFER_H
