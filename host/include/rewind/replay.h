// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Host-side pieces. C++17 here: this code only ever runs on a workstation,
// so it gets a heap, <vector> and <string>, none of which core/, target/ or
// firmware/ are allowed to touch.

#ifndef REWIND_REPLAY_H
#define REWIND_REPLAY_H

#include <cstddef>
#include <string>
#include <vector>

#include "rewind/hal.h"
#include "rewind/trace_reader.h"

namespace rwhost {

using rwd::u8;
using rwd::u16;
using rwd::u32;
using rwd::u64;

// A TraceWriter sink that accumulates into memory. On the target the
// equivalent sink pushes into a ring buffer drained over UART, SWO or RTT.
class VectorSink {
public:
    std::vector<u8> bytes;
    static bool write(void* ctx, const u8* data, u32 len);
};

// A HalOps backed by a recorded trace instead of by hardware.
//
// The firmware cannot tell the difference, and that is the whole trick: it
// runs the same code down the same paths, and every input it asks for comes
// out of the file. If it asks for something the recording does not have --
// an access the recorded run never made, in an order it never made it -- the
// mismatch is a divergence, and the run is over.
class ReplayHal {
public:
    ReplayHal();

    // Decodes the whole trace up front. Returns false on a malformed trace;
    // error() then says why.
    bool load(const u8* data, std::size_t len);

    const rwd::HalOps* ops() const { return &ops_; }
    const rwd::TraceHeader& header() const { return header_; }

    const char* error() const { return error_.c_str(); }
    const std::string& divergence() const { return divergence_; }
    bool diverged() const { return !divergence_.empty(); }

    // Called at the moment a chosen event is about to be served. The
    // Timeline uses it to snapshot firmware state at an exact point.
    typedef void (*CaptureFn)(void* ctx);

    // Fires `fn` immediately before event `index` is served -- that is, in
    // the instant the event is about to happen, with the firmware still in
    // the state that preceded it. Asking "what did `pending` look like when
    // that interrupt arrived?" wants the state before, not after.
    void set_capture_point(std::size_t index, CaptureFn fn, void* ctx);
    void clear_capture_point();
    bool captured() const { return captured_; }

    // Repositions the cursor, for resuming from a checkpoint. Clears any
    // latched divergence, since a restored run has not diverged yet.
    void set_cursor(std::size_t cursor, u64 now);

    // Events served across the whole life of this object. The measure of how
    // much work reverse navigation actually costs.
    u64 events_served() const { return events_served_; }

    std::size_t cursor() const { return cursor_; }
    std::size_t event_count() const { return events_.size(); }
    const std::vector<rwd::Event>& events() const { return events_; }
    u64 now() const { return now_; }

private:
    u32  read32(u32 addr);
    void write32(u32 addr, u32 value);
    u32  poll_irq();
    void diverge(const char* fmt, ...);
    void maybe_capture();

    static u32  s_read32(void* ctx, u32 addr);
    static void s_write32(void* ctx, u32 addr, u32 value);
    static u64  s_now(void* ctx);
    static u32  s_poll_irq(void* ctx);

    ReplayHal(const ReplayHal&);
    ReplayHal& operator=(const ReplayHal&);

    rwd::HalOps             ops_;
    rwd::TraceHeader        header_;
    std::vector<rwd::Event> events_;
    std::size_t                cursor_;
    u64                        now_;
    std::size_t                capture_at_;
    CaptureFn                  capture_fn_;
    void*                      capture_ctx_;
    bool                       captured_;
    u64                        events_served_;
    bool                       check_writes_;
    std::string                error_;
    std::string                divergence_;
    // hal_set_divergence() stores the pointer, not the text, so the message
    // lives here for as long as this object does.
    char                       detail_[256];
};

} // namespace rwhost

#endif // REWIND_REPLAY_H
