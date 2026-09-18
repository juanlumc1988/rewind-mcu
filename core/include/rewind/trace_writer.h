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

// Returns false to signal a write failure; the writer then latches an error
// and every subsequent call is a no-op. Must not re-enter the writer.
typedef bool (*SinkFn)(void* ctx, const u8* data, u32 len);

// A block buffer smaller than this cannot hold a worst-case event.
const u32 kMinBlockBuffer = 64;

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
    u64  bytes_written() const { return bytes_; }
    u16  flags() const { return flags_; }

    // Why the writer latched an error, or "" while healthy.
    const char* error() const { return err_ ? err_ : ""; }

private:
    bool write_mmio(u8 type, u64 ts, u32 addr, u32 value);
    bool write_plain(u8 type, u64 ts, const u32* arg);
    bool stage(const u8* bytes, u32 n);
    bool flush_block();
    bool emit(const u8* data, u32 len);
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
