// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/replay.h"

#include <cstdarg>
#include <cstdio>

namespace rwhost {

bool VectorSink::write(void* ctx, const u8* data, u32 len) {
    VectorSink* self = static_cast<VectorSink*>(ctx);
    self->bytes.insert(self->bytes.end(), data, data + len);
    return true;
}

namespace {

const char* event_name(u8 type) {
    switch (type) {
        case rwd::EV_MMIO_READ:  return "MMIO_READ";
        case rwd::EV_MMIO_WRITE: return "MMIO_WRITE";
        case rwd::EV_IRQ_ENTER:  return "IRQ_ENTER";
        case rwd::EV_IRQ_AT:     return "IRQ_AT";
        case rwd::EV_SITE:       return "SITE";
        case rwd::EV_DMA:        return "DMA";
        case rwd::EV_STOP:       return "STOP";
        default:                    return "?";
    }
}

} // namespace

ReplayHal::ReplayHal()
    : cursor_(0), now_(0), divergence_cursor_(0), sites_recorded_(true),
      probe_count_(0),
      capture_at_(0), capture_fn_(0), capture_ctx_(0),
      captured_(false), events_served_(0), check_writes_(false) {
    ops_.read32   = &ReplayHal::s_read32;
    ops_.write32  = &ReplayHal::s_write32;
    ops_.now      = &ReplayHal::s_now;
    ops_.poll_irq = &ReplayHal::s_poll_irq;
    ops_.ctx      = this;
    ops_.site     = &ReplayHal::s_site;
    ops_.dma_read = &ReplayHal::s_dma_read;
    ops_.poll_irq_pri = &ReplayHal::s_poll_irq_pri;

    header_.magic    = 0;
    header_.version  = 0;
    header_.flags    = 0;
    header_.build_id = 0;
    header_.seed     = 0;
    detail_[0] = '\0';
}

bool ReplayHal::load(const u8* data, std::size_t len) {
    if (len > 0xFFFFFFFFu) {
        error_ = "trace larger than 4 GiB";
        return false;
    }

    rwd::TraceReader reader;
    if (!reader.open(data, static_cast<u32>(len))) {
        error_ = reader.error();
        return false;
    }

    header_       = reader.header();
    check_writes_ = (header_.flags & rwd::kFlagRecordWrites) != 0;
    sites_recorded_ = (header_.flags & rwd::kFlagNoSites) == 0;

    events_.clear();
    rwd::Event ev;
    while (reader.next(&ev)) {
        events_.push_back(ev);
    }
    if (!reader.ok()) {
        error_ = reader.error();
        return false;
    }

    cursor_ = 0;
    now_    = 0;
    return true;
}

void ReplayHal::set_capture_point(std::size_t index, CaptureFn fn, void* ctx) {
    capture_at_  = index;
    capture_fn_  = fn;
    capture_ctx_ = ctx;
    captured_    = false;
}

void ReplayHal::clear_capture_point() {
    capture_fn_  = 0;
    capture_ctx_ = 0;
    captured_    = false;
}

void ReplayHal::set_landing(std::size_t event, u32 probes) {
    landings_[event] = probes;
}

void ReplayHal::clear_landings() {
    landings_.clear();
}

void ReplayHal::restart() {
    cursor_      = 0;
    now_         = 0;
    probe_count_ = 0;
    captured_    = false;
    divergence_cursor_ = 0;
    divergence_.clear();
    detail_[0] = '\0';
}

void ReplayHal::set_cursor(std::size_t cursor, u64 now) {
    cursor_      = cursor;
    now_         = now;
    probe_count_ = 0;
    divergence_cursor_ = 0;
    divergence_.clear();
    detail_[0] = '\0';
}

// Fires at the first point the cursor sits on the chosen event, whichever
// access asks for it -- a read, a write or an interrupt poll. There is
// exactly one such first point, so the capture is deterministic.
void ReplayHal::maybe_capture() {
    if (capture_fn_ != 0 && !captured_ && cursor_ == capture_at_) {
        captured_ = true;
        capture_fn_(capture_ctx_);
    }
}

void ReplayHal::diverge(const char* fmt, ...) {
    if (!divergence_.empty()) {
        return;   // keep the first one; later ones are consequences
    }
    std::va_list args;
    va_start(args, fmt);
    std::vsnprintf(detail_, sizeof(detail_), fmt, args);
    va_end(args);

    divergence_        = detail_;
    divergence_cursor_ = cursor_;
    rwd::hal_set_divergence(detail_);
}

u32 ReplayHal::read32(u32 addr) {
    maybe_capture();

    if (cursor_ >= events_.size()) {
        diverge("read of 0x%08X past the end of the recording "
                "(%zu events consumed)", addr, cursor_);
        return 0;
    }

    const rwd::Event& ev = events_[cursor_];
    if (ev.type == rwd::EV_STOP) {
        // The recorded run had already stopped here. Almost always a
        // mismatched iteration count rather than a genuine behaviour change.
        diverge("read of 0x%08X past the end of the recording "
                "(the recorded run stopped after %zu events)", addr, cursor_);
        return 0;
    }
    if (ev.type != rwd::EV_MMIO_READ) {
        diverge("event %zu: firmware read 0x%08X, trace has %s",
                cursor_, addr, event_name(ev.type));
        return 0;
    }
    if (ev.addr != addr) {
        diverge("event %zu: firmware read 0x%08X, trace recorded 0x%08X",
                cursor_, addr, ev.addr);
        return 0;
    }

    ++cursor_;
    ++events_served_;
    now_ = ev.ts;
    probe_count_ = 0;   // a crossing: candidate positions count from here
    return ev.value;
}

void ReplayHal::write32(u32 addr, u32 value) {
    maybe_capture();

    if (!check_writes_) {
        // Writes were not recorded, so there is nothing to check against.
        // The replay is still correct -- writes are output, not input -- it
        // just loses this consistency check.
        return;
    }
    if (cursor_ >= events_.size()) {
        diverge("write of 0x%08X to 0x%08X past the end of the recording",
                value, addr);
        return;
    }

    const rwd::Event& ev = events_[cursor_];
    if (ev.type == rwd::EV_STOP) {
        diverge("write of 0x%08X to 0x%08X past the end of the recording "
                "(the recorded run stopped after %zu events)",
                value, addr, cursor_);
        return;
    }
    if (ev.type != rwd::EV_MMIO_WRITE) {
        diverge("event %zu: firmware wrote 0x%08X, trace has %s",
                cursor_, addr, event_name(ev.type));
        return;
    }
    if (ev.addr != addr || ev.value != value) {
        diverge("event %zu: firmware wrote 0x%08X=0x%08X, "
                "trace recorded 0x%08X=0x%08X",
                cursor_, addr, value, ev.addr, ev.value);
        return;
    }

    ++cursor_;
    ++events_served_;
    now_ = ev.ts;
    probe_count_ = 0;   // a crossing: candidate positions count from here
}

u32 ReplayHal::poll_irq() {
    maybe_capture();

    if (cursor_ >= events_.size()) {
        return rwd::kNoIrq;
    }
    const rwd::Event& ev = events_[cursor_];
    // An interrupt recorded with its return address is served exactly like
    // one without: after the access that precedes it in the trace. The
    // address is carried for the dump; stopping on it is the next step.
    if (ev.type != rwd::EV_IRQ_ENTER && ev.type != rwd::EV_IRQ_AT) {
        return rwd::kNoIrq;
    }
    // Told to hold this one back until a number of probe points have gone by
    // since the last crossing? Not yet. (>= rather than ==: if the planned
    // probe went by while interrupts were masked, it is delivered as soon as
    // they are not.)
    const std::map<std::size_t, u32>::const_iterator plan = landings_.find(cursor_);
    if (plan != landings_.end() && probe_count_ < plan->second) {
        return rwd::kNoIrq;
    }

    ++cursor_;
    ++events_served_;
    now_ = ev.ts;
    probe_count_ = 0;   // a crossing: candidate positions count from here
    return ev.addr;   // vector number
}

// poll_irq for a firmware whose interrupts have priorities. The trace says
// which interrupt came next and the shim says how urgent the running handler
// is; if the next one is not more urgent, it did not preempt this handler in
// the recording and will not now -- it comes after the handler returns, when
// the shim asks again from outside it. Nothing is wrong with that, so it is not
// a divergence; if the firmware really is not the recorded one, the accesses
// that follow will say so.
//
// Asked from inside a handler this is not a capture point: poll_irq was never
// asked there before priorities, and moving the instant a capture is taken to
// a place it never was would change what a snapshot at an event means.
u32 ReplayHal::poll_irq_pri(const u8* priority, u32 level) {
    if (level == rwd::kThreadLevel) {
        return poll_irq();
    }
    if (cursor_ >= events_.size()) {
        return rwd::kNoIrq;
    }
    const rwd::Event& ev = events_[cursor_];
    if (ev.type != rwd::EV_IRQ_ENTER && ev.type != rwd::EV_IRQ_AT) {
        return rwd::kNoIrq;
    }
    const u32 urgency = ev.addr < rwd::kMaxIrqVectors ? (u32)priority[ev.addr] : 0u;
    if (urgency >= level) {
        return rwd::kNoIrq;
    }
    return poll_irq();
}

// A probe point is one more crossing, served in trace order like an access:
// the recording crossed this label here, so the firmware must too. Anything
// else -- no site in the trace, a different label, the trace already over --
// means this is not the run that was recorded.
void ReplayHal::site(u32 id) {
    if (!sites_recorded_) {
        // The recorder did not keep its sites, so the trace has no event for
        // this one. It is not a crossing; it is a place a held-back interrupt
        // may be delivered, and the shim will ask for it right after this.
        ++probe_count_;
        return;
    }

    maybe_capture();

    if (cursor_ >= events_.size()) {
        diverge("site %u past the end of the recording "
                "(%zu events consumed)", id, cursor_);
        return;
    }

    const rwd::Event& ev = events_[cursor_];
    if (ev.type == rwd::EV_STOP) {
        diverge("site %u past the end of the recording "
                "(the recorded run stopped after %zu events)", id, cursor_);
        return;
    }
    if (ev.type != rwd::EV_SITE) {
        diverge("event %zu: firmware crossed site %u, trace has %s",
                cursor_, id, event_name(ev.type));
        return;
    }
    if (ev.addr != id) {
        diverge("event %zu: firmware crossed site %u, trace recorded site %u",
                cursor_, id, ev.addr);
        return;
    }

    ++cursor_;
    ++events_served_;
    now_ = ev.ts;
    probe_count_ = 0;   // a crossing: candidate positions count from here
}

// What the firmware saw when it looked at a DMA buffer. Served in trace order
// like an access, and checked like one: the same region, offset and length
// must be asked for, or this is not the run that was recorded.
void ReplayHal::dma_read(u32 region, u32 offset, u8* dst, u32 len) {
    maybe_capture();

    for (u32 i = 0; i < len; ++i) {
        dst[i] = 0;
    }

    if (cursor_ >= events_.size()) {
        diverge("DMA read of region %u past the end of the recording "
                "(%zu events consumed)", region, cursor_);
        return;
    }

    const rwd::Event& ev = events_[cursor_];
    if (ev.type == rwd::EV_STOP) {
        diverge("DMA read of region %u past the end of the recording "
                "(the recorded run stopped after %zu events)", region, cursor_);
        return;
    }
    if (ev.type != rwd::EV_DMA) {
        diverge("event %zu: firmware read %u bytes of DMA region %u, trace has %s",
                cursor_, len, region, event_name(ev.type));
        return;
    }
    if (ev.addr != region || ev.value != offset || ev.len != len) {
        diverge("event %zu: firmware read %u bytes of DMA region %u at %u, "
                "trace recorded %u bytes of region %u at %u",
                cursor_, len, region, offset, (unsigned)ev.len, ev.addr, ev.value);
        return;
    }

    for (u32 i = 0; i < len; ++i) {
        dst[i] = ev.data[i];
    }
    ++cursor_;
    ++events_served_;
    now_         = ev.ts;
    probe_count_ = 0;
}

u32  ReplayHal::s_read32(void* ctx, u32 addr) { return static_cast<ReplayHal*>(ctx)->read32(addr); }
void ReplayHal::s_write32(void* ctx, u32 a, u32 v) { static_cast<ReplayHal*>(ctx)->write32(a, v); }
u64  ReplayHal::s_now(void* ctx) { return static_cast<ReplayHal*>(ctx)->now_; }
u32  ReplayHal::s_poll_irq(void* ctx) { return static_cast<ReplayHal*>(ctx)->poll_irq(); }
u32  ReplayHal::s_poll_irq_pri(void* ctx, const u8* priority, u32 level) {
    return static_cast<ReplayHal*>(ctx)->poll_irq_pri(priority, level);
}
void ReplayHal::s_site(void* ctx, u32 id) { static_cast<ReplayHal*>(ctx)->site(id); }
void ReplayHal::s_dma_read(void* ctx, u32 region, u32 offset, u8* dst, u32 len) {
    static_cast<ReplayHal*>(ctx)->dma_read(region, offset, dst, len);
}

} // namespace rwhost
