// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/ring_buffer.h"

namespace rwd {
namespace {

bool is_power_of_two(u32 v) {
    return v != 0 && (v & (v - 1u)) == 0;
}

// An explicit loop rather than memcpy: <cstring> is not a freestanding
// header, and the compiler recognises this shape and emits the same code.
void copy_bytes(u8* dst, const u8* src, u32 len) {
    for (u32 i = 0; i < len; ++i) {
        dst[i] = src[i];
    }
}

} // namespace

RingBuffer::RingBuffer()
    : store_(0), cap_(0), mask_(0), head_(0), tail_(0),
      drops_(0), bytes_dropped_(0), high_water_(0) {}

bool RingBuffer::init(u8* storage, u32 capacity) {
    // The upper bound keeps used() -- a difference of free-running counters
    // -- unambiguous: with capacity <= 2^31 the subtraction can never be
    // mistaken for a wrap in the other direction.
    if (storage == 0 || capacity < 2u || capacity > 0x80000000u
        || !is_power_of_two(capacity)) {
        store_ = 0;
        cap_   = 0;
        mask_  = 0;
        return false;
    }

    store_         = storage;
    cap_           = capacity;
    mask_          = capacity - 1u;
    head_          = 0;
    tail_          = 0;
    drops_         = 0;
    bytes_dropped_ = 0;
    high_water_    = 0;
    return true;
}

void RingBuffer::reset() {
    head_          = 0;
    tail_          = 0;
    drops_         = 0;
    bytes_dropped_ = 0;
    high_water_    = 0;
}

u32 RingBuffer::used() const {
    // Unsigned subtraction, correct across counter wraparound.
    return head_ - tail_;
}

u32 RingBuffer::space() const {
    return cap_ - used();
}

bool RingBuffer::push(const u8* data, u32 len) {
    if (!valid()) {
        return false;
    }
    // A zero-length push is a no-op, and succeeds even with a null pointer:
    // an empty buffer's data() is allowed to be null, and refusing it would
    // make callers special-case something that means "nothing to do".
    if (len == 0) {
        return true;
    }
    if (data == 0) {
        return false;
    }
    if (len > space()) {
        ++drops_;
        bytes_dropped_ += (u64)len;
        return false;
    }

    const u32 offset = head_ & mask_;
    u32       first  = cap_ - offset;
    if (first > len) {
        first = len;
    }
    copy_bytes(store_ + offset, data, first);
    if (len > first) {
        copy_bytes(store_, data + first, len - first);
    }

    // Published last: until head_ moves, the consumer sees none of it.
    head_ = head_ + len;

    const u32 now = used();
    if (now > high_water_) {
        high_water_ = now;
    }
    return true;
}

u32 RingBuffer::peek(const u8** out) const {
    if (out == 0) {
        return 0;
    }
    const u32 filled = used();
    if (!valid() || filled == 0) {
        *out = 0;
        return 0;
    }

    const u32 offset     = tail_ & mask_;
    u32       contiguous = cap_ - offset;
    if (contiguous > filled) {
        contiguous = filled;
    }
    *out = store_ + offset;
    return contiguous;
}

bool RingBuffer::consume(u32 len) {
    if (len > used()) {
        return false;
    }
    tail_ = tail_ + len;
    return true;
}

} // namespace rwd
