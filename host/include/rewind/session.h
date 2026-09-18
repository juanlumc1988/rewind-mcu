// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Record and replay a run of the example firmware. Shared by the CLI and the
// tests so both exercise the same path.
//
// None of this is re-entrant or thread-safe, and that is not an oversight:
// the shim and the firmware keep their state in static storage because that
// is what a microcontroller does. One MCU, one HAL, one run at a time.

#ifndef REWIND_SESSION_H
#define REWIND_SESSION_H

#include <cstddef>
#include <string>
#include <vector>

#include "fw/rx_pump.h"
#include "rewind/replay.h"

namespace rwhost {

// Identifies the firmware build that produced a trace. Replaying a trace
// against a different build is the fastest way to spend an afternoon chasing
// a bug that is not there, so the header carries this and replay_run()
// refuses a mismatch.
const u64 kBuildIdBuggy = 0xBADC0DE000000001ULL;
const u64 kBuildIdFixed = 0x600DC0DE00000001ULL;

struct RunConfig {
    u64          seed       = 1;
    u32          tx_total   = 24;
    u32          main_iters = 6000;
    fw::Variant  variant    = fw::kBuggy;
    u16          flags      = rwd::kFlagRecordWrites;
};

// What the firmware ended up believing. Comparing this across a record and
// its replay is the invariant the whole project rests on.
struct FirmwareOutcome {
    u64 state_hash = 0;
    u32 consumed   = 0;
    u32 checksum   = 0;
    u32 overflows  = 0;
};

struct RecordResult {
    std::vector<u8> trace;
    FirmwareOutcome fw;

    u32 tx_sent     = 0;   // bytes the UART actually delivered
    u32 tx_checksum = 0;   // checksum over those bytes
    u64 cycles      = 0;
    u32 events      = 0;

    bool        writer_ok = false;
    std::string writer_error;

    // The firmware fell behind: it did not consume everything that arrived,
    // or consumed it out of order. This is the bug, observed from outside.
    bool bug_triggered() const {
        return fw.consumed != tx_sent || fw.checksum != tx_checksum;
    }
};

struct ReplayResult {
    FirmwareOutcome fw;

    bool        loaded = false;
    std::string error;

    bool        diverged = false;
    std::string divergence;

    std::size_t events_consumed = 0;
    std::size_t events_total    = 0;
};

u64  build_id_for(fw::Variant variant);
bool variant_for_build_id(u64 build_id, fw::Variant* out);

RecordResult record_run(const RunConfig& cfg);

// Replays a trace against the firmware build that produced it, as named by
// the trace header.
//
// `main_iters` must match the recording. In a real deployment it would not
// be a parameter at all -- it is a constant compiled into the binary, and
// the binary is pinned by build_id. It is a parameter here only so the tests
// can show what happens when the two disagree.
ReplayResult replay_run(const std::vector<u8>& trace, u32 main_iters);

// Replays against a caller-chosen variant, skipping the build_id check.
// Exists to demonstrate what replaying the wrong binary looks like.
ReplayResult replay_run_as(const std::vector<u8>& trace, fw::Variant variant,
                           u32 main_iters);

} // namespace rwhost

#endif // REWIND_SESSION_H
