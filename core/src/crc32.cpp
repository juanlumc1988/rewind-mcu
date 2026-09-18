// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/crc32.h"

namespace rwd {
namespace {

// crc32_nibble_table[i] == the reflected remainder of the 4-bit value i.
// Verified against zlib.crc32 over the standard check vectors; see
// tests/test_crc32.cpp.
const u32 kNibbleTable[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

} // namespace

u32 crc32_init() {
    return 0xFFFFFFFFu;
}

u32 crc32_final(u32 crc) {
    return crc ^ 0xFFFFFFFFu;
}

u32 crc32_update(u32 crc, const u8* data, u32 len) {
    for (u32 i = 0; i < len; ++i) {
        crc ^= (u32)data[i];
        crc = (crc >> 4) ^ kNibbleTable[crc & 0x0Fu];
        crc = (crc >> 4) ^ kNibbleTable[crc & 0x0Fu];
    }
    return crc;
}

u32 crc32(const u8* data, u32 len) {
    return crc32_final(crc32_update(crc32_init(), data, len));
}

} // namespace rwd
