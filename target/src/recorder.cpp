// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/recorder.h"

namespace rwd {

Recorder::Recorder() : started_(false) {}

bool Recorder::sink(void* ctx, const u8* data, u32 len) {
    RingBuffer* ring = static_cast<RingBuffer*>(ctx);

    // The critical section exists because firmware records from its main
    // loop and from inside ISRs, so this push can interrupt another push.
    // RingBuffer's lock-free path handles one producer against one consumer;
    // it does not handle two producers, and pretending otherwise would
    // corrupt a block roughly as often as the bug being hunted.
    hal_irq_disable();
    const bool pushed = ring->push(data, len);
    hal_irq_enable();

    // A rejected push tells the writer this block did not make it. The
    // writer counts it, advances the sequence number so the hole is visible,
    // and keeps recording -- a device that briefly could not keep up should
    // lose the burst it could not buffer, not the rest of the run.
    return pushed;
}

bool Recorder::begin(u8* ring_storage, u32 ring_capacity,
                     u8* block_buffer, u32 block_bytes,
                     u64 build_id, u64 seed, u16 flags) {
    if (!ring_.init(ring_storage, ring_capacity)) {
        return false;
    }
    return writer_.begin(&Recorder::sink, &ring_, block_buffer, block_bytes,
                         build_id, seed, flags);
}

void Recorder::start() {
    started_ = true;
    hal_mode_record(&writer_);
}

void Recorder::stop(u64 final_ts) {
    writer_.finish(final_ts);
    hal_mode_off();
    started_ = false;
}

u32 Recorder::drain(TransportFn transport, void* ctx, u32 max_bytes) {
    if (transport == 0) {
        return 0;
    }

    u32 sent = 0;
    while (sent < max_bytes) {
        const u8* chunk = 0;
        u32       avail = ring_.peek(&chunk);
        if (avail == 0) {
            break;
        }
        if (avail > max_bytes - sent) {
            avail = max_bytes - sent;
        }

        const u32 taken = transport(ctx, chunk, avail);
        if (taken == 0) {
            break;   // link is full; try again next time round the loop
        }
        // A transport claiming more than it was offered is a bug in the
        // transport. Consuming that much would hand out bytes that were
        // never sent, so clamp rather than trust it.
        ring_.consume(taken > avail ? avail : taken);
        sent += (taken > avail ? avail : taken);
    }
    return sent;
}

bool Recorder::healthy() const {
    // blocks_lost() counts refused blocks; ring drops additionally catch a
    // refused header or terminator, which are emitted through the same sink
    // but are not blocks.
    return writer_.ok() && writer_.blocks_lost() == 0 && ring_.drops() == 0;
}

} // namespace rwd
