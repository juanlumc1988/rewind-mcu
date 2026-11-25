// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/tracediff.h"

#include <algorithm>
#include <cstdio>

namespace rwhost {

namespace {

bool ignored(const DiffOptions& o, u32 addr) {
    for (std::size_t i = 0; i < o.ignore_value_at.size(); ++i) {
        if (o.ignore_value_at[i] == addr) {
            return true;
        }
    }
    return false;
}

} // namespace

bool events_match(const rwd::Event& a, const rwd::Event& b, const DiffOptions& o) {
    if (a.type != b.type) {
        return false;
    }
    if (o.compare_time && a.ts != b.ts) {
        return false;
    }
    if (a.addr != b.addr) {
        return false;
    }
    if (a.type == rwd::EV_MMIO_READ && ignored(o, a.addr)) {
        return true;   // the register, but not what it held
    }
    if (a.value != b.value || a.len != b.len) {
        return false;
    }
    for (u32 i = 0; i < (u32)a.len; ++i) {
        if (a.data[i] != b.data[i]) {
            return false;
        }
    }
    return true;
}

namespace {

enum Op { kOpEqual, kOpDelete, kOpInsert };   // delete: only in A; insert: only in B

// Appends one step to the script, merging it into the previous hunk of the
// same kind.
void push_op(std::vector<DiffHunk>* hunks, Op op, std::size_t a, std::size_t b) {
    const HunkKind kind = (op == kOpEqual) ? kHunkEqual
                        : (op == kOpDelete) ? kHunkOnlyA : kHunkOnlyB;
    if (!hunks->empty()) {
        DiffHunk& last = hunks->back();
        if (last.kind == kind) {
            if (kind != kHunkOnlyB) { ++last.na; }
            if (kind != kHunkOnlyA) { ++last.nb; }
            return;
        }
    }
    DiffHunk h;
    h.kind = kind;
    h.a    = a;
    h.b    = b;
    h.na   = (kind == kHunkOnlyB) ? 0u : 1u;
    h.nb   = (kind == kHunkOnlyA) ? 0u : 1u;
    hunks->push_back(h);
}

} // namespace

TraceDiff diff_traces(const std::vector<rwd::Event>& a, const std::vector<rwd::Event>& b,
                      const DiffOptions& o) {
    TraceDiff out;
    const std::size_t na = a.size();
    const std::size_t nb = b.size();

    // What the two runs have in common at each end costs nothing to find and
    // is most of any real pair of traces.
    std::size_t prefix = 0;
    while (prefix < na && prefix < nb && events_match(a[prefix], b[prefix], o)) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < na - prefix && suffix < nb - prefix
           && events_match(a[na - 1 - suffix], b[nb - 1 - suffix], o)) {
        ++suffix;
    }

    out.first = prefix;
    if (prefix == na && prefix == nb) {
        out.identical = true;
        out.equal     = na;
        if (na > 0) {
            DiffHunk h = { kHunkEqual, 0, na, 0, nb };
            out.hunks.push_back(h);
        }
        return out;
    }

    // The middle, a[prefix .. na-suffix) against b[prefix .. nb-suffix).
    const std::size_t n = na - prefix - suffix;
    const std::size_t m = nb - prefix - suffix;

    std::vector<DiffHunk> middle;
    bool minimal = true;

    if (n == 0 || m == 0) {
        // One side has nothing left: everything on the other is extra.
        for (std::size_t i = 0; i < n; ++i) { push_op(&middle, kOpDelete, prefix + i, prefix); }
        for (std::size_t j = 0; j < m; ++j) { push_op(&middle, kOpInsert, prefix, prefix + j); }
    } else {
        // Myers' greedy shortest edit script. V[k] is the furthest x reached on
        // diagonal k = x - y after d edits; each d's V is kept for the walk back.
        const long long max_d = (long long)std::min<std::size_t>(n + m, o.max_edits);
        const long long off   = max_d + 1;
        std::vector<long long>              v(2 * (std::size_t)off + 2, 0);
        std::vector<std::vector<long long> > trace;
        bool found = false;
        long long final_d = 0;

        for (long long d = 0; d <= max_d && !found; ++d) {
            for (long long k = -d; k <= d; k += 2) {
                long long x;
                if (k == -d || (k != d && v[(std::size_t)(off + k - 1)] < v[(std::size_t)(off + k + 1)])) {
                    x = v[(std::size_t)(off + k + 1)];
                } else {
                    x = v[(std::size_t)(off + k - 1)] + 1;
                }
                long long y = x - k;
                while (x < (long long)n && y < (long long)m
                       && events_match(a[prefix + (std::size_t)x], b[prefix + (std::size_t)y], o)) {
                    ++x;
                    ++y;
                }
                v[(std::size_t)(off + k)] = x;
                if (x >= (long long)n && y >= (long long)m) {
                    found = true;
                    final_d = d;
                    break;
                }
            }
            // Keep this round, so the way back can be retraced.
            trace.push_back(std::vector<long long>(v.begin() + (off - d - 1),
                                                   v.begin() + (off + d + 2)));
        }

        if (!found) {
            // Past the cap. Still a valid alignment -- the middle simply counted
            // as entirely different -- but not a shortest one.
            minimal = false;
            for (std::size_t i = 0; i < n; ++i) { push_op(&middle, kOpDelete, prefix + i, prefix); }
            for (std::size_t j = 0; j < m; ++j) { push_op(&middle, kOpInsert, prefix + n, prefix + j); }
        } else {
            // Walk back from (n, m), collecting the steps in reverse.
            std::vector<Op>          ops;
            long long x = (long long)n;
            long long y = (long long)m;
            for (long long d = final_d; d > 0; --d) {
                const std::vector<long long>& vd = trace[(std::size_t)(d - 1)];
                const long long base = d - 1 + 1;   // index of k = 0 within a slice of round d-1
                const long long k = x - y;
                // At the two ends of the diagonals only one neighbour exists, so
                // read neither until it is known which are in range.
                long long prev_k;
                if (k == -d) {
                    prev_k = k + 1;
                } else if (k == d) {
                    prev_k = k - 1;
                } else {
                    const long long km1 = vd[(std::size_t)(base + k - 1)];
                    const long long kp1 = vd[(std::size_t)(base + k + 1)];
                    prev_k = (km1 < kp1) ? k + 1 : k - 1;
                }
                const long long prev_x = vd[(std::size_t)(base + prev_k)];
                const long long prev_y = prev_x - prev_k;
                while (x > prev_x && y > prev_y) {   // the snake
                    ops.push_back(kOpEqual);
                    --x;
                    --y;
                }
                ops.push_back(prev_k == k + 1 ? kOpInsert : kOpDelete);
                x = prev_x;
                y = prev_y;
            }
            while (x > 0 && y > 0) {                 // the leading snake of round 0
                ops.push_back(kOpEqual);
                --x;
                --y;
            }

            std::size_t ai = prefix;
            std::size_t bi = prefix;
            for (std::size_t i = ops.size(); i > 0; --i) {
                const Op op = ops[i - 1];
                push_op(&middle, op, ai, bi);
                if (op != kOpInsert) { ++ai; }
                if (op != kOpDelete) { ++bi; }
            }
        }
    }

