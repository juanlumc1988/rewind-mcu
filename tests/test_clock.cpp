// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "doctest.h"

#include "rewind/clock.h"

using rwd::u32;
using rwd::u64;

TEST_CASE("readings below the wrap pass through unchanged") {
    rwd::ClockExtender clock;
    CHECK(clock.extend(0u) == 0u);
    CHECK(clock.extend(1000u) == 1000u);
    CHECK(clock.extend(0x7FFFFFFFu) == 0x7FFFFFFFu);
    CHECK(clock.extend(0xFFFFFFFFu) == 0xFFFFFFFFu);
    CHECK(clock.wraps() == 0u);
}

TEST_CASE("a wrap carries into the high word") {
    rwd::ClockExtender clock;
    REQUIRE(clock.extend(0xFFFFFFF0u) == 0xFFFFFFF0ULL);
    CHECK(clock.extend(0x00000010u) == 0x0000000100000010ULL);
    CHECK(clock.wraps() == 1u);
    CHECK(clock.suspicious() == 0u);
}

TEST_CASE("many wraps accumulate") {
    rwd::ClockExtender clock;
    clock.extend(0u);
    for (u32 i = 1; i <= 100u; ++i) {
        // Four samples per lap. Two would put each step at exactly half a
        // period, which is the threshold, and so would trip suspicious() --
        // correctly, since a sampler running that close to the limit is one
        // scheduling hiccup away from missing a wrap outright.
        clock.extend(0x40000000u);
        clock.extend(0x80000000u);
        clock.extend(0xC0000000u);
        const u64 got = clock.extend(0x00000000u);
        CAPTURE(i);
        CHECK(clock.wraps() == i);
        CHECK(got == ((u64)i << 32));
    }
    CHECK(clock.suspicious() == 0u);
}

TEST_CASE("the first reading anchors rather than counting a wrap") {
    // A core reset that leaves DWT running starts part-way round the dial.
    rwd::ClockExtender clock;
    CHECK(clock.extend(0xDEADBEEFu) == 0xDEADBEEFULL);
    CHECK(clock.wraps() == 0u);
    CHECK(clock.extend(0xDEADBEF0u) == 0xDEADBEF0ULL);
    CHECK(clock.wraps() == 0u);
}

TEST_CASE("an over-long sample gap is reported, not hidden") {
    rwd::ClockExtender clock;
    clock.extend(0u);
    clock.extend(0x90000000u);   // more than half a period in one step
    CHECK(clock.suspicious() == 1u);
    CHECK(clock.wraps() == 0u);  // still no wrap observed -- only doubt
}

TEST_CASE("reset clears everything") {
    rwd::ClockExtender clock;
    clock.extend(0xFFFFFFF0u);
    clock.extend(0x10u);
    REQUIRE(clock.wraps() == 1u);

    clock.reset();
    CHECK(clock.wraps() == 0u);
    CHECK(clock.value() == 0u);
    CHECK(clock.suspicious() == 0u);
    CHECK(clock.extend(0x500u) == 0x500ULL);   // and re-anchors
}

TEST_CASE("extension reconstructs a real 64-bit counter") {
    // Drive a true 64-bit counter forward in bounded random steps, feed the
    // extender only its low 32 bits, and require the original back.
    rwd::ClockExtender clock;
    u64                truth = 0;
    u64                rng   = 0xB5026F5AA96619E9ULL;

    clock.extend((u32)truth);

    for (u32 step = 0; step < 300000u; ++step) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;

        // Steps stay well under half a period, which is the documented
        // precondition: sample more often than the counter wraps.
        const u64 delta = 1ULL + (rng % 0x20000000ULL);
        truth += delta;

        const u64 got = clock.extend((u32)(truth & 0xFFFFFFFFu));
        REQUIRE(got == truth);
    }

    CHECK(clock.wraps() > 100u);     // the test actually crossed wraps
    CHECK(clock.suspicious() == 0u);
}

TEST_CASE("extended timestamps stay monotonic, which is what the writer needs") {
    // The trace writer latches an error on a backwards timestamp. This is
    // the property that keeps it from ever seeing one.
    rwd::ClockExtender clock;
    u64                previous = 0;
    u64                rng      = 0x9E3779B97F4A7C15ULL;
    u64                truth    = 0;

    clock.extend(0u);
    for (u32 step = 0; step < 200000u; ++step) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        truth += 1ULL + (rng % 100000ULL);

        const u64 got = clock.extend((u32)(truth & 0xFFFFFFFFu));
        REQUIRE(got >= previous);
        previous = got;
    }
}
