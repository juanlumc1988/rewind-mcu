// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Unsigned LEB128. Trace events are dominated by small numbers -- timestamp
// deltas between consecutive MMIO accesses are typically a handful of cycles,
// and peripheral addresses cluster -- so a varint costs one byte where a
// fixed u32 costs four. That ratio is what keeps the on-target ring buffer
// small enough to be affordable.

#ifndef REWIND_VARINT_H
#define REWIND_VARINT_H

#include "rewind/rw_types.h"

namespace rwd {

// ceil(64 / 7) == 10 bytes for the widest value.
const u32 kVarintMaxBytes = 10;

// Encodes `v` into `out`, which must have room for kVarintMaxBytes.
// Returns the number of bytes written (1..kVarintMaxBytes).
u32 varint_encode(u64 v, u8* out);

// Number of bytes varint_encode would write for `v`.
u32 varint_size(u64 v);

// Decodes from [in, in + len). On success stores the value in *out and
// returns the number of bytes consumed. Returns 0 -- and leaves *out
// untouched -- if the input is truncated, longer than kVarintMaxBytes, or
// encodes a value that does not fit in u64.
u32 varint_decode(const u8* in, u32 len, u64* out);

// ZigZag: maps signed values onto unsigned ones so that small magnitudes of
// either sign stay small, which is what LEB128 rewards. -1 becomes 1, 1
// becomes 2, -2 becomes 3, and so on.
//
// Written without shifting a negative signed value: that is
// implementation-defined before C++20, and this code has to survive
// compilers older than the idea of defining it.
inline u64 zigzag_encode(i64 v) {
    if (v < 0) {
        // -(v + 1) rather than -v, so INT64_MIN does not overflow.
        return ((u64)(-(v + 1)) << 1) | 1u;
    }
    return (u64)v << 1;
}

inline i64 zigzag_decode(u64 v) {
    if (v & 1u) {
        return -(i64)(v >> 1) - 1;
    }
    return (i64)(v >> 1);
}

} // namespace rwd

#endif // REWIND_VARINT_H