    // Assemble: common prefix, the middle, common suffix.
    if (prefix > 0) {
        DiffHunk h = { kHunkEqual, 0, prefix, 0, prefix };
        out.hunks.push_back(h);
    }
    for (std::size_t i = 0; i < middle.size(); ++i) {
        out.hunks.push_back(middle[i]);
    }
    if (suffix > 0) {
        DiffHunk h = { kHunkEqual, na - suffix, suffix, nb - suffix, suffix };
        out.hunks.push_back(h);
    }

    // Neighbouring equal hunks (an empty middle) merge.
    std::vector<DiffHunk> merged;
    for (std::size_t i = 0; i < out.hunks.size(); ++i) {
        const DiffHunk& h = out.hunks[i];
        if (!merged.empty() && merged.back().kind == kHunkEqual && h.kind == kHunkEqual) {
            merged.back().na += h.na;
            merged.back().nb += h.nb;
        } else {
            merged.push_back(h);
        }
    }
    out.hunks = merged;

    out.minimal = minimal;
    for (std::size_t i = 0; i < out.hunks.size(); ++i) {
        const DiffHunk& h = out.hunks[i];
        if (h.kind == kHunkEqual)      { out.equal  += h.na; }
        else if (h.kind == kHunkOnlyA) { out.only_a += h.na; }
        else                           { out.only_b += h.nb; }
    }
    return out;
}

namespace {

// `from_a` says which side `index` is on. The same walk serves both.
std::size_t map_index(const TraceDiff& d, std::size_t index, std::size_t other_size, bool from_a) {
    std::size_t result    = 0;
    std::size_t gap_there = 0;      // where, on the other side, the current differing stretch begins
    bool        prev_equal = true;

    for (std::size_t i = 0; i < d.hunks.size(); ++i) {
        const DiffHunk& h = d.hunks[i];
        const std::size_t here        = from_a ? h.a  : h.b;
        const std::size_t count       = from_a ? h.na : h.nb;
        const std::size_t there       = from_a ? h.b  : h.a;
        const std::size_t there_count = from_a ? h.nb : h.na;

        // A stretch of differences can be several hunks (one side's events,
        // then the other's). It begins at the first of them.
        if (h.kind != kHunkEqual && prev_equal) {
            gap_there = there;
        }
        prev_equal = (h.kind == kHunkEqual);

        // A hunk with none of this side's events cannot hold the index; the
        // index belongs to the next one.
        if (count > 0 && index < here + count) {
            result = (h.kind == kHunkEqual) ? there + (index - here) : gap_there;
            return other_size == 0 ? 0 : (result < other_size ? result : other_size - 1);
        }
        result = there + there_count;
    }
    // Past the end of the alignment.
    return other_size == 0 ? 0 : (result < other_size ? result : other_size - 1);
}

} // namespace

std::size_t map_a_to_b(const TraceDiff& d, std::size_t a_index, std::size_t b_size) {
    return map_index(d, a_index, b_size, true);
}

std::size_t map_b_to_a(const TraceDiff& d, std::size_t b_index, std::size_t a_size) {
    return map_index(d, b_index, a_size, false);
}

std::vector<StateDelta> diff_state(const fw::State& a, const fw::State& b) {
    std::vector<StateDelta> out;
    auto field = [&out](const char* name, u64 x, u64 y) {
        if (x != y) {
            StateDelta d;
            d.name = name;
            d.a    = x;
            d.b    = y;
            out.push_back(d);
        }
    };
    field("head",       a.head,       b.head);
    field("tail",       a.tail,       b.tail);
    field("pending",    a.pending,    b.pending);
    field("consumed",   a.consumed,   b.consumed);
    field("checksum",   a.checksum,   b.checksum);
    field("last_stamp", a.last_stamp, b.last_stamp);
    field("overflows",  a.overflows,  b.overflows);
    field("dma_tail",   a.dma_tail,   b.dma_tail);
    field("dma_lost",   a.dma_lost,   b.dma_lost);
    for (u32 i = 0; i < fw::kRingSize; ++i) {
        if (a.ring[i] != b.ring[i]) {
            char name[16];
            std::snprintf(name, sizeof(name), "ring[%u]", i);
            field(name, a.ring[i], b.ring[i]);
        }
    }
    return out;
}

} // namespace rwhost
