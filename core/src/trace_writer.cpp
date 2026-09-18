// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/trace_writer.h"
#include "rewind/crc32.h"

namespace rwd {

TraceWriter::TraceWriter()
    : sink_(0), ctx_(0), buf_(0), cap_(0), used_(0), last_ts_(0),
      last_addr_(0), events_(0), bytes_(0), flags_(0), ok_(false),
      started_(false), finished_(false), err_(0) {}

void TraceWriter::fail(const char* why) {
    if (!err_) {
        err_ = why;
    }
    ok_ = false;
}

bool TraceWriter::emit(const u8* data, u32 len) {
    if (!sink_(ctx_, data, len)) {
        fail("sink rejected data");
        return false;
    }
    bytes_ += (u64)len;
    return true;
}

bool TraceWriter::begin(SinkFn sink, void* ctx, u8* buffer, u32 buf_len,
                        u64 build_id, u64 seed, u16 flags) {
    if (started_) {
        fail("begin() called twice");
        return false;
    }
    if (sink == 0 || buffer == 0) {
        fail("null sink or buffer");
        return false;
    }
    if (buf_len < kMinBlockBuffer) {
        fail("block buffer smaller than kMinBlockBuffer");
        return false;
    }

    sink_    = sink;
    ctx_     = ctx;
    buf_     = buffer;
    cap_     = buf_len;
    used_      = 0;
    last_ts_   = 0;
    last_addr_ = 0;
    events_    = 0;
    bytes_   = 0;
    flags_   = flags;
    ok_      = true;
    started_ = true;

    u8 header[kTraceHeaderSize];
    for (u32 i = 0; i < kTraceHeaderSize; ++i) {
        header[i] = 0;
    }
    put_u32_le(header + 0, kTraceMagic);
    put_u16_le(header + 4, kTraceVersion);
    put_u16_le(header + 6, flags);
    put_u64_le(header + 8, build_id);
    put_u64_le(header + 16, seed);
    // header + 24 stays zero (reserved).

    return emit(header, kTraceHeaderSize);
}

bool TraceWriter::flush_block() {
    if (used_ == 0) {
        // Never emit an empty block: payload_len == 0 is the terminator.
        return ok_;
    }

    u8 frame[4];
    put_u32_le(frame, used_);
    if (!emit(frame, 4)) {
        return false;
    }
    if (!emit(buf_, used_)) {
        return false;
    }
    put_u32_le(frame, crc32(buf_, used_));
    if (!emit(frame, 4)) {
        return false;
    }

    used_ = 0;
    return true;
}

// Copies a staged event into the block buffer, flushing first if it will not
// fit. cap_ >= kMinBlockBuffer >= kMaxEventBytes guarantees it fits after.
bool TraceWriter::stage(const u8* bytes, u32 n) {
    if (used_ + n > cap_) {
        if (!flush_block()) {
            return false;
        }
    }
    for (u32 i = 0; i < n; ++i) {
        buf_[used_ + i] = bytes[i];
    }
    used_ += n;
    ++events_;
    return true;
}

bool TraceWriter::write_mmio(u8 type, u64 ts, u32 addr, u32 value) {
    if (!ok_ || !started_ || finished_) {
        return false;
    }
    if (ts < last_ts_) {
        // A timestamp that moves backwards means the clock source wrapped
        // without being extended, which would corrupt every delta after it.
        fail("non-monotonic timestamp");
        return false;
    }

    // Widen to i64 before subtracting: the difference between two u32
    // addresses does not fit in u32 when it is negative, and the whole point
    // here is that it goes both ways.
    const i64 delta_addr = (i64)addr - (i64)last_addr_;

    u8  scratch[kMaxEventBytes];
    u32 n = 0;
    scratch[n++] = type;
    n += varint_encode(ts - last_ts_, scratch + n);
    n += varint_encode(zigzag_encode(delta_addr), scratch + n);
    n += varint_encode((u64)value, scratch + n);

    if (!stage(scratch, n)) {
        return false;
    }
    last_ts_   = ts;
    last_addr_ = addr;
    return true;
}

bool TraceWriter::write_plain(u8 type, u64 ts, const u32* arg) {
    if (!ok_ || !started_ || finished_) {
        return false;
    }
    if (ts < last_ts_) {
        fail("non-monotonic timestamp");
        return false;
    }

    u8  scratch[kMaxEventBytes];
    u32 n = 0;
    scratch[n++] = type;
    n += varint_encode(ts - last_ts_, scratch + n);
    if (arg != 0) {
        n += varint_encode((u64)*arg, scratch + n);
    }

    if (!stage(scratch, n)) {
        return false;
    }
    last_ts_ = ts;
    // last_addr_ is deliberately untouched: an interrupt is not a peripheral
    // access, and letting a vector number reset the address predictor would
    // cost a long delta on the very next MMIO event.
    return true;
}

bool TraceWriter::mmio_read(u64 ts, u32 addr, u32 value) {
    return write_mmio((u8)EV_MMIO_READ, ts, addr, value);
}

bool TraceWriter::mmio_write(u64 ts, u32 addr, u32 value) {
    if ((flags_ & kFlagRecordWrites) == 0) {
        return ok_;
    }
    return write_mmio((u8)EV_MMIO_WRITE, ts, addr, value);
}

bool TraceWriter::irq_enter(u64 ts, u32 vector) {
    return write_plain((u8)EV_IRQ_ENTER, ts, &vector);
}

bool TraceWriter::finish(u64 ts) {
    if (!started_ || finished_) {
        return false;
    }
    if (ok_) {
        write_plain((u8)EV_STOP, ts, 0);
    }
    if (ok_) {
        flush_block();
    }
    if (ok_) {
        u8 terminator[4];
        put_u32_le(terminator, 0);
        emit(terminator, 4);
    }
    finished_ = true;
    return ok_;
}

} // namespace rwd
