// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/session.h"

#include "sim/mcu.h"

namespace rwhost {
namespace {

// Matches the block size a modest target would dedicate to a trace ring
// buffer. Nothing depends on it -- change it and the traces stay readable --
// but keeping it small here means the tests exercise block boundaries
// thousands of times per run instead of never.
const u32 kBlockBufferBytes = 256;

FirmwareOutcome snapshot() {
    const fw::State& s = fw::rx_pump_state();
    FirmwareOutcome out;
    out.state_hash = fw::rx_pump_state_hash();
    out.consumed   = s.consumed;
    out.checksum   = s.checksum;
    out.overflows  = s.overflows;
    return out;
}

} // namespace

u64 build_id_for(fw::Variant variant) {
    return (variant == fw::kFixed) ? kBuildIdFixed : kBuildIdBuggy;
}

bool variant_for_build_id(u64 build_id, fw::Variant* out) {
    if (build_id == kBuildIdBuggy) {
        *out = fw::kBuggy;
        return true;
    }
    if (build_id == kBuildIdFixed) {
        *out = fw::kFixed;
        return true;
    }
    return false;
}

RecordResult record_run(const RunConfig& cfg) {
    RecordResult result;

    sim::Mcu            mcu(cfg.seed, cfg.tx_total);
    VectorSink          sink;
    rwd::TraceWriter writer;
    u8                  block[kBlockBufferBytes];

    rwd::hal_reset();
    fw::rx_pump_reset();
    fw::rx_pump_install();
    rwd::hal_attach(mcu.ops());

    if (!writer.begin(&VectorSink::write, &sink, block, sizeof(block),
                      build_id_for(cfg.variant), cfg.seed, cfg.flags)) {
        result.writer_error = writer.error();
        rwd::hal_reset();
        return result;
    }

    rwd::hal_mode_record(&writer);
    fw::rx_pump_run(cfg.variant, cfg.main_iters);
    writer.finish(rwd::hal_now());
    rwd::hal_mode_off();

    result.fw           = snapshot();
    result.trace        = sink.bytes;
    result.tx_sent      = mcu.tx_sent();
    result.tx_checksum  = mcu.tx_checksum();
    result.cycles       = mcu.cycles();
    result.events       = writer.events();
    result.writer_ok    = writer.ok();
    result.writer_error = writer.error();

    // Detach before `mcu` goes out of scope: the shim holds a pointer to it.
    rwd::hal_reset();
    return result;
}

ReplayResult replay_run_as(const std::vector<u8>& trace, fw::Variant variant,
                           u32 main_iters) {
    ReplayResult result;

    ReplayHal hal;
    if (!hal.load(trace.data(), trace.size())) {
        result.error = hal.error();
        return result;
    }
    result.loaded       = true;
    result.events_total = hal.event_count();

    rwd::hal_reset();
    fw::rx_pump_reset();
    fw::rx_pump_install();
    rwd::hal_attach(hal.ops());
    rwd::hal_mode_replay();

    fw::rx_pump_run(variant, main_iters);

    result.fw              = snapshot();
    result.diverged        = hal.diverged() || rwd::hal_diverged();
    result.divergence      = hal.divergence();
    result.events_consumed = hal.cursor();

    // Read the divergence out before hal_reset() clears the pointer the shim
    // holds into `hal`.
    rwd::hal_reset();
    return result;
}

ReplayResult replay_run(const std::vector<u8>& trace, u32 main_iters) {
    ReplayResult probe;

    ReplayHal hal;
    if (!hal.load(trace.data(), trace.size())) {
        probe.error = hal.error();
        return probe;
    }

    fw::Variant variant;
    if (!variant_for_build_id(hal.header().build_id, &variant)) {
        probe.error = "trace was recorded by an unknown firmware build";
        return probe;
    }

    return replay_run_as(trace, variant, main_iters);
}

} // namespace rwhost
