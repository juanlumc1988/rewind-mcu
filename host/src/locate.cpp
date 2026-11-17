// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/locate.h"

#include <algorithm>

namespace rwhost {

std::size_t LocateResult::unresolved() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (!choices[i].resolved) {
            ++n;
        }
    }
    return n;
}

std::size_t LocateResult::ambiguous() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].ambiguous) {
            ++n;
        }
    }
    return n;
}

namespace {

struct Outcome {
    bool        diverged;
    std::size_t at;          // cursor at the FIRST divergence (valid if diverged)
    std::string divergence;
    std::size_t cursor;      // cursor at the end of the run
};

class Search {
public:
    Search(ReplayHal* hal, RunFirmwareFn run, void* ctx, const LocateOptions& opt)
        : hal_(hal), run_(run), ctx_(ctx), opt_(opt), runs_(0), gave_up_(false),
          best_depth_(0) {
        const std::vector<rwd::Event>& events = hal_->events();
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].type == rwd::EV_IRQ_ENTER || events[i].type == rwd::EV_IRQ_AT) {
                irqs_.push_back(i);
            }
        }
        plan_.assign(irqs_.size(), 0u);
        best_.assign(irqs_.size(), 0u);
        tried_.assign(irqs_.size(), 0u);
        truncated_.assign(irqs_.size(), 0);
    }

    LocateResult run_search();

private:
    // One replay of the firmware with the current plan.
    Outcome replay() {
        hal_->clear_landings();
        for (std::size_t i = 0; i < irqs_.size(); ++i) {
            if (plan_[i] != 0u) {
                hal_->set_landing(irqs_[i], plan_[i]);
            }
        }
        hal_->restart();
        run_(ctx_, hal_);
        ++runs_;

        Outcome o;
        o.diverged   = hal_->diverged();
        o.at         = hal_->divergence_cursor();
        o.divergence = hal_->divergence();
        o.cursor     = hal_->cursor();
        return o;
    }

    // The interrupt was never delivered: the gap has fewer than k probe
    // points, the firmware reached its next crossing with it still held back,
    // and the replay stopped on it. No larger k can differ.
    bool never_delivered(const Outcome& o, std::size_t n, u32 k) const {
        return k > 0 && o.diverged && o.at == irqs_[n];
    }

    bool dfs(std::size_t n);

    ReplayHal*               hal_;
    RunFirmwareFn            run_;
    void*                    ctx_;
    LocateOptions            opt_;
    u32                      runs_;
    bool                     gave_up_;
    std::vector<std::size_t> irqs_;
    std::vector<u32>         plan_;       // the placement being tried, per interrupt
    std::vector<u32>         best_;       // the deepest consistent prefix found
    std::size_t              best_depth_;
    std::vector<u32>         tried_;
    std::vector<char>        truncated_;
};

// Chooses a placement for interrupt n, given those before it. True once the
// whole assignment replays to the end.
bool Search::dfs(std::size_t n) {
    const bool last = (n + 1 == irqs_.size());

    for (u32 k = 0; k <= opt_.max_probes; ++k) {
        if (runs_ >= opt_.max_runs) {
            gave_up_ = true;
            return false;
        }
        plan_[n] = k;
        const Outcome o = replay();

        if (never_delivered(o, n, k)) {
            break;
        }
        tried_[n] = std::max(tried_[n], k + 1u);
        if (k == opt_.max_probes) {
            truncated_[n] = 1;
        }

        // Up to the next interrupt only this one, and those already decided,
        // can have made the replay diverge; the later ones are still at the
        // default and may break it after that. So it fits if it is not
        // diverged, or got past the next interrupt's own event.
        const bool fits = !o.diverged || (!last && o.at > irqs_[n + 1]);
        if (!fits) {
            continue;
        }

        if (n + 1 > best_depth_) {
            best_depth_ = n + 1;
            best_       = plan_;
            for (std::size_t j = n + 1; j < best_.size(); ++j) {
                best_[j] = 0u;
            }
        }
        if (last) {
            return true;          // fits at the last one means: not diverged
        }
        if (dfs(n + 1)) {
            return true;
        }
        for (std::size_t j = n + 1; j < plan_.size(); ++j) {
            plan_[j] = 0u;
        }
        if (gave_up_) {
            return false;
        }
    }
    plan_[n] = 0u;
    return false;
}

LocateResult Search::run_search() {
    LocateResult result;
    result.applicable   = true;
    result.events_total = hal_->event_count();

    bool solved = true;
    if (!irqs_.empty()) {
        solved = dfs(0);
    }

    // With a solution, ask of each interrupt whether anywhere else it could
    // have been would also have reproduced the whole trace.
    std::vector<u32> fitting(irqs_.size(), 1u);
    std::vector<u32> cand(irqs_.size(), 0u);
    if (solved && !irqs_.empty()) {
        const std::vector<u32> solution = plan_;
        for (std::size_t i = 0; i < irqs_.size() && !gave_up_; ++i) {
            cand[i] = 1;   // the chosen placement itself
            for (u32 k = 0; k <= opt_.max_probes; ++k) {
                if (k == solution[i]) {
                    continue;
                }
                if (runs_ >= opt_.max_runs) {
                    gave_up_ = true;
                    break;
                }
                plan_[i] = k;
                const Outcome o = replay();
                if (never_delivered(o, i, k)) {
                    break;
                }
                ++cand[i];
                if (k == opt_.max_probes) {
                    truncated_[i] = 1;
                }
                if (!o.diverged) {
                    ++fitting[i];
                }
            }
            plan_[i] = solution[i];
        }
        plan_ = solution;
    } else if (!irqs_.empty()) {
        // No placement of every interrupt reproduces the trace. Report the
        // deepest consistent prefix; from the first interrupt that nothing
        // fits, they are unresolved and left at the default.
        plan_ = best_;
    }

    for (std::size_t i = 0; i < irqs_.size(); ++i) {
        const rwd::Event& ev = hal_->events()[irqs_[i]];
        LandingChoice c;
        c.event      = irqs_[i];
        c.ts         = ev.ts;
        c.vector     = ev.addr;
        c.probes     = plan_[i];
        c.candidates = std::max(tried_[i], cand[i]);
        c.resolved   = solved || i < best_depth_;
        c.fitting    = c.resolved ? (solved ? fitting[i] : 1u) : 0u;
        c.ambiguous  = solved && fitting[i] > 1u;
        c.truncated  = truncated_[i] != 0;
        result.choices.push_back(c);
    }

    // The located replay, last, so the firmware's state is its outcome.
    const Outcome fin = replay();
    result.final_diverged   = fin.diverged;
    result.final_divergence = fin.divergence;
    result.final_cursor     = fin.diverged ? fin.at : fin.cursor;
    result.runs             = runs_;
    result.gave_up          = gave_up_;
    return result;
}

} // namespace

LocateResult locate_landings(const std::vector<u8>& trace, RunFirmwareFn run,
                             void* ctx, const LocateOptions& options) {
    ReplayHal hal;
    if (!hal.load(trace.data(), trace.size())) {
        LocateResult bad;
        bad.why = hal.error();
        return bad;
    }
    if (hal.sites_recorded()) {
        LocateResult none;
        none.why = "this trace recorded its probe points, so replay already "
                   "delivers every interrupt where it landed; there is "
                   "nothing to search for";
        return none;
    }

    Search search(&hal, run, ctx, options);
    return search.run_search();
}

} // namespace rwhost
