// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/varint.h"

namespace rwd {

u32 varint_encode(u64 v, u8* out) {
    u32 n = 0;
    while (v >= 0x80u) {
        out[n++] = (u8)(v | 0x80u);
        v >>= 7;
    }
    out[n++] = (u8)v;
    return n;
}

u32 varint_size(u64 v) {
    u32 n = 1;
    while (v >= 0x80u) {
        v >>= 7;
        ++n;
    }
    return n;
}

u32 varint_decode(const u8* in, u32 len, u64* out) {
    u64 result = 0;
    u32 shift  = 0;

    const u32 limit = (len < kVarintMaxBytes) ? len : kVarintMaxBytes;
    for (u32 i = 0; i < limit; ++i) {
        const u8  byte    = in[i];
        const u64 payload = (u64)(byte & 0x7Fu);

        // The tenth byte contributes bit 63 and nothing above it. Anything
        // larger is an encoding that overflows u64, which we reject rather
        // than silently truncate.
        if (shift == 63 && payload > 1u) {
            return 0;
        }

        result |= payload << shift;

        if ((byte & 0x80u) == 0) {
            *out = result;
            return i + 1;
        }
        shift += 7;
    }

    // Truncated input, or a continuation bit set on the tenth byte.
    return 0;
}

} // namespace rwd
