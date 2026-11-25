// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/tracedoc.h"

#include "rewind/trace_reader.h"

namespace rwhost {

TraceDoc::TraceDoc()
    : has_firmware_(false), variant_(fw::kBuggy), main_iters_(0), timeline_failed_(false) {
    header_.magic    = 0;
    header_.version  = 0;
    header_.flags    = 0;
    header_.build_id = 0;
    header_.seed     = 0;
}

const char* TraceDoc::variant_name() const {
    if (!has_firmware_) {
        return "unknown";
    }
    switch (variant_) {
        case fw::kFixed:     return "fixed";
        case fw::kUnguarded: return "unguarded";
        case fw::kDma:       return "dma";
        default:             return "buggy";
    }
}

bool TraceDoc::open(const std::vector<u8>& bytes, u32 main_iters) {
    bytes_ = bytes;
    events_.clear();
    timeline_.reset();
    timeline_failed_ = false;
    has_firmware_    = false;
    main_iters_      = main_iters;
    error_.clear();

    if (bytes.size() > 0xFFFFFFFFu) {
        error_ = "trace larger than 4 GiB";
        return false;
    }
    rwd::TraceReader reader;
    if (!reader.open(bytes.data(), (u32)bytes.size())) {
        error_ = reader.error();
        return false;
    }
    header_ = reader.header();

    rwd::Event ev;
    while (reader.next(&ev)) {
        events_.push_back(ev);
    }
    if (!reader.ok()) {
        error_ = reader.error();
        events_.clear();
        return false;
    }

    has_firmware_ = variant_for_build_id(header_.build_id, &variant_);
    return true;
}

bool TraceDoc::snapshot_at(std::size_t index, Snapshot* out) {
    if (!has_firmware_ || timeline_failed_) {
        return false;
    }
    if (!timeline_) {
        timeline_.reset(new Timeline());
        if (!timeline_->open(bytes_, main_iters_, 128)) {
            error_           = timeline_->error();
            timeline_failed_ = true;
            timeline_.reset();
            return false;
        }
    }
    return timeline_->seek(index, out);
}

} // namespace rwhost
