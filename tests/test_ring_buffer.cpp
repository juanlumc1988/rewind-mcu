// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "doctest.h"

#include <deque>
#include <vector>

#include "rewind/ring_buffer.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

namespace {

std::vector<u8> pattern(u32 len, u8 seed) {
    std::vector<u8> out(len);
    for (u32 i = 0; i < len; ++i) {
        out[i] = (u8)((i * 31u + seed) & 0xFFu);
    }
    return out;
}

// Drains everything currently readable, following the wrap.
std::vector<u8> drain(rwd::RingBuffer& rb) {
    std::vector<u8> out;
    for (;;) {
        const u8* chunk = 0;
        const u32 n     = rb.peek(&chunk);
        if (n == 0) {
            break;
        }
        out.insert(out.end(), chunk, chunk + n);
        REQUIRE(rb.consume(n));
    }
    return out;
}

} // namespace

TEST_CASE("init accepts only a power-of-two capacity") {
    u8              storage[256];
    rwd::RingBuffer rb;

    CHECK(rb.init(storage, 256));
    CHECK(rb.valid());
    CHECK(rb.capacity() == 256u);

    SUBCASE("rejected shapes leave the buffer unusable rather than half set") {
        CHECK_FALSE(rb.init(storage, 100));   // not a power of two
        CHECK_FALSE(rb.valid());
        CHECK_FALSE(rb.init(storage, 0));
        CHECK_FALSE(rb.init(storage, 1));     // below the minimum
        CHECK_FALSE(rb.init(0, 256));         // null storage
        CHECK_FALSE(rb.init(storage, 0x80000001u));   // above 2^31
        CHECK_FALSE(rb.push(storage, 1));     // and stays unusable
    }
    SUBCASE("2 and 2^31 are the boundaries and both are accepted") {
        CHECK(rb.init(storage, 2));
        CHECK(rb.init(storage, 0x80000000u));
    }
}

TEST_CASE("bytes come out in the order they went in") {
    u8              storage[64];
    rwd::RingBuffer rb;
    REQUIRE(rb.init(storage, 64));

    const std::vector<u8> a = pattern(10, 1);
    const std::vector<u8> b = pattern(20, 2);
    REQUIRE(rb.push(a.data(), (u32)a.size()));
    REQUIRE(rb.push(b.data(), (u32)b.size()));

    CHECK(rb.used() == 30u);
    CHECK(rb.space() == 34u);

    std::vector<u8> expected = a;
    expected.insert(expected.end(), b.begin(), b.end());
    CHECK(drain(rb) == expected);
    CHECK(rb.used() == 0u);
}

TEST_CASE("a push that does not fit changes nothing") {
    // Half a block in the buffer would pass its length check, fail its CRC,
    // and look exactly like line noise on the wire.
    u8              storage[16];
    rwd::RingBuffer rb;
    REQUIRE(rb.init(storage, 16));

    const std::vector<u8> fits = pattern(10, 7);
    REQUIRE(rb.push(fits.data(), 10));

    const std::vector<u8> too_big = pattern(10, 9);
    CHECK_FALSE(rb.push(too_big.data(), 10));   // only 6 bytes free

    CHECK(rb.used() == 10u);
    CHECK(rb.drops() == 1u);
    CHECK(rb.bytes_dropped() == 10u);
    CHECK(drain(rb) == fits);

    // Exactly filling the remaining space is fine.
    rb.reset();
    REQUIRE(rb.push(fits.data(), 10));
    const std::vector<u8> exact = pattern(6, 3);
    CHECK(rb.push(exact.data(), 6));
    CHECK(rb.used() == 16u);
    CHECK(rb.space() == 0u);
    CHECK(rb.drops() == 0u);
}

TEST_CASE("data that straddles the wrap comes back intact") {
    u8              storage[32];
    rwd::RingBuffer rb;
    REQUIRE(rb.init(storage, 32));

    // Park the write cursor near the end of the storage.
    const std::vector<u8> filler = pattern(28, 5);
    REQUIRE(rb.push(filler.data(), 28));
    REQUIRE(rb.consume(28));
    CHECK(rb.used() == 0u);

    // This one has to split across the end of the array.
    const std::vector<u8> split = pattern(20, 11);
    REQUIRE(rb.push(split.data(), 20));
    CHECK(rb.used() == 20u);

    const u8* chunk = 0;
    const u32 first = rb.peek(&chunk);
    CHECK(first == 4u);        // 32 - 28: capped at the wrap, as documented
    CHECK(first < rb.used());

    CHECK(drain(rb) == split);
}

