// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// A trace opened for looking at: its header, its events, and -- when it was
// recorded by the example firmware -- the firmware's state at any point in it.
//
// Shared by the command line and the viewer so that both say the same thing.
// Like everything that runs firmware, it is not re-entrant: the shim is one per
// process. Alternating between two documents is fine, because every state
// lookup installs the state it needs from scratch.

#ifndef REWIND_TRACEDOC_H
#define REWIND_TRACEDOC_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "rewind/example_firmware.h"
#include "rewind/firmware.h"
#include "rewind/session.h"
#include "rewind/timeline.h"

namespace rwhost {

class TraceDoc {
public:
    TraceDoc();

    // How a trace's build id becomes the firmware that can replay it. The
    // example firmware's builds are known by default; a tool for another
    // firmware gives its own, before open(). Return null for an id it does not
    // know: the trace is then readable, but has no state to show.
    void set_firmware_factory(FirmwareFactory factory);

    // Decodes the trace. False if it cannot be read; error() says why. A trace
    // that is damaged part-way is refused, not shown in part.
    bool open(const std::vector<u8>& bytes, u32 main_iters = 6000);

    const std::string& error() const { return error_; }
    const rwd::TraceHeader& header() const { return header_; }
    const std::vector<rwd::Event>& events() const { return events_; }
    std::size_t size_bytes() const { return bytes_.size(); }

    // Cycle count of the last event: how long the run lasted.
    u64 duration() const { return events_.empty() ? 0 : events_.back().ts; }

    // Whether the header names a firmware build this can run, and which. For
    // the example firmware, variant() says which of its four; for any other it
    // is meaningless, and variant_name() is the firmware's own name.
    bool has_firmware() const { return firmware_ != nullptr; }
    const Firmware* firmware() const { return firmware_.get(); }
    fw::Variant variant() const;
    const char* variant_name() const;

    // The firmware's state just before event `index` is served: 0 is before the
    // run, events().size() is after it. False when there is no firmware to run.
    bool snapshot_at(std::size_t index, Snapshot* out);

private:
    TraceDoc(const TraceDoc&);
    TraceDoc& operator=(const TraceDoc&);

    std::vector<u8>          bytes_;
    rwd::TraceHeader         header_;
    std::vector<rwd::Event>  events_;
    FirmwareFactory          factory_;
    std::unique_ptr<Firmware> firmware_;   // null: no firmware knows this build
    u32                      main_iters_;
    std::string              error_;
    std::unique_ptr<Timeline> timeline_;   // built the first time it is needed
    bool                     timeline_failed_;
};

} // namespace rwhost

#endif // REWIND_TRACEDOC_H
