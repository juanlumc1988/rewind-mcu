// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// What the host tools need to know about a firmware to replay it.
//
// Replay runs the firmware itself on the workstation, against a trace instead of
// hardware; so does going backwards through a run, and so does asking what the
// state was at some event. All of that is the same handful of operations on
// something that is not this repository's example, and this is that handful.
// A firmware that wants the tools implements Firmware once; the example
// firmware's own implementation is ExampleFirmware (example_firmware.h), which
// is also the worked case.
//
// The firmware is compiled for the host with the shim (hal.h), which is what
// makes it replayable at all: the same source that runs on the part, its
// register accesses and interrupts going through mmio_read32 and friends. What
// the host needs from it beyond that is below.
//
// Its state
// ---------
// Everything that makes one run different from another and is not in the trace
// is *state*: the firmware's static storage. Going backwards is re-executing
// from a checkpoint, and a checkpoint is a copy of that state, so the firmware
// has to be able to save it and put it back (save_state / restore_state). The
// bytes are opaque to the tools; they are only ever given back to the firmware
// that made them, in the same process. Take care that "all of it" really is all
// of it: a variable left out of the saved state is one a checkpoint does not
// restore, and the replay from that checkpoint then differs from the run.
//
// Checkpoints are taken between iterations of the main loop (run(1) at a
// time), where no call is in progress. The firmware's main loop has to be one
// that can be run a few iterations, stopped, and run a few more.

#ifndef REWIND_FIRMWARE_H
#define REWIND_FIRMWARE_H

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rewind/rw_types.h"

namespace rwhost {

using rwd::u8;
using rwd::u32;
using rwd::u64;

// One named number of the firmware's state, for showing and comparing. A name of
// the form `name[index]` is an element of an array, and viewers show the
// elements together.
struct StateField {
    StateField() : value(0) {}
    StateField(const std::string& n, u64 v) : name(n), value(v) {}
    std::string name;
    u64         value;
};

class Firmware {
public:
    virtual ~Firmware() {}

    // A short name for what this is -- a variant, a build -- for the tools to
    // print next to a trace.
    virtual const char* name() const { return "firmware"; }

    // The state it has at power-on. Called first, before the shim is attached.
    virtual void reset() = 0;

    // Registers its interrupt handlers and priorities with the shim
    // (hal_set_isr, hal_set_irq_priority). Called right after the shim is
    // reset, every time the firmware is started.
    virtual void install() = 0;

    // Runs `iterations` passes of the main loop. May be called again to go on
    // from where the last call stopped.
    virtual void run(u32 iterations) = 0;

    // The whole of the state as bytes: how many, and the bytes themselves.
    virtual std::size_t state_size() const = 0;
    virtual void        save_state(u8* out) const = 0;       // exactly state_size() bytes
    virtual void        restore_state(const u8* in) = 0;

    // A number that is equal for equal states: how a replay is shown to have
    // ended where the recording did. The default is a hash of the saved bytes,
    // which is right unless they hold padding or anything else that can differ
    // between equal states; then override it.
    virtual u64 state_hash() const;

    // The state in a saved image, as named numbers for showing and comparing.
    // Optional; with none, the tools show the hash.
    virtual void describe_state(const u8* state, std::vector<StateField>* out) const {
        (void)state;
        out->clear();
    }

    // Puts the bytes a recording took of one region of the device's memory (see
    // hal_state_region) into the state. The default is the common case: one
    // region, number 0, that is the whole state in the layout save_state uses.
    virtual bool restore_region(u32 region, const u8* data, std::size_t len);
};

class ReplayHal;

// Puts `firmware` where a recording began: power-on, its handlers installed, and
// then, if the trace began mid-run (EV_RAM), the memory image it carries put
// back through Firmware::restore_region. The shim must already have been reset.
// False, with `why` set, if the firmware refuses a region of the image.
bool start_firmware(Firmware& firmware, const ReplayHal& trace, std::string* why);

// Makes the firmware that a trace's build id names, or null for one this does
// not know. How the tools find the firmware to replay a trace with.
typedef std::function<std::unique_ptr<Firmware>(u64 build_id)> FirmwareFactory;

} // namespace rwhost

#endif // REWIND_FIRMWARE_H
