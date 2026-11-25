// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/session.h"

#include "rewind/example_firmware.h"
#include "rewind/recorder.h"
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

namespace {

// Stands in for a UART: accepts at most `per_call` bytes at a time, the way
// a hardware FIFO would.
struct Link {
    std::vector<u8> received;
    u32             per_call;

    Link() : per_call(0xFFFFFFFFu) {}

    static u32 write(void* ctx, const u8* data, u32 len) {
        Link*     self  = static_cast<Link*>(ctx);
        const u32 taken = (len < self->per_call) ? len : self->per_call;
        self->received.insert(self->received.end(), data, data + taken);
        return taken;
    }
};

} // namespace

BufferedResult record_buffered(const BufferedConfig& cfg) {
    BufferedResult result;

    sim::Mcu        mcu(cfg.run.seed, cfg.run.tx_total);
    if (cfg.run.variant == fw::kDma) {
        mcu.enable_dma();
    }
    rwd::Recorder   recorder;
    std::vector<u8> ring(cfg.ring_capacity);
    std::vector<u8> block(kBlockBufferBytes);
    Link            link;
    link.per_call = cfg.link_per_call;

    rwd::hal_reset();
    fw::rx_pump_reset();
    fw::rx_pump_install();
    rwd::hal_attach(mcu.ops());

    if (!recorder.begin(ring.data(), cfg.ring_capacity, block.data(),
                        kBlockBufferBytes, build_id_for(cfg.run.variant),
                        cfg.run.seed, cfg.run.flags)) {
        rwd::hal_reset();
        return result;
    }
    result.began = true;
    recorder.start();

    // Execution interleaved with draining, as a main loop would do it.
    const u32 slices    = cfg.slices ? cfg.slices : 1u;
    const u32 per_slice = cfg.run.main_iters / slices;
    for (u32 slice = 0; slice < slices; ++slice) {
        fw::rx_pump_run(cfg.run.variant, per_slice);
        recorder.drain(&Link::write, &link, cfg.drain_budget);
    }

    recorder.stop(rwd::hal_now());
    while (recorder.drain(&Link::write, &link, 0xFFFFu) > 0) {
    }

    result.trace       = link.received;
    result.fw          = snapshot();
    result.blocks_lost = recorder.blocks_lost();
    result.ring_drops  = recorder.ring().drops();
    result.bytes_lost  = recorder.ring().bytes_dropped();
    result.high_water  = recorder.ring().high_water();
    result.events      = recorder.writer().events();
    result.healthy     = recorder.healthy();
    result.tx_sent     = mcu.tx_sent();
    result.tx_checksum = mcu.tx_checksum();

    rwd::hal_reset();
    return result;
}

u64 build_id_for(fw::Variant variant) {
    switch (variant) {
        case fw::kFixed:     return kBuildIdFixed;
        case fw::kUnguarded: return kBuildIdUnguarded;
        case fw::kDma:       return kBuildIdDma;
        default:             return kBuildIdBuggy;
    }
}

