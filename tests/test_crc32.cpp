// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Expected values were produced by Python's zlib.crc32, which is the
// reference implementation of CRC-32/ISO-HDLC. A nibble-wise CRC is easy to
// get subtly wrong -- a reversed table, a missing final xor -- and subtly
// wrong is exactly the failure mode that turns a corrupt-trace detector into
// a corrupt-trace generator.

#include "doctest.h"

#include "rewind/crc32.h"

using rwd::u8;
using rwd::u32;

TEST_CASE("crc32 matches the standard check vectors") {
    const u8 check[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    CHECK(rwd::crc32(check, 9) == 0xCBF43926u);

    CHECK(rwd::crc32(0, 0) == 0x00000000u);

    const u8 one[1] = { 'a' };
    CHECK(rwd::crc32(one, 1) == 0xE8B7BE43u);

    const char* pangram = "The quick brown fox jumps over the lazy dog";
    u32 len = 0;
    while (pangram[len] != '\0') {
        ++len;
    }
    CHECK(rwd::crc32(reinterpret_cast<const u8*>(pangram), len) == 0x414FA339u);
}

TEST_CASE("crc32 covers the whole byte range") {
    // Exercises all sixteen table entries in both nibble positions.
    u8 all[256];
    for (u32 i = 0; i < 256u; ++i) {
        all[i] = static_cast<u8>(i);
    }
    CHECK(rwd::crc32(all, 256) == 0x29058C73u);
}

TEST_CASE("incremental crc32 equals the one-shot result") {
    u8 data[128];
    for (u32 i = 0; i < 128u; ++i) {
        data[i] = static_cast<u8>((i * 37u + 11u) & 0xFFu);
    }
    const u32 expected = rwd::crc32(data, 128);

    // Split at every possible point: a block can be flushed anywhere.
    for (u32 split = 0; split <= 128u; ++split) {
        u32 crc = rwd::crc32_init();
        crc = rwd::crc32_update(crc, data, split);
        crc = rwd::crc32_update(crc, data + split, 128u - split);
        CHECK(rwd::crc32_final(crc) == expected);
    }
}

TEST_CASE("crc32 detects single-bit corruption") {
    u8 data[64];
    for (u32 i = 0; i < 64u; ++i) {
        data[i] = static_cast<u8>(i * 3u);
    }
    const u32 clean = rwd::crc32(data, 64);

    for (u32 byte = 0; byte < 64u; ++byte) {
        for (u32 bit = 0; bit < 8u; ++bit) {
            data[byte] ^= static_cast<u8>(1u << bit);
            CHECK(rwd::crc32(data, 64) != clean);
            data[byte] ^= static_cast<u8>(1u << bit);
        }
    }
    CHECK(rwd::crc32(data, 64) == clean);
}
