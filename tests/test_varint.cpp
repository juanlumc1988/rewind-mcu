// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "doctest.h"

#include "rewind/varint.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

// Encodes then decodes, asserting the round trip and that both directions
// agree with varint_size().
void round_trip(u64 value, u32 expected_bytes) {
    u8 buf[rwd::kVarintMaxBytes];
    const u32 written = rwd::varint_encode(value, buf);

    CHECK(written == expected_bytes);
    CHECK(rwd::varint_size(value) == written);

    u64       decoded = 0;
    const u32 read    = rwd::varint_decode(buf, written, &decoded);

    CHECK(read == written);
    CHECK(decoded == value);
}

} // namespace

TEST_CASE("varint round-trips at every length boundary") {
    // One extra bit of payload per byte; these are the values either side of
    // each step, which is where an off-by-one in the shift would show up.
    round_trip(0u, 1);
    round_trip(127u, 1);
    round_trip(128u, 2);
    round_trip(16383u, 2);
    round_trip(16384u, 3);
    round_trip(2097151u, 3);
    round_trip(2097152u, 4);
    round_trip(268435455u, 4);
    round_trip(268435456u, 5);
    round_trip(34359738367ULL, 5);
    round_trip(34359738368ULL, 6);
    round_trip(4398046511103ULL, 6);
    round_trip(4398046511104ULL, 7);
    round_trip(562949953421311ULL, 7);
    round_trip(562949953421312ULL, 8);
    round_trip(72057594037927935ULL, 8);
    round_trip(72057594037927936ULL, 9);
    round_trip(9223372036854775807ULL, 9);
    round_trip(9223372036854775808ULL, 10);   // bit 63 set: needs the tenth
    round_trip(18446744073709551615ULL, 10);  // UINT64_MAX
}

TEST_CASE("varint round-trips values likely to appear in a trace") {
    // Peripheral addresses, cycle deltas, register values.
    const u64 samples[] = {
        1u, 3u, 7u, 42u, 0x40000000u, 0x40001004u, 0xFFFFFFFFu,
        0xDEADBEEFu, 0xCAFEBABEu, 1000000u
    };
    for (u32 i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        u8        buf[rwd::kVarintMaxBytes];
        const u32 n = rwd::varint_encode(samples[i], buf);
        u64       out = 0;
        CHECK(rwd::varint_decode(buf, n, &out) == n);
        CHECK(out == samples[i]);
    }
}

TEST_CASE("varint_decode rejects truncated input") {
    u64 out = 0xA5A5A5A5u;

    SUBCASE("zero available bytes") {
        const u8 buf[1] = { 0x00 };
        CHECK(rwd::varint_decode(buf, 0, &out) == 0);
    }
    SUBCASE("continuation bit with nothing following") {
        const u8 buf[1] = { 0x80 };
        CHECK(rwd::varint_decode(buf, 1, &out) == 0);
    }
    SUBCASE("cut off mid-value") {
        const u8 buf[3] = { 0xFF, 0xFF, 0xFF };
        CHECK(rwd::varint_decode(buf, 3, &out) == 0);
    }

    // A rejected decode must not touch the output.
    CHECK(out == 0xA5A5A5A5u);
}

TEST_CASE("varint_decode rejects values that overflow u64") {
    u64 out = 0;

    SUBCASE("tenth byte carries more than bit 63") {
        // Nine full groups is 63 bits; the tenth may only contribute one
        // more. A payload of 2 here would shift out of the top of the word.
        const u8 buf[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                             0xFF, 0xFF, 0xFF, 0xFF, 0x02 };
        CHECK(rwd::varint_decode(buf, 10, &out) == 0);
    }
    SUBCASE("continuation past the tenth byte") {
        const u8 buf[11] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                             0xFF, 0xFF, 0xFF, 0x81, 0x00 };
        CHECK(rwd::varint_decode(buf, 11, &out) == 0);
    }
    SUBCASE("tenth byte carrying exactly bit 63 is accepted") {
        const u8 buf[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                             0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
        CHECK(rwd::varint_decode(buf, 10, &out) == 10);
        CHECK(out == 18446744073709551615ULL);
    }
}

TEST_CASE("varint_decode ignores trailing bytes") {
    // Events sit back to back in a block, so a decode must stop at its own
    // terminator rather than running into the next event.
    const u8 buf[4] = { 0x2A, 0xFF, 0xFF, 0xFF };
    u64       out   = 0;
    CHECK(rwd::varint_decode(buf, 4, &out) == 1);
    CHECK(out == 42u);
}

TEST_CASE("zigzag keeps small magnitudes small in both directions") {
    // The property that makes it worth using: a delta of -1 must not cost
    // ten bytes just because it is negative.
    CHECK(rwd::zigzag_encode(0) == 0u);
    CHECK(rwd::zigzag_encode(-1) == 1u);
    CHECK(rwd::zigzag_encode(1) == 2u);
    CHECK(rwd::zigzag_encode(-2) == 3u);
    CHECK(rwd::zigzag_encode(2) == 4u);

    CHECK(rwd::varint_size(rwd::zigzag_encode(-4)) == 1u);
    CHECK(rwd::varint_size(rwd::zigzag_encode(4)) == 1u);
}

TEST_CASE("zigzag round-trips, including the extremes") {
    const rwd::i64 samples[] = {
        0, 1, -1, 2, -2, 63, -63, 64, -64, 1000, -1000,
        4096, -4096, 0x7FFFFFFFLL, -0x80000000LL,
        9223372036854775807LL,              // INT64_MAX
        -9223372036854775807LL - 1LL        // INT64_MIN, spelled so the
                                            // literal itself never overflows
    };
    for (u32 i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        CAPTURE(i);
        CHECK(rwd::zigzag_decode(rwd::zigzag_encode(samples[i])) == samples[i]);
    }
}

TEST_CASE("zigzag round-trips through a varint") {
    // The combination as the trace writer actually uses it: an address delta
    // that can point either way.
    for (rwd::i64 delta = -2048; delta <= 2048; ++delta) {
        u8        buf[rwd::kVarintMaxBytes];
        const u32 n = rwd::varint_encode(rwd::zigzag_encode(delta), buf);
        u64       raw = 0;
        REQUIRE(rwd::varint_decode(buf, n, &raw) == n);
        CHECK(rwd::zigzag_decode(raw) == delta);
    }
}

TEST_CASE("address deltas from a real access pattern stay one byte") {
    // The four registers the example firmware touches. Every delta between
    // them fits in a single varint byte, where each absolute address would
    // cost five.
    const rwd::u32 addrs[] = { 0x40000000u, 0x40001000u, 0x40001004u, 0x40002000u };
    for (u32 i = 0; i < 4u; ++i) {
        CHECK(rwd::varint_size((u64)addrs[i]) == 5u);
        for (u32 j = 0; j < 4u; ++j) {
            const rwd::i64 delta = (rwd::i64)addrs[i] - (rwd::i64)addrs[j];
            CAPTURE(i);
            CAPTURE(j);
            CHECK(rwd::varint_size(rwd::zigzag_encode(delta)) <= 3u);
        }
    }
    // Back-to-back accesses to the same register -- the common case -- cost
    // nothing at all beyond the terminator byte.
    CHECK(rwd::varint_size(rwd::zigzag_encode(0)) == 1u);
}
