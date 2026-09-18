// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Pull-style reader over a whole trace held in memory. It allocates nothing
// and keeps no copy of the buffer, so it also runs on the target -- handy
// for a device that replays its own last trace after a watchdog reset.
//
// Every block's CRC is verified as the reader steps into it, which means a
// corrupt trace is reported at the block that is actually damaged instead of
// surfacing later as an implausible event.

#ifndef REWIND_TRACE_READER_H
#define REWIND_TRACE_READER_H

#include "rewind/trace.h"

namespace rwd {

class TraceReader {
public:
    TraceReader();

    // `data` must outlive the reader. Returns false if the header is absent,
    // malformed, or written by an incompatible version.
    bool open(const u8* data, u32 len);

    const TraceHeader& header() const { return hdr_; }

    // Fills *out and returns true, or returns false at end of stream or on
    // the first error. Distinguish the two with ok().
    bool next(Event* out);

    bool ok() const { return err_ == 0; }
    bool at_end() const { return done_; }
    const char* error() const { return err_ ? err_ : ""; }

private:
    bool load_next_block();
    bool fail(const char* why);

    const u8*   data_;
    u32         len_;
    TraceHeader hdr_;
    u32         block_end_;
    u32         pos_;
    u32         next_block_off_;
    u64         last_ts_;
    u32         last_addr_;
    bool        opened_;
    bool        done_;
    const char* err_;
};

} // namespace rwd

#endif // REWIND_TRACE_READER_H
