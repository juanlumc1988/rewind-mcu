// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Host-side pieces. C++17 here: this code only ever runs on a workstation,
// so it gets a heap, <vector> and <string>, none of which core/, target/ or
// firmware/ are allowed to touch.

#ifndef REWIND_REPLAY_H
#define REWIND_REPLAY_H

#include <cstddef>
#include <map>
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

    // The cursor at the moment of the first divergence. After it the firmware
    // keeps running against a trace it has left, and can by luck fall back
    // into step, so cursor() at the end of a run says little about how far a
    // replay really got; this does.
    std::size_t divergence_cursor() const { return divergence_cursor_; }

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

    // True unless the trace was recorded with kFlagNoSites. Then a probe point
    // in the firmware is a crossing the trace has an event for; otherwise it
    // is only a candidate place to deliver an interrupt (see set_landing).
    bool sites_recorded() const { return sites_recorded_; }

    // Where to deliver the interrupt that sits at trace event `event`: after
    // `probes` probe points have gone by since the last crossing. 0, the
    // default, is right after the crossing itself, which is where replay puts
    // every interrupt when it is not told otherwise. Only meaningful when the
    // trace was recorded without its sites.
    void set_landing(std::size_t event, u32 probes);
    void clear_landings();

    // Starts the run over from the top: cursor, clock, divergence, and the
    // probe count. Landings are kept; the event counter is not reset.
    void restart();

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
    void site(u32 id);
    void dma_read(u32 region, u32 offset, u8* dst, u32 len);
    void diverge(const char* fmt, ...);
    void maybe_capture();

    static u32  s_read32(void* ctx, u32 addr);
    static void s_write32(void* ctx, u32 addr, u32 value);
    static u64  s_now(void* ctx);
    static u32  s_poll_irq(void* ctx);
    static void s_site(void* ctx, u32 id);
    static void s_dma_read(void* ctx, u32 region, u32 offset, u8* dst, u32 len);

    ReplayHal(const ReplayHal&);
    ReplayHal& operator=(const ReplayHal&);

    rwd::HalOps             ops_;
    rwd::TraceHeader        header_;
    std::vector<rwd::Event> events_;
    std::size_t                cursor_;
    u64                        now_;
    std::size_t                divergence_cursor_;
    bool                       sites_recorded_;
    u32                        probe_count_;   // sites since the last crossing
    std::map<std::size_t, u32> landings_;
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
