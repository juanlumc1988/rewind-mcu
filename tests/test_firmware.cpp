// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The host tools on a firmware that is not the example.
//
// Everything else in this suite replays the example firmware, so a dependence
// on it anywhere in the replay, the timeline, the document or the diff would go
// unnoticed. This one has its own: a button and an LED, different registers,
// different state, a different build id, and none of the example's types. It is
// recorded against a device model written here, replayed from the trace alone,
// walked backwards, shown and diffed -- all through Firmware.

#include "doctest.h"

#include "support/blinker.h"

#include <cstring>
#include <string>
#include <vector>

#include "rewind/firmware.h"
#include "rewind/hal.h"
#include "rewind/session.h"
#include "rewind/timeline.h"
#include "rewind/tracediff.h"
#include "rewind/tracedoc.h"

using rwd::u8;
using rwd::u32;
using rwd::u64;

using namespace blinker;

namespace {

BlinkState decode_checked(const rwhost::Snapshot& s) {
    REQUIRE(s.state.size() == sizeof(BlinkState));
    return decode(s);
}

} // namespace

TEST_CASE("a firmware that is not the example records and replays from the trace alone") {
    const rwhost::FirmwareRecording rec = record(5);
    REQUIRE(rec.writer_ok);
    CHECK(rec.events > 500u);
    const BlinkState recorded = g_state;
    CHECK(recorded.presses > 5u);
    CHECK(recorded.irqs > 5u);

    // The device is gone; only the trace is left.
    Blinker fw;
    const rwhost::ReplayResult rep = rwhost::replay_firmware(fw, rec.trace, kIters);
    REQUIRE(rep.loaded);
    CHECK_FALSE(rep.diverged);
    CHECK(rep.divergence.empty());
    CHECK(rep.events_consumed + 1 == rep.events_total);
    CHECK(rep.fw.state_hash == rec.state_hash);
    CHECK(std::memcmp(&g_state, &recorded, sizeof(recorded)) == 0);
}

TEST_CASE("the default state hash is a hash of the saved bytes") {
    Blinker fw;
    fw.reset();
    const u64 empty = fw.state_hash();
    g_state.presses = 1;
    CHECK(fw.state_hash() != empty);

    std::vector<u8> bytes(fw.state_size());
    fw.save_state(bytes.data());
    u64 h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        h = (h ^ bytes[i]) * 1099511628211ULL;
    }
    CHECK(fw.state_hash() == h);
}

TEST_CASE("replaying with the wrong firmware is noticed") {
    const rwhost::FirmwareRecording rec = record(5);
    Blinker wrong(true);                      // drives the LED the other way round
    const rwhost::ReplayResult rep = rwhost::replay_firmware(wrong, rec.trace, kIters);
    REQUIRE(rep.loaded);
    CHECK(rep.diverged);
    CHECK(rep.divergence.find("wrote") != std::string::npos);
    CHECK(rep.events_consumed < rep.events_total);
}

TEST_CASE("a firmware's state can be visited at any event, forwards and back") {
    const rwhost::FirmwareRecording rec = record(7);
    REQUIRE(rec.writer_ok);

    Blinker fw;
    rwhost::Timeline tl;
    REQUIRE(tl.open(rec.trace, &fw, kIters, 64));
    CHECK(tl.checkpoints() >= 9u);
    CHECK(tl.event_count() + 1 == rec.events);

    rwhost::Snapshot start, end, mid;
    REQUIRE(tl.seek(0, &start));
    CHECK(decode_checked(start).presses == 0u);
    CHECK(start.state_hash != 0u);

    REQUIRE(tl.seek(tl.event_count(), &end));
    CHECK(end.state_hash == rec.state_hash);
    CHECK(decode_checked(end).presses > 5u);

    // Presses only ever go up, so stepping back never finds more of them, and
    // every snapshot is exactly what a fresh replay to that event gives.
    u32 last = decode_checked(end).presses;
    for (std::size_t at = tl.event_count(); at > 0; at -= (at > 211 ? 211 : at)) {
        REQUIRE(tl.seek(at, &mid));
        CAPTURE(at);
        CHECK(decode_checked(mid).presses <= last);
        last = decode_checked(mid).presses;
        CHECK(mid.fields.size() == 7u);
        CHECK(mid.fields[1].name == "presses");
        CHECK(mid.fields[1].value == decode_checked(mid).presses);
    }

    // And the example's typed view is not filled for a firmware that is not it.
    CHECK(end.fw.consumed == 0u);

    // Bisection finds the first moment the firmware had seen three presses.
    struct P {
        static bool three(const rwhost::Snapshot& s, void*) { return decode_checked(s).presses >= 3; }
    };
    rwhost::Snapshot found, before;
    REQUIRE(tl.find_first(&P::three, nullptr, &found));
    CHECK(decode_checked(found).presses == 3u);
    REQUIRE(found.event_index > 0);
    REQUIRE(tl.seek(found.event_index - 1, &before));
    CHECK(decode_checked(before).presses == 2u);
}

