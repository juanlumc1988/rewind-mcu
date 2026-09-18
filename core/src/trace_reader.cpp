// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/trace_reader.h"
#include "rewind/crc32.h"

namespace rwd {

TraceReader::TraceReader()
    : data_(0), len_(0), block_end_(0), pos_(0), next_block_off_(0),
      last_ts_(0), last_addr_(0), expect_seq_(0), blocks_read_(0),
      blocks_lost_(0), opened_(false), done_(false), err_(0) {
    hdr_.magic    = 0;
    hdr_.version  = 0;
    hdr_.flags    = 0;
    hdr_.build_id = 0;
    hdr_.seed     = 0;
}

bool TraceReader::fail(const char* why) {
    if (!err_) {
        err_ = why;
    }
    return false;
}

bool TraceReader::open(const u8* data, u32 len) {
    // Drop every trace of a previous trace before validating this one, so a
    // failed open leaves the reader unusable rather than still pointing at
    // the last one with its counters intact. Resetting only part of it is
    // worse than resetting none: it looks clean and is not.
    data_           = 0;
    len_            = 0;
    block_end_      = 0;
    pos_            = 0;
    next_block_off_ = 0;
    last_ts_        = 0;
    last_addr_      = 0;
    expect_seq_     = 0;
    blocks_read_    = 0;
    blocks_lost_    = 0;
    opened_         = false;
    done_           = false;
    err_            = 0;
    hdr_.magic      = 0;
    hdr_.version    = 0;
    hdr_.flags      = 0;
    hdr_.build_id   = 0;
    hdr_.seed       = 0;

    if (data == 0) {
        return fail("null trace buffer");
    }
    if (len < kTraceHeaderSize) {
        return fail("trace shorter than its header");
    }

    hdr_.magic = get_u32_le(data + 0);
    if (hdr_.magic != kTraceMagic) {
        return fail("bad magic: not a rewind trace");
    }
    hdr_.version = get_u16_le(data + 4);
    if (hdr_.version != kTraceVersion) {
        return fail("unsupported trace version");
    }
    hdr_.flags    = get_u16_le(data + 6);
    hdr_.build_id = get_u64_le(data + 8);
    hdr_.seed     = get_u64_le(data + 16);

    data_           = data;
    len_            = len;
    next_block_off_ = kTraceHeaderSize;
    opened_         = true;
    return true;
}

bool TraceReader::load_next_block() {
    // Written as a subtraction rather than next_block_off_ + 4 > len_, which
    // wraps when the offset is near 2^32. len_ >= kTraceHeaderSize is
    // guaranteed by open(), so len_ - 4 cannot underflow.
    if (next_block_off_ > len_ - 4u) {
        // No room for a length word: the trace was cut short. Everything
        // handed out so far was CRC-checked, so report end rather than error.
        done_ = true;
        return false;
    }

    const u32 payload_len = get_u32_le(data_ + next_block_off_);
    if (payload_len == 0) {
        done_ = true;
        return false;
    }

    // Each step is checked against the remaining length rather than summed,
    // so a hostile payload_len cannot wrap the arithmetic into a pass.
    u32 remaining = len_ - next_block_off_ - 4;
    if (remaining < 4) {
        done_ = true;
        return fail("truncated block: trace ends mid-block");
    }
    const u32 seq_off = next_block_off_ + 4;

    remaining -= 4;
    if (payload_len > remaining || remaining - payload_len < 4) {
        done_ = true;
        return fail("truncated block: trace ends mid-block");
    }
    const u32 payload_off = seq_off + 4;

    u32 crc = crc32_update(crc32_init(), data_ + seq_off, 4);
    crc     = crc32_update(crc, data_ + payload_off, payload_len);
    const u32 want = get_u32_le(data_ + payload_off + payload_len);
    if (want != crc32_final(crc)) {
        done_ = true;
        return fail("block CRC mismatch: trace is corrupt");
    }

    const u32 seq = get_u32_le(data_ + seq_off);
    if (seq != expect_seq_) {
        // Sequence numbers only ever move forwards. Going backwards means
        // two traces were spliced together, or blocks arrived out of order
        // on a transport that promised it would not do that.
        if (seq < expect_seq_) {
            done_ = true;
            return fail("block sequence went backwards: trace is not a "
                        "single recording");
        }
        // Forwards: the device dropped blocks it could not drain. Every
        // event after this point belongs to a different moment than the
        // reader would otherwise assume, and timestamp and address deltas
        // are both relative -- so continuing would not merely skip data, it
        // would decode the remainder wrongly.
        blocks_lost_ = seq - expect_seq_;
        done_        = true;
        return fail("gap in trace: the device dropped blocks it could not "
                    "drain in time");
    }

    expect_seq_ = seq + 1u;
    ++blocks_read_;

    pos_            = payload_off;
    block_end_      = payload_off + payload_len;
    next_block_off_ = block_end_ + 4;
    return true;
}

bool TraceReader::next(Event* out) {
    if (!opened_) {
        return fail("next() before open()");
    }
    if (done_ || err_ != 0 || out == 0) {
        return false;
    }

    if (pos_ >= block_end_) {
        if (!load_next_block()) {
            return false;
        }
    }

    const u32 avail = block_end_ - pos_;
    const u8  type  = data_[pos_];

    bool is_mmio = false;
    bool has_arg = false;
    switch (type) {
        case EV_MMIO_READ:
        case EV_MMIO_WRITE: is_mmio = true;  has_arg = true;  break;
        case EV_IRQ_ENTER:  is_mmio = false; has_arg = true;  break;
        case EV_STOP:       is_mmio = false; has_arg = false; break;
        default:
            done_ = true;
            return fail("unknown event type");
    }

    u32 off = 1;
    u64 delta_ts = 0;
    u32 n = varint_decode(data_ + pos_ + off, avail - off, &delta_ts);
    if (n == 0) {
        done_ = true;
        return fail("malformed timestamp varint");
    }
    off += n;

    u32 addr  = 0;
    u32 value = 0;

    if (has_arg) {
        u64 raw = 0;
        n = varint_decode(data_ + pos_ + off, avail - off, &raw);
        if (n == 0) {
            done_ = true;
            return fail("malformed argument varint");
        }
        off += n;

        if (is_mmio) {
            // Range-check the delta BEFORE adding it. A corrupt or hostile
            // trace can encode a delta near INT64_MIN, and
            // (i64)last_addr_ + that is signed overflow -- undefined
            // behaviour in a routine whose whole job is parsing input it
            // does not trust. Both bounds below are computed from
            // last_addr_, which is a u32, so neither can overflow.
            const i64 delta = zigzag_decode(raw);
            const i64 lower = -(i64)last_addr_;
            const i64 upper = (i64)(0xFFFFFFFFu - last_addr_);
            if (delta < lower || delta > upper) {
                done_ = true;
                return fail("address delta lands outside the 32-bit space");
            }
            addr       = (u32)((i64)last_addr_ + delta);
            last_addr_ = addr;
        } else {
            if (raw > 0xFFFFFFFFu) {
                done_ = true;
                return fail("argument does not fit in 32 bits");
            }
            addr = (u32)raw;   // vector number
        }
    }

    if (is_mmio) {
        u64 raw = 0;
        n = varint_decode(data_ + pos_ + off, avail - off, &raw);
        if (n == 0) {
            done_ = true;
            return fail("malformed value varint");
        }
        if (raw > 0xFFFFFFFFu) {
            done_ = true;
            return fail("value does not fit in 32 bits");
        }
        off += n;
        value = (u32)raw;
    }

    last_ts_ += delta_ts;

    out->type  = type;
    out->ts    = last_ts_;
    out->addr  = addr;
    out->value = value;

    pos_ += off;
    return true;
}

} // namespace rwd
