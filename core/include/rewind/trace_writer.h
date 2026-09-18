// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Serialises events into blocks and hands each finished block to a sink.
//
// Two constraints shape this class, and both come from the target:
//   * No allocation. The caller owns the block buffer and passes it in, so
//     the writer's own footprint is a handful of scalars and it can live in
//     .bss on a part with no heap at all.
//   * No virtuals. The sink is a function pointer plus a context pointer
//     rather than an abstract base, which keeps a vtable out of flash and
//     makes the indirect call something a Cortex-M can inline-cache in its
//     own way. It also makes the thing trivial to fake in a unit test.

#ifndef REWIND_TRACE_WRITER_H
#define REWIND_TRACE_WRITER_H

#include "rewind/trace.h"

namespace rwd {

// Receives one complete, framed block -- or the header, or the terminator --
// in a single call, and must take all of it or none of it. Must not re-enter
// the writer.
//
// Returning false means this block did not make it. For the header that is
// fatal. For a block it is not: the writer counts it, advances the sequence
// number so the gap is visible to whoever reads the trace, and carries on
// with the next one. A device that briefly cannot keep up should lose the
// burst it could not buffer, not the rest of the recording.
typedef bool (*SinkFn)(void* ctx, const u8* data, u32 len);

// The block buffer holds the frame as well as the payload, so that a block
// reaches the sink in one piece:
//
//   [0..4)                payload_len
//   [4..8)                seq
//   [8 .. 8+payload)      events
//   [8+payload .. +4)     crc32
//
// Splitting it across several sink calls would let a full ring buffer accept
// a header and reject its payload, putting half a block on the wire --
// which passes its length check, fails its CRC, and looks exactly like line
// noise. All-or-nothing is the only safe shape.
//
// The minimum therefore covers the 12-byte frame plus one worst-case event.
const u32 kMinBlockBuffer = 64;

RW_STATIC_ASSERT(kMinBlockBuffer >= kBlockOverhead + kMaxEventBytes,
                 min_block_holds_frame_and_one_event);

class TraceWriter {
public:
    TraceWriter();

    // Emits the trace header through `sink` and arms the writer.
    // `buffer` must remain valid until finish() returns and be at least
    // kMinBlockBuffer bytes. Returns false on bad arguments or sink failure.
    bool begin(SinkFn sink, void* ctx, u8* buffer, u32 buf_len,
               u64 build_id, u64 seed, u16 flags);

    bool mmio_read(u64 ts, u32 addr, u32 value);
    bool mmio_write(u64 ts, u32 addr, u32 value);
    bool irq_enter(u64 ts, u32 vector);

    // Emits EV_STOP, flushes the pending block and writes the terminator.
    bool finish(u64 ts);

    bool ok() const { return ok_; }
    u32  events() const { return events_; }

    // Blocks the sink refused. Non-zero means the trace has a hole; the
    // sequence numbers show a reader exactly where and how big.
    u32  blocks_lost() const { return blocks_lost_; }

    // Events handed to the writer -- which is not the same as events that
    // reached the sink. When blocks_lost() is non-zero, some of these are in
    // blocks that never made it out.
    u64  bytes_written() const { return bytes_; }
    u16  flags() const { return flags_; }

    // Blocks emitted so far; also the seq the next block will carry.
    u32  blocks() const { return block_seq_; }

    // Why the writer latched an error, or "" while healthy.
    const char* error() const { return err_ ? err_ : ""; }

private:
    bool write_mmio(u8 type, u64 ts, u32 addr, u32 value);
    bool write_plain(u8 type, u64 ts, const u32* arg);
    bool stage(const u8* bytes, u32 n);
    // Always succeeds: a block the sink refuses is counted, not an error.
    void flush_block();
    bool emit(const u8* data, u32 len);
    u32  payload_cap() const;
    void fail(const char* why);

    // Non-copyable: two writers sharing one buffer would interleave blocks.
    TraceWriter(const TraceWriter&);
    TraceWriter& operator=(const TraceWriter&);

    SinkFn      sink_;
    void*       ctx_;
    u8*         buf_;
    u32         cap_;
    u32         used_;
    u64         last_ts_;
    u32         last_addr_;
    u32         block_seq_;
    u32         blocks_lost_;
    u32         events_;
    u64         bytes_;
    u16         flags_;
    bool        ok_;
    bool        started_;
    bool        finished_;
    const char* err_;
};

} // namespace rwd

#endif // REWIND_TRACE_WRITER_H
