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
        case rwd::EV_STOP:       return "STOP";
        default:                    return "?";
    }
}

} // namespace

ReplayHal::ReplayHal()
    : cursor_(0), now_(0), check_writes_(false) {
    ops_.read32   = &ReplayHal::s_read32;
    ops_.write32  = &ReplayHal::s_write32;
    ops_.now      = &ReplayHal::s_now;
    ops_.poll_irq = &ReplayHal::s_poll_irq;
    ops_.ctx      = this;

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

void ReplayHal::diverge(const char* fmt, ...) {
    if (!divergence_.empty()) {
        return;   // keep the first one; later ones are consequences
    }
    std::va_list args;
    va_start(args, fmt);
    std::vsnprintf(detail_, sizeof(detail_), fmt, args);
    va_end(args);

    divergence_ = detail_;
    rwd::hal_set_divergence(detail_);
}

u32 ReplayHal::read32(u32 addr) {
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
    now_ = ev.ts;
    return ev.value;
}

void ReplayHal::write32(u32 addr, u32 value) {
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
    now_ = ev.ts;
}

u32 ReplayHal::poll_irq() {
    if (cursor_ >= events_.size()) {
        return rwd::kNoIrq;
    }
    const rwd::Event& ev = events_[cursor_];
    if (ev.type != rwd::EV_IRQ_ENTER) {
        return rwd::kNoIrq;
    }

    ++cursor_;
    now_ = ev.ts;
    return ev.addr;   // vector number
}

u32  ReplayHal::s_read32(void* ctx, u32 addr) { return static_cast<ReplayHal*>(ctx)->read32(addr); }
void ReplayHal::s_write32(void* ctx, u32 a, u32 v) { static_cast<ReplayHal*>(ctx)->write32(a, v); }
u64  ReplayHal::s_now(void* ctx) { return static_cast<ReplayHal*>(ctx)->now_; }
u32  ReplayHal::s_poll_irq(void* ctx) { return static_cast<ReplayHal*>(ctx)->poll_irq(); }

} // namespace rwhost