TEST_CASE("a document knows a firmware by the factory it is given") {
    const rwhost::FirmwareRecording rec = record(5);

    // The default factory knows the example's builds, and this is not one.
    rwhost::TraceDoc plain;
    REQUIRE(plain.open(rec.trace, kIters));
    CHECK_FALSE(plain.has_firmware());
    CHECK(std::string(plain.variant_name()) == "unknown");
    rwhost::Snapshot s;
    CHECK_FALSE(plain.snapshot_at(10, &s));
    CHECK(plain.events().size() > 500u);            // still readable as events

    rwhost::TraceDoc doc;
    doc.set_firmware_factory(&blinker::factory);
    REQUIRE(doc.open(rec.trace, kIters));
    REQUIRE(doc.has_firmware());
    CHECK(std::string(doc.variant_name()) == "blinker");
    REQUIRE(doc.snapshot_at(doc.events().size() - 1, &s));
    CHECK(s.state_hash == rec.state_hash);
    CHECK(s.fields.size() == 7u);

    // A factory that does not know the build leaves it as readable events.
    rwhost::TraceDoc other;
    other.set_firmware_factory([](u64) { return std::unique_ptr<rwhost::Firmware>(); });
    REQUIRE(other.open(rec.trace, kIters));
    CHECK_FALSE(other.has_firmware());
}

TEST_CASE("two runs of another firmware are compared field by field") {
    const rwhost::FirmwareRecording a = record(5);
    const rwhost::FirmwareRecording b = record(5, true);    // same device, LED inverted
    REQUIRE(a.writer_ok);
    REQUIRE(b.writer_ok);

    rwhost::TraceDoc da, db;
    da.set_firmware_factory(&blinker::factory);
    db.set_firmware_factory(&blinker::factory);
    REQUIRE(da.open(a.trace, kIters));
    REQUIRE(db.open(b.trace, kIters));

    // Same inputs, so the same state; only what the firmware wrote differs, and
    // writes are output, not state. The diff finds the first write.
    const rwhost::TraceDiff d = rwhost::diff_traces(da.events(), db.events());
    REQUIRE_FALSE(d.identical);
    CHECK(d.first < 10u);
    rwhost::Snapshot sa, sb;
    REQUIRE(da.snapshot_at(d.first, &sa));
    REQUIRE(db.snapshot_at(d.first, &sb));
    CHECK(rwhost::diff_fields(sa.fields, sb.fields).empty());

    // Two states that do differ: this run's end against its start.
    REQUIRE(da.snapshot_at(0, &sa));
    REQUIRE(da.snapshot_at(da.events().size() - 1, &sb));
    const std::vector<rwhost::StateDelta> delta = rwhost::diff_fields(sa.fields, sb.fields);
    REQUIRE_FALSE(delta.empty());
    bool presses = false;
    for (std::size_t i = 0; i < delta.size(); ++i) {
        if (delta[i].name == "presses") {
            presses = true;
            CHECK(delta[i].a == 0u);
            CHECK(delta[i].b > 5u);
        }
    }
    CHECK(presses);
}

TEST_CASE("field comparison matches by name, and a missing field is zero") {
    using rwhost::StateField;
    std::vector<StateField> a, b;
    a.push_back(StateField("x", 1));
    a.push_back(StateField("y", 2));
    a.push_back(StateField("gone", 5));
    b.push_back(StateField("y", 2));
    b.push_back(StateField("x", 7));
    b.push_back(StateField("new", 9));
    b.push_back(StateField("quiet", 0));

    const std::vector<rwhost::StateDelta> d = rwhost::diff_fields(a, b);
    // x differs (found though it moved); y is the same; `gone` and `new` exist on
    // one side only; `quiet` is zero where it is missing, so not a difference.
    REQUIRE(d.size() == 3u);
    CHECK(d[0].name == "x");     CHECK(d[0].a == 1u);  CHECK(d[0].b == 7u);
    CHECK(d[1].name == "gone");  CHECK(d[1].a == 5u);  CHECK(d[1].b == 0u);
    CHECK(d[2].name == "new");   CHECK(d[2].a == 0u);  CHECK(d[2].b == 9u);

    CHECK(rwhost::diff_fields(a, a).empty());
    CHECK(rwhost::diff_fields(std::vector<StateField>(), std::vector<StateField>()).empty());
}

TEST_CASE("a single region of the default restore puts back the whole state") {
    Blinker fw;
    fw.reset();
    g_state.presses = 9;
    g_state.irqs    = 4;
    std::vector<u8> image(fw.state_size());
    fw.save_state(image.data());

    fw.reset();
    CHECK(g_state.presses == 0u);
    CHECK(fw.restore_region(0, image.data(), image.size()));
    CHECK(g_state.presses == 9u);
    CHECK(g_state.irqs == 4u);

    // Not region 0, or not the whole state: refused, and nothing changes.
    fw.reset();
    CHECK_FALSE(fw.restore_region(1, image.data(), image.size()));
    CHECK_FALSE(fw.restore_region(0, image.data(), image.size() - 1));
    CHECK(g_state.presses == 0u);
}
