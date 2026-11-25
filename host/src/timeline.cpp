// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/timeline.h"

#include "rewind/example_firmware.h"
#include "rewind/trace_reader.h"

namespace rwhost {

Timeline::Timeline()
    : firmware_(0), main_iters_(0), checkpoint_every_(128),
      event_count_(0), position_(0), last_search_seeks_(0), opened_(false),
      pending_(0), current_iter_(0) {}

bool Timeline::install(const std::vector<u8>* state, std::size_t cursor, u64 ts) {
    rwd::hal_reset();
    if (state != 0) {
        firmware_->reset();
        firmware_->install();
        firmware_->restore_state(state->data());
    } else if (!start_firmware(*firmware_, replay_, &error_)) {
        // Where the recording began, which may be mid-run.
        rwd::hal_reset();
        return false;
    }
    rwd::hal_attach(replay_.ops());
    rwd::hal_mode_replay();
    replay_.set_cursor(cursor, ts);
    return true;
}

// What the firmware's state is right now, into a snapshot.
void Timeline::fill(Snapshot* snap) const {
    snap->state.assign(firmware_->state_size(), 0);
    if (!snap->state.empty()) {
        firmware_->save_state(snap->state.data());
    }
    snap->state_hash = firmware_->state_hash();
    firmware_->describe_state(snap->state.data(), &snap->fields);
    if (dynamic_cast<const ExampleFirmware*>(firmware_) != 0) {
        ExampleFirmware::decode(snap->state.data(), snap->state.size(), &snap->fw);
    }
}

void Timeline::capture(void* ctx) {
    Timeline* self = static_cast<Timeline*>(ctx);
    if (self->pending_ == 0) {
        return;
    }
    self->fill(self->pending_);
    self->pending_->ts        = self->replay_.now();
    self->pending_->main_iter = self->current_iter_;
    self->pending_->valid     = true;
}

bool Timeline::open(const std::vector<u8>& trace, u32 main_iters,
                    u32 checkpoint_every) {
    opened_ = false;
    error_.clear();

    // Which example variant, by the build id in the header.
    if (trace.size() > 0xFFFFFFFFu) {
        error_ = "trace larger than 4 GiB";
        return false;
    }
    rwd::TraceReader reader;
    if (!reader.open(trace.data(), (u32)trace.size())) {
        error_ = reader.error();
        return false;
    }
    owned_ = make_example_firmware(reader.header().build_id);
    if (!owned_) {
        error_ = "trace was recorded by an unknown firmware build";
        return false;
    }
    return open(trace, owned_.get(), main_iters, checkpoint_every);
}

bool Timeline::open(const std::vector<u8>& trace, Firmware* firmware, u32 main_iters,
                    u32 checkpoint_every) {
    opened_ = false;
    checkpoints_.clear();
    error_.clear();

    if (firmware == 0) {
        error_ = "no firmware to replay the trace with";
        return false;
    }
    firmware_ = firmware;
    if (owned_.get() != firmware) {
        owned_.reset();
    }

    if (!replay_.load(trace.data(), trace.size())) {
        error_ = replay_.error();
        return false;
    }

    main_iters_       = main_iters;
    checkpoint_every_ = checkpoint_every ? checkpoint_every : 1u;

    // One full pass, snapshotting between iterations. Iteration boundaries
    // are the only points where a copy of the state is a complete description
    // of the run: no call in progress, interrupt mask balanced.
    if (!install(0, replay_.first_event(), replay_.now())) {
        return false;
    }
    replay_.clear_capture_point();

    for (u32 iter = 0; iter < main_iters_; ++iter) {
        if (iter % checkpoint_every_ == 0) {
            Checkpoint cp;
            cp.main_iter = iter;
            cp.cursor    = replay_.cursor();
            cp.ts        = replay_.now();
            cp.state.assign(firmware_->state_size(), 0);
            if (!cp.state.empty()) {
                firmware_->save_state(cp.state.data());
            }
            checkpoints_.push_back(cp);
        }
        firmware_->run(1);
    }

    event_count_ = replay_.cursor();
    rwd::hal_reset();

    position_ = 0;
    opened_   = true;
    return true;
}

const Timeline::Checkpoint* Timeline::nearest(std::size_t event_index) const {
    // Checkpoints are in increasing cursor order; take the last one at or
    // before the target.
    const Checkpoint* best = 0;
    for (std::size_t i = 0; i < checkpoints_.size(); ++i) {
        if (checkpoints_[i].cursor <= event_index) {
            best = &checkpoints_[i];
        } else {
            break;
        }
    }
    return best;
}

bool Timeline::seek(std::size_t event_index, Snapshot* out) {
    if (!opened_ || out == 0) {
        return false;
    }
    if (event_index > event_count_) {
        event_index = event_count_;
    }
    // Before the first event the firmware asks for there is only the state the
    // recording began in -- a trace that began mid-run has a memory image there.
    if (event_index < replay_.first_event()) {
        event_index = replay_.first_event();
    }

    const Checkpoint* cp = nearest(event_index);
    if (cp == 0) {
        error_ = "no checkpoint at or before the requested event";
        return false;
    }

    Snapshot snap;
    snap.event_index = event_index;

    install(&cp->state, cp->cursor, cp->ts);
    pending_      = &snap;
    current_iter_ = cp->main_iter;
    replay_.set_capture_point(event_index, &Timeline::capture, this);

    // Forward from the checkpoint, one iteration at a time so the capture
    // point can be observed and the run stopped as soon as it passes.
    for (u32 iter = cp->main_iter; iter < main_iters_; ++iter) {
        current_iter_ = iter;
        if (replay_.captured()) {
            break;
        }
        firmware_->run(1);
    }

    if (!replay_.captured()) {
        // The target is the end of the run: nothing further is served, so no
        // access ever sits on it. The final state is the answer.
        fill(&snap);
        snap.ts         = replay_.now();
        snap.main_iter  = main_iters_;
        snap.valid      = true;
    }

    pending_ = 0;
    replay_.clear_capture_point();
    rwd::hal_reset();

    position_ = event_index;
    *out      = snap;
    return snap.valid;
}

bool Timeline::find_first(Predicate pred, void* ctx, Snapshot* out) {
    if (!opened_ || pred == 0 || out == 0) {
        return false;
    }

    last_search_seeks_ = 0;
    Snapshot probe;

    // If it never holds, there is nothing to find. Checking the end first
    // also means a false search costs one seek rather than log(N) of them.
    ++last_search_seeks_;
    if (!seek(event_count_, &probe)) {
        return false;
    }
    if (!pred(probe, ctx)) {
        return false;
    }

    std::size_t lo = 0;
    std::size_t hi = event_count_;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2u;
        ++last_search_seeks_;
        if (!seek(mid, &probe)) {
            return false;
        }
        if (pred(probe, ctx)) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }

    ++last_search_seeks_;
    return seek(lo, out);
}

bool Timeline::step_back(std::size_t n, Snapshot* out) {
    const std::size_t target = (n > position_) ? 0 : position_ - n;
    return seek(target, out);
}

bool Timeline::step_forward(std::size_t n, Snapshot* out) {
    return seek(position_ + n, out);
}

} // namespace rwhost
