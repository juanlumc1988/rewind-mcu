// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// What firmware actually instantiates to record itself.
//
// Three pieces that have to agree with each other:
//
//   TraceWriter   serialises events into blocks
//   RingBuffer    absorbs the difference between how fast blocks are
//                 produced and how fast the link can carry them
//   drain()       hands whole chunks to a transport, from the main loop
//
// Recording happens in whatever context touched the peripheral, main loop or
// ISR. Draining happens only from the main loop. That is one producer
// against one consumer, except that "one producer" is a lie the moment an
// ISR records something while the main loop is mid-push -- so the sink masks
// interrupts around the push itself. The masked window is a bounded copy of
// at most one block, no loops and no I/O.
//
// Memory is entirely the caller's: two buffers, sized at link time, no heap
// anywhere. A reasonable starting point on a part with 64 KiB of RAM is a
// 256-byte block buffer and a 4 KiB ring, then look at
// ring().high_water() on real traffic and cut it down.

#ifndef REWIND_RECORDER_H
#define REWIND_RECORDER_H

#include "rewind/hal.h"
#include "rewind/ring_buffer.h"
#include "rewind/trace_writer.h"

namespace rwd {

// Hands bytes to a link. Returns how many it accepted, which may be fewer
// than offered -- a UART with a four-deep FIFO is the normal case, not the
// exception -- or zero when it can take no more right now.
typedef u32 (*TransportFn)(void* ctx, const u8* data, u32 len);

class Recorder {
public:
    Recorder();

    // `ring_storage` must be a power-of-two number of bytes; `block_buffer`
    // at least kMinBlockBuffer. Both must outlive the Recorder. Emits the
    // trace header into the ring.
    bool begin(u8* ring_storage, u32 ring_capacity,
               u8* block_buffer, u32 block_bytes,
               u64 build_id, u64 seed, u16 flags);

    // Puts the shim into MODE_RECORD. Call after hal_attach().
    void start();

    // Emits EV_STOP, flushes the final block and leaves MODE_RECORD. The
    // ring still holds whatever has not been drained.
    void stop(u64 final_ts);

    // Moves up to `max_bytes` from the ring to the transport. Call from the
    // main loop's idle time. Returns bytes accepted by the transport.
    u32 drain(TransportFn transport, void* ctx, u32 max_bytes);

    // Everything the writer produced reached the ring, and the writer has
    // not latched an error. False means the trace has a hole in it -- the
    // block sequence numbers will show a reader exactly where.
    bool healthy() const;

    TraceWriter&      writer() { return writer_; }
    const RingBuffer& ring() const { return ring_; }
    u32               drops() const { return ring_.drops(); }
    u32               blocks_lost() const { return writer_.blocks_lost(); }

private:
    static bool sink(void* ctx, const u8* data, u32 len);

    Recorder(const Recorder&);
    Recorder& operator=(const Recorder&);

    RingBuffer  ring_;
    TraceWriter writer_;
    bool        started_;
};

} // namespace rwd

#endif // REWIND_RECORDER_H