bool variant_for_build_id(u64 build_id, fw::Variant* out) {
    if (build_id == kBuildIdBuggy) {
        *out = fw::kBuggy;
        return true;
    }
    if (build_id == kBuildIdDma) {
        *out = fw::kDma;
        return true;
    }
    if (build_id == kBuildIdUnguarded) {
        *out = fw::kUnguarded;
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
    if (cfg.variant == fw::kDma) {
        mcu.enable_dma();
    }
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

    // A recording that starts mid-run: the firmware has been running, with
    // nothing recorded, and from here on it is -- first its memory as it stands.
    if (cfg.skip_iters > 0) {
        fw::rx_pump_run(cfg.variant, cfg.skip_iters);
    }
    rwd::hal_mode_record(&writer);
    if (cfg.skip_iters > 0 && !rwd::hal_snapshot()) {
        result.writer_error = writer.ok() ? "the memory image could not be written"
                                          : writer.error();
        rwd::hal_reset();
        return result;
    }
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

ReplayResult replay_firmware(Firmware& firmware, const std::vector<u8>& trace,
                             u32 main_iters) {
    ReplayResult result;

    ReplayHal hal;
    if (!hal.load(trace.data(), trace.size())) {
        result.error = hal.error();
        return result;
    }
    result.loaded         = true;
    result.events_total   = hal.event_count();
    result.sites_recorded = hal.sites_recorded();

    rwd::hal_reset();
    std::string why;
    if (!start_firmware(firmware, hal, &why)) {
        result.loaded = false;
        result.error  = why;
        return result;
    }
    rwd::hal_attach(hal.ops());
    rwd::hal_mode_replay();

    firmware.run(main_iters);

    result.fw.state_hash   = firmware.state_hash();
    result.diverged        = hal.diverged() || rwd::hal_diverged();
    result.divergence      = hal.divergence();
    result.events_consumed = hal.cursor();

    // Read the divergence out before hal_reset() clears the pointer the shim
    // holds into `hal`.
    rwd::hal_reset();
    return result;
}

ReplayResult replay_run_as(const std::vector<u8>& trace, fw::Variant variant,
                           u32 main_iters) {
    ExampleFirmware example(variant);
    ReplayResult result = replay_firmware(example, trace, main_iters);
    if (result.loaded) {
        // The example's own outcome: the fields of its state that the tools
        // and the tests compare, which a generic replay has no business knowing.
        result.fw = snapshot();
    }
    return result;
}

FirmwareRecording record_firmware(Firmware& firmware, const rwd::HalOps* backend,
                                  u64 build_id, u64 seed, u16 flags, u32 main_iters,
                                  u32 skip_iters) {
    FirmwareRecording result;

    VectorSink          sink;
    rwd::TraceWriter writer;
    u8                  block[kBlockBufferBytes];

    rwd::hal_reset();
    firmware.reset();
    firmware.install();
    rwd::hal_attach(backend);

    if (!writer.begin(&VectorSink::write, &sink, block, sizeof(block), build_id, seed, flags)) {
        result.writer_error = writer.error();
        rwd::hal_reset();
        return result;
    }

    if (skip_iters > 0) {
        firmware.run(skip_iters);
    }
    rwd::hal_mode_record(&writer);
    if (skip_iters > 0 && !rwd::hal_snapshot()) {
        result.writer_error = writer.ok() ? "the memory image could not be written (did the "
                                            "firmware register a state region?)"
                                          : writer.error();
        rwd::hal_reset();
        return result;
    }
    firmware.run(main_iters);
    writer.finish(rwd::hal_now());
    rwd::hal_mode_off();

    result.trace        = sink.bytes;
    result.state_hash   = firmware.state_hash();
    result.events       = writer.events();
    result.writer_ok    = writer.ok();
    result.writer_error = writer.error();

    // Detach before the backend can go out of scope: the shim holds a pointer to it.
    rwd::hal_reset();
    return result;
}

namespace {

struct LocateCtx {
    Firmware* firmware;
    u32       main_iters;
};

// One replay of the firmware, for the search.
void run_firmware_once(void* ctx, ReplayHal* hal) {
    const LocateCtx* c = static_cast<const LocateCtx*>(ctx);
    rwd::hal_reset();
    // Cannot fail here: locate_firmware() has tried the same image already.
    start_firmware(*c->firmware, *hal, 0);
    rwd::hal_attach(hal->ops());
    rwd::hal_mode_replay();
    c->firmware->run(c->main_iters);
    rwd::hal_reset();
}

} // namespace

LocateReport locate_firmware(Firmware& firmware, const std::vector<u8>& trace,
                             u32 main_iters) {
    LocateReport report;

    ReplayHal probe;
    if (!probe.load(trace.data(), trace.size())) {
        report.error = probe.error();
        return report;
    }
    std::string why;
    rwd::hal_reset();
    if (!start_firmware(firmware, probe, &why)) {
        report.error = why;
        return report;
    }
    report.loaded       = true;
    report.events_total = probe.event_count();

    LocateCtx ctx;
    ctx.firmware   = &firmware;
    ctx.main_iters = main_iters;
    report.located = locate_landings(trace, &run_firmware_once, &ctx);
    // The search's last run is the located replay, so the firmware state is
    // its outcome.
    report.fw.state_hash = firmware.state_hash();
    return report;
}

LocateReport locate_run(const std::vector<u8>& trace, u32 main_iters) {
    ReplayHal probe;
    if (!probe.load(trace.data(), trace.size())) {
        LocateReport report;
        report.error = probe.error();
        return report;
    }
    fw::Variant variant;
    if (!variant_for_build_id(probe.header().build_id, &variant)) {
        LocateReport report;
        report.error = "trace was recorded by an unknown firmware build";
        return report;
    }

    ExampleFirmware example(variant);
    LocateReport report = locate_firmware(example, trace, main_iters);
    report.fw = snapshot();
    return report;
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
