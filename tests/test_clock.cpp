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

// -- the reference clock --

namespace {

// A free-running reference, `cpt` cycles per tick, that reads what the real
// time says it should (optionally running fast or slow by `ppm`).
u32 reference_at(u64 true_cycles, u32 cpt, long ppm) {
    const long double t = (long double)true_cycles * (1.0L + (long double)ppm / 1e6L);
    return (u32)(u64)(t / (long double)cpt);
}

struct Lcg {
    u64 s;
    explicit Lcg(u64 seed) : s(seed) {}
    u64 next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return s >> 11;
    }
};

} // namespace

TEST_CASE("without a reference, a silence of two periods looks like none") {
    // The limit this whole section exists for, stated as a test so that it
    // cannot be forgotten: the counter alone cannot tell.
    rwd::ClockExtender c;
    REQUIRE(c.extend(100u) == 100u);
    const u64 got = c.extend(100u + 0x00001000u);       // really 2 periods + 0x1000 later
    CHECK(got == 100u + 0x1000u);                      // so it reports 0x1000 and no wrap
    CHECK_FALSE(c.has_reference());
}

TEST_CASE("a reference rejects what cannot work and leaves things as they were") {
    rwd::ClockExtender c;
    CHECK_FALSE(c.set_reference(0u));
    CHECK_FALSE(c.set_reference(0x20000001u));         // a tick over an eighth of a period
    CHECK_FALSE(c.has_reference());
    CHECK(c.set_reference(0x20000000u));
    CHECK(c.has_reference());
    CHECK(c.set_reference(5127u));
}

TEST_CASE("a reference finds the wraps the counter cannot") {
    rwd::ClockExtender c;
    REQUIRE(c.set_reference(5127u));                   // 168 MHz against 32.768 kHz
    const u32 cpt = 5127u;

    u64 t = 1000u;
    REQUIRE(c.extend((u32)t, reference_at(t, cpt, 0)) == t);

    // Silences of 0.2, 1.3, 2.9, 4.5 periods; each wrapped the counter a different
    // number of times, and the counter alone would have said "at most one".
    const double gaps[] = {0.2, 1.3, 2.9, 4.5};
    for (std::size_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]); ++i) {
        t += (u64)(gaps[i] * 4294967296.0);
        const u64 got = c.extend((u32)t, reference_at(t, cpt, 0));
        CAPTURE(i);
        CHECK(got == t);
    }
    CHECK(c.recovered() > 0u);
    CHECK(c.doubtful() == 0u);
    CHECK(c.wraps() == (u32)(t >> 32));
}

TEST_CASE("the reference gives the same answer as the counter when nothing was missed") {
    rwd::ClockExtender plain, ref;
    REQUIRE(ref.set_reference(5127u));
    Lcg rng(7);
    u64 t = 0;
    for (int i = 0; i < 20000; ++i) {
        t += rng.next() % 0x30000000ULL;               // up to 0.19 period: the counter is enough
        const u64 a = plain.extend((u32)t);
        const u64 b = ref.extend((u32)t, reference_at(t, 5127u, 0));
        CAPTURE(i);
        REQUIRE(a == b);
    }
    CHECK(ref.recovered() == 0u);
    CHECK(plain.suspicious() == 0u);
}

TEST_CASE("a hundred thousand random silences, with a drifting reference") {
    // The reference is not exact: here it runs 200 ppm fast, and is quantised to
    // a tick. Silences of up to six periods.
    for (long ppm = -200; ppm <= 200; ppm += 400) {
        rwd::ClockExtender c;
        REQUIRE(c.set_reference(5127u));
        Lcg rng((u64)(ppm + 1000));
        u64 t = rng.next() % 0xFFFFFFFFULL;
        REQUIRE(c.extend((u32)t, reference_at(t, 5127u, ppm)) == t);
        for (int i = 0; i < 100000; ++i) {
            t += rng.next() % (6ULL << 32);
            const u64 got = c.extend((u32)t, reference_at(t, 5127u, ppm));
            if (got != t) {
                CAPTURE(ppm);
                CAPTURE(i);
                CAPTURE(t);
                CAPTURE(got);
                FAIL("a reference within tolerance must be exact");
            }
        }
        CHECK(c.doubtful() == 0u);
        CHECK(c.recovered() > 100000u);
    }
}

TEST_CASE("the reference may itself wrap") {
    // A coarse reference (a tick is 2^29 cycles, the most allowed) wraps its own
    // 32 bits after 2^61 cycles; steps of 2^58 take it round in under 40 samples.
    rwd::ClockExtender c;
    const u32 cpt = 0x20000000u;
    REQUIRE(c.set_reference(cpt));
    u64 t = 0;
    REQUIRE(c.extend(0u, 0u) == 0u);
    bool wrapped = false;
    u32 last_ref = 0;
    for (int i = 0; i < 80; ++i) {
        t += 1ULL << 58;
        const u32 ref = reference_at(t, cpt, 0);
        wrapped = wrapped || ref < last_ref;
        last_ref = ref;
        CAPTURE(i);
        REQUIRE(c.extend((u32)t, ref) == t);
    }
    CHECK(wrapped);
}

TEST_CASE("a wrong calibration is reported instead of believed") {
    // The reference runs at a thousandth of the speed it was told. Its estimate
    // then bears no relation to the counter's step -- off by a random fraction
    // of a period -- and the disagreement is counted. (Not every sample shows
    // it: a short silence has no room to disagree, and is read right anyway.)
    rwd::ClockExtender c;
    REQUIRE(c.set_reference(5127u));
    Lcg rng(3);
    u64 t = 0;
    REQUIRE(c.extend(0u, 0u) == 0u);
    for (int i = 0; i < 400; ++i) {
        t += (1ULL << 32) + rng.next() % (4ULL << 32);
        c.extend((u32)t, reference_at(t, 5127u * 1000u, 0));
    }
    CHECK(c.doubtful() > 50u);

    // The same silences with the right calibration raise no doubt at all.
    rwd::ClockExtender good;
    REQUIRE(good.set_reference(5127u));
    Lcg again(3);
    t = 0;
    REQUIRE(good.extend(0u, 0u) == 0u);
    for (int i = 0; i < 400; ++i) {
        t += (1ULL << 32) + again.next() % (4ULL << 32);
        REQUIRE(good.extend((u32)t, reference_at(t, 5127u, 0)) == t);
    }
    CHECK(good.doubtful() == 0u);
}

TEST_CASE("reset keeps the reference's calibration") {
    rwd::ClockExtender c;
    REQUIRE(c.set_reference(5127u));
    c.extend(5u, 1u);
    c.extend(0xF0000000u, 0x00080000u);
    c.reset();
    CHECK(c.has_reference());
    CHECK(c.wraps() == 0u);
    CHECK(c.recovered() == 0u);
    CHECK(c.doubtful() == 0u);
    CHECK(c.extend(77u, 5u) == 77u);                   // anchors again
}