TEST_CASE("high water mark tracks the worst occupancy seen") {
    u8              storage[128];
    rwd::RingBuffer rb;
    REQUIRE(rb.init(storage, 128));

    const std::vector<u8> data = pattern(100, 1);
    REQUIRE(rb.push(data.data(), 100));
    CHECK(rb.high_water() == 100u);

    REQUIRE(rb.consume(100));
    CHECK(rb.used() == 0u);
    CHECK(rb.high_water() == 100u);   // a peak, not a gauge

    REQUIRE(rb.push(data.data(), 50));
    CHECK(rb.high_water() == 100u);
    REQUIRE(rb.push(data.data(), 60));
    CHECK(rb.high_water() == 110u);
}

TEST_CASE("consume refuses to outrun the data") {
    u8              storage[32];
    rwd::RingBuffer rb;
    REQUIRE(rb.init(storage, 32));

    const std::vector<u8> data = pattern(10, 1);
    REQUIRE(rb.push(data.data(), 10));

    CHECK_FALSE(rb.consume(11));
    CHECK(rb.used() == 10u);   // and the buffer is untouched
    CHECK(rb.consume(10));
    CHECK(rb.used() == 0u);
    CHECK_FALSE(rb.consume(1));
}

TEST_CASE("peek on an empty buffer yields nothing") {
    u8              storage[32];
    rwd::RingBuffer rb;
    REQUIRE(rb.init(storage, 32));

    const u8* chunk = reinterpret_cast<const u8*>(0x1234);
    CHECK(rb.peek(&chunk) == 0u);
    CHECK(chunk == 0);
}

TEST_CASE("the index counters survive 32-bit wraparound") {
    // used() is a subtraction of free-running counters. After 2^32 bytes they
    // wrap, and the subtraction has to stay correct -- a device recording for
    // days will get here, and getting it wrong would corrupt the trace at
    // precisely the least convenient moment.
    const u32       kCap = 65536;
    std::vector<u8> storage(kCap);
    rwd::RingBuffer rb;
    REQUIRE(rb.init(storage.data(), kCap));

    const std::vector<u8> block = pattern(kCap, 0x5A);

    // 65536 full cycles moves each counter through exactly 2^32.
    for (u32 cycle = 0; cycle < 65536u; ++cycle) {
        REQUIRE(rb.push(block.data(), kCap));
        REQUIRE(rb.used() == kCap);
        REQUIRE(rb.consume(kCap));
        REQUIRE(rb.used() == 0u);
    }

    // Still correct on the far side.
    const std::vector<u8> after = pattern(1000, 0x33);
    REQUIRE(rb.push(after.data(), 1000));
    CHECK(rb.used() == 1000u);
    CHECK(drain(rb) == after);
    CHECK(rb.drops() == 0u);
}

TEST_CASE("randomised push and consume agree with a reference model") {
    // The wrap arithmetic has enough corners that hand-picked cases will
    // miss one. A deque says what the answer should be.
    const u32       kCap = 64;
    u8              storage[64];
    rwd::RingBuffer rb;
    REQUIRE(rb.init(storage, kCap));

    std::deque<u8> model;
    u64            rng     = 0x243F6A8885A308D3ULL;
    u32            pushes  = 0;
    u32            rejects = 0;
    u8             next    = 0;

    for (u32 step = 0; step < 200000u; ++step) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;

        if ((rng & 1u) != 0) {
            const u32       len  = (u32)((rng >> 8) % 24u);
            std::vector<u8> data(len);
            for (u32 i = 0; i < len; ++i) {
                data[i] = next++;
            }

            const bool fits = (len <= kCap - (u32)model.size());
            const bool got  = rb.push(data.data(), len);
            REQUIRE(got == fits);

            if (got) {
                model.insert(model.end(), data.begin(), data.end());
                ++pushes;
            } else {
                next -= (u8)len;   // keep the model's byte stream aligned
                ++rejects;
            }
        } else {
            const u32 want = (u32)((rng >> 8) % 24u);
            const u32 len  = (want <= model.size()) ? want : (u32)model.size();
            REQUIRE(rb.consume(len));
            for (u32 i = 0; i < len; ++i) {
                model.pop_front();
            }
        }

        REQUIRE(rb.used() == (u32)model.size());

        // Compare the readable region against the model, without consuming.
        const u8* chunk = 0;
        const u32 n     = rb.peek(&chunk);
        REQUIRE(n <= (u32)model.size());
        for (u32 i = 0; i < n; ++i) {
            REQUIRE(chunk[i] == model[i]);
        }
    }

    // The mix has to have actually exercised both outcomes.
    CHECK(pushes > 1000u);
    CHECK(rejects > 1000u);
    CHECK(rb.drops() == rejects);
}
