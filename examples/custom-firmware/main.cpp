// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The workflow on a firmware of your own, end to end, and a check that it works:
//
//   1. run the firmware against a model of its hardware and record it
//      (on the part this is the recorder; the trace is the same)
//   2. keep only the trace, as a file
//   3. replay the file with the firmware alone -- no device, no model -- and
//      see it end in the state the recording did
//   4. go back through the run: when did the heater first switch three times?
//      what did the firmware believe just before?
//   5. record from the middle of a run, with the firmware's memory at that point
//      in the trace, and see the replay reach the same end
//
// Exits 0 if every step held. Run it from anywhere:  thermostat_replay [file.rwd]

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "rewind/session.h"
#include "rewind/timeline.h"
#include "rewind/trace.h"
#include "rewind/version.h"
#include "thermostat_device.h"
#include "thermostat_firmware.h"

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failed;
    }
}

rwd::u64 field(const rwhost::Snapshot& s, const char* name) {
    for (std::size_t i = 0; i < s.fields.size(); ++i) {
        if (s.fields[i].name == name) {
            return s.fields[i].value;
        }
    }
    return ~0ULL;
}

bool switched_three_times(const rwhost::Snapshot& s, void*) {
    return field(s, "switches") >= 3;
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "thermostat.rwd";
    const rwd::u32    iters = 4000;
    const rwd::u32    seed  = 7;

    std::printf("rewind %s -- the thermostat, a firmware that is not the example\n\n", REWIND_VERSION);

    // 1. Record it.
    std::printf("record\n");
    thermostat::Device   device(seed);
    thermostat::Firmware recorded;
    const rwhost::FirmwareRecording rec = rwhost::record_firmware(
        recorded, device.ops(), thermostat::kBuildId, seed, rwd::kFlagRecordWrites, iters);
    check(rec.writer_ok, "the trace was written");
    check(rec.events > 1000, "and it has events in it");
    const thermostat::State at_end = thermostat::state();
    std::printf("  %u events, %zu bytes; the heater switched %u times, hottest %u.%u\n\n", rec.events,
                rec.trace.size(), at_end.switches, at_end.hottest / 10, at_end.hottest % 10);

    // 2. Keep only the file.
    {
        std::ofstream out(path.c_str(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(rec.trace.data()), (std::streamsize)rec.trace.size());
    }
    std::vector<rwd::u8> trace;
    {
        std::ifstream in(path.c_str(), std::ios::binary);
        trace.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    check(trace == rec.trace, "the file holds the trace exactly");

    // 3. Replay it without the device.
    std::printf("\nreplay, from the file alone\n");
    thermostat::Firmware replayed;
    const rwhost::ReplayResult rep = rwhost::replay_firmware(replayed, trace, iters);
    check(rep.loaded, "the file is a trace");
    check(!rep.diverged, "the firmware asked for nothing the trace lacks");
    check(rep.events_consumed + 1 == rep.events_total, "every event was used");
    check(rep.fw.state_hash == rec.state_hash, "it ended in the state the recording did");

    // 4. Go back through the run.
    std::printf("\nwalk the run\n");
    thermostat::Firmware walker;
    rwhost::Timeline timeline;
    check(timeline.open(trace, &walker, iters, 128), "a timeline over the trace");
    rwhost::Snapshot found, before, end;
    check(timeline.find_first(&switched_three_times, 0, &found),
          "found the first moment the heater had switched three times");
    std::printf("  event %zu, cycle %llu, in %u seeks (of %zu events)\n", found.event_index,
                (unsigned long long)found.ts, timeline.last_search_seeks(), timeline.event_count());
    check(field(found, "switches") == 3, "and it is exactly three there");
    check(found.event_index > 0 && timeline.seek(found.event_index - 1, &before), "step back one event");
    check(field(before, "switches") == 2, "one event earlier it was two");
    check(field(before, "heater") != field(found, "heater"), "and the heater had just changed");
    check(timeline.seek(timeline.event_count(), &end) && end.state_hash == rec.state_hash,
          "the end of the timeline is the end of the run");

    // 5. Start the recording in the middle of the run.
    std::printf("\nrecord from the middle\n");
    thermostat::Device   device2(seed);
    thermostat::Firmware part_fw;
    const rwhost::FirmwareRecording part = rwhost::record_firmware(
        part_fw, device2.ops(), thermostat::kBuildId, seed, rwd::kFlagRecordWrites, iters - 1500, 1500);
    check(part.writer_ok, "the second stretch was recorded, with its memory image");
    check(part.trace.size() < rec.trace.size(), "the trace is shorter for it");
    thermostat::Firmware part_replay;
    const rwhost::ReplayResult prep = rwhost::replay_firmware(part_replay, part.trace, iters - 1500);
    check(!prep.diverged, "it replays from its image");
    check(prep.fw.state_hash == rec.state_hash, "to the state the whole run ended in");

    std::printf("\n%s\n", g_failed == 0 ? "all steps held" : "SOMETHING DID NOT HOLD");
    return g_failed == 0 ? 0 : 1;
}
