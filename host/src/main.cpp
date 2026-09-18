// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
//   rewind hunt    -- run the firmware under many seeds, count the failures
//   rewind record  -- run it once and write the trace out
//   rewind replay  -- run that trace back, with no simulator involved
//   rewind dump    -- print a trace as events

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rewind/session.h"
#include "rewind/timeline.h"
#include "rewind/trace_reader.h"
#include "sim/mcu.h"

namespace {

using rwd::u32;
using rwd::u64;

int usage() {
    std::fprintf(stderr,
        "rewind -- deterministic record & replay for bare-metal firmware\n"
        "\n"
        "usage:\n"
        "  rewind hunt   [--from N] [--to N] [--variant buggy|fixed] [--save FILE]\n"
        "  rewind record [--seed N] [--variant buggy|fixed] [--tx N] [--iters N]\n"
        "                [--no-writes] --out FILE\n"
        "  rewind replay FILE [--iters N]\n"
        "  rewind dump   FILE [--limit N]\n"
        "  rewind sizing [--seed N] [--drain N] [--fifo N] [--slices N]\n"
        "  rewind inspect FILE [--at N] [--walk M] [--stride K]\n"
        "                      [--find-consumed N] [--iters N]\n"
        "\n"
        "common options:\n"
        "  --seed N      simulator seed; picks the interrupt arrival schedule\n"
        "  --variant V   buggy (default) clears the shared counter;\n"
        "                fixed subtracts it under a critical section\n"
        "  --tx N        bytes the UART delivers over the run (default 24)\n"
        "  --iters N     main-loop iterations (default 6000). Must match the\n"
        "                recording when replaying.\n"
        "  --no-writes   omit MMIO writes from the trace: smaller, but replay\n"
        "                loses its consistency check\n"
        "\n"
        "inspect options -- reverse execution over a recorded run:\n"
        "  --at N            go to event N (default: the end of the run)\n"
        "  --walk M          then step backwards M times, printing each\n"
        "  --stride K        events per backward step (default 1)\n"
        "  --find-consumed N bisect to the first point where the firmware had\n"
        "                    consumed N bytes, then walk back from there\n"
        "  --checkpoints K   snapshot every K main-loop iterations (default\n"
        "                    128). Raise it hugely to see what navigation\n"
        "                    costs without checkpoints.\n"
        "\n"
        "sizing options:\n"
        "  --drain N     bytes moved from the ring per pass through the main\n"
        "                loop (default 4096)\n"
        "  --fifo N      how many bytes the transport accepts per call; model\n"
        "                a UART FIFO with 4 or 8 (default: unlimited)\n"
        "  --slices N    drains per run (default 60). Fewer means the main\n"
        "                loop is busier and the ring has to hold more.\n");
    return 2;
}

bool read_file(const char* path, std::vector<rwd::u8>* out) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == 0) {
        std::fprintf(stderr, "rewind: cannot open %s\n", path);
        return false;
    }
    rwd::u8  chunk[8192];
    std::size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        out->insert(out->end(), chunk, chunk + n);
    }
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        std::fprintf(stderr, "rewind: error reading %s\n", path);
        return false;
    }
    return true;
}

bool write_file(const char* path, const std::vector<rwd::u8>& bytes) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == 0) {
        std::fprintf(stderr, "rewind: cannot write %s\n", path);
        return false;
    }
    const std::size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) {
        std::fprintf(stderr, "rewind: short write to %s\n", path);
        return false;
    }
    return true;
}

const char* event_name(rwd::u8 type) {
    switch (type) {
        case rwd::EV_MMIO_READ:  return "read ";
        case rwd::EV_MMIO_WRITE: return "write";
        case rwd::EV_IRQ_ENTER:  return "irq  ";
        case rwd::EV_STOP:       return "stop ";
        default:                    return "?    ";
    }
}

// Names the peripheral register an address belongs to, so a dump reads as
// firmware behaviour rather than as a column of hex.
const char* register_name(u32 addr) {
    switch (addr) {
        case sim::kSystickCnt: return "SYSTICK.CNT";
        case sim::kUart0Sr:    return "UART0.SR";
        case sim::kUart0Dr:    return "UART0.DR";
        case sim::kGpio0Odr:   return "GPIO0.ODR";
        default:               return "";
    }
}

struct Args {
    u64         from     = 1;
    u64         to       = 200;
    u64         seed     = 1;
    u32         tx       = 24;
    u32         iters    = 6000;
    u32         limit    = 0;
    u32         drain    = 4096;
    u32         fifo     = 0xFFFFFFFFu;
    u32         slices   = 60;
    u64         at       = 0;
    bool        has_at   = false;
    u32         walk     = 0;
    u32         stride   = 1;
    u32         find     = 0;
    bool        has_find = false;
    u32         ckpt     = 128;
    bool        writes   = true;
    fw::Variant variant  = fw::kBuggy;
    std::string out;
    std::string save;
    std::string file;
    bool        bad      = false;
};

Args parse(int argc, char** argv, int first) {
    Args a;
    for (int i = first; i < argc; ++i) {
        const std::string opt = argv[i];
        const bool has_value = (i + 1 < argc);

        if (opt == "--no-writes") {
            a.writes = false;
        } else if (opt == "--from" && has_value) {
            a.from = std::strtoull(argv[++i], 0, 10);
        } else if (opt == "--to" && has_value) {
            a.to = std::strtoull(argv[++i], 0, 10);
        } else if (opt == "--seed" && has_value) {
            a.seed = std::strtoull(argv[++i], 0, 10);
        } else if (opt == "--tx" && has_value) {
            a.tx = (u32)std::strtoul(argv[++i], 0, 10);
        } else if (opt == "--iters" && has_value) {
            a.iters = (u32)std::strtoul(argv[++i], 0, 10);
        } else if (opt == "--limit" && has_value) {
            a.limit = (u32)std::strtoul(argv[++i], 0, 10);
        } else if (opt == "--drain" && has_value) {
            a.drain = (u32)std::strtoul(argv[++i], 0, 10);
        } else if (opt == "--fifo" && has_value) {
            a.fifo = (u32)std::strtoul(argv[++i], 0, 10);
        } else if (opt == "--slices" && has_value) {
            a.slices = (u32)std::strtoul(argv[++i], 0, 10);
        } else if (opt == "--at" && has_value) {
            a.at     = std::strtoull(argv[++i], 0, 10);
            a.has_at = true;
        } else if (opt == "--walk" && has_value) {
            a.walk = (u32)std::strtoul(argv[++i], 0, 10);
        } else if (opt == "--stride" && has_value) {
            a.stride = (u32)std::strtoul(argv[++i], 0, 10);
        } else if (opt == "--checkpoints" && has_value) {
            a.ckpt = (u32)std::strtoul(argv[++i], 0, 10);
        } else if (opt == "--find-consumed" && has_value) {
            a.find     = (u32)std::strtoul(argv[++i], 0, 10);
            a.has_find = true;
        } else if (opt == "--out" && has_value) {
            a.out = argv[++i];
        } else if (opt == "--save" && has_value) {
            a.save = argv[++i];
        } else if (opt == "--variant" && has_value) {
            const std::string v = argv[++i];
            if (v == "fixed") {
                a.variant = fw::kFixed;
            } else if (v == "buggy") {
                a.variant = fw::kBuggy;
            } else {
                std::fprintf(stderr, "rewind: unknown variant '%s'\n", v.c_str());
                a.bad = true;
            }
        } else if (!opt.empty() && opt[0] != '-' && a.file.empty()) {
            a.file = opt;
        } else {
            std::fprintf(stderr, "rewind: unexpected argument '%s'\n", opt.c_str());
            a.bad = true;
        }
    }
    return a;
}

rwhost::RunConfig to_config(const Args& a) {
    rwhost::RunConfig cfg;
    cfg.seed       = a.seed;
    cfg.tx_total   = a.tx;
    cfg.main_iters = a.iters;
    cfg.variant    = a.variant;
    cfg.flags      = a.writes ? rwd::kFlagRecordWrites : 0;
    return cfg;
}

int cmd_hunt(const Args& a) {
    if (a.to < a.from) {
        std::fprintf(stderr, "rewind: --to must not be below --from\n");
        return 2;
    }

    u64                    runs = 0, failures = 0;
    std::vector<rwd::u8> first_failing;
    u64                     first_seed = 0;

    for (u64 seed = a.from; seed <= a.to; ++seed) {
        rwhost::RunConfig cfg = to_config(a);
        cfg.seed = seed;

        const rwhost::RecordResult rec = rwhost::record_run(cfg);
        if (!rec.writer_ok) {
            std::fprintf(stderr, "rewind: trace writer failed: %s\n",
                         rec.writer_error.c_str());
            return 1;
        }
        ++runs;

        if (rec.bug_triggered()) {
            ++failures;
            if (first_failing.empty()) {
                first_failing = rec.trace;
                first_seed    = seed;
                std::printf("seed %-8llu FAIL  consumed %u of %u bytes"
                            "  (%zu byte trace)\n",
                            (unsigned long long)seed, rec.fw.consumed,
                            rec.tx_sent, rec.trace.size());
            }
        }
    }

    std::printf("\n%llu of %llu runs failed (%.1f%%)\n",
                (unsigned long long)failures, (unsigned long long)runs,
                runs ? 100.0 * (double)failures / (double)runs : 0.0);

    if (failures == 0) {
        std::printf("no failure in this range.\n");
        return 0;
    }

    if (!a.save.empty()) {
        if (!write_file(a.save.c_str(), first_failing)) {
            return 1;
        }
        std::printf("wrote the seed %llu failure to %s -- "
                    "it now fails identically, every time:\n"
                    "  rewind replay %s\n",
                    (unsigned long long)first_seed, a.save.c_str(), a.save.c_str());
    } else {
        std::printf("pass --save FILE to keep the first failing trace.\n");
    }
    return 0;
}

// How big does the ring buffer on the device need to be?
//
// Sweeps power-of-two capacities and reports peak occupancy and losses. The
// answer is the smallest capacity that loses nothing, with headroom -- and
// the high-water column says how much headroom you are actually buying.
int cmd_sizing(const Args& a) {
    rwhost::BufferedConfig cfg;
    cfg.run           = to_config(a);
    cfg.drain_budget  = a.drain;
    cfg.link_per_call = a.fifo;
    cfg.slices        = a.slices;

    std::printf("seed %llu, draining %u bytes per pass, %u passes",
                (unsigned long long)a.seed, a.drain, a.slices);
    if (a.fifo != 0xFFFFFFFFu) {
        std::printf(", transport takes %u bytes per call", a.fifo);
    }
    std::printf("\n\n%10s  %10s  %11s  %12s  %s\n",
                "ring", "high water", "blocks lost", "bytes lost", "verdict");

    u32 smallest_safe = 0;
    for (u32 capacity = 128; capacity <= 65536u; capacity *= 2u) {
        cfg.ring_capacity = capacity;
        const rwhost::BufferedResult r = rwhost::record_buffered(cfg);
        if (!r.began) {
            std::fprintf(stderr, "rewind: could not start recorder at %u bytes\n",
                         capacity);
            return 1;
        }

        const bool safe = r.healthy;
        if (safe && smallest_safe == 0) {
            smallest_safe = capacity;
        }
        std::printf("%10u  %10u  %11u  %12llu  %s\n",
                    capacity, r.high_water, r.blocks_lost,
                    (unsigned long long)r.bytes_lost,
                    safe ? "ok" : "LOSES DATA");
    }

    if (smallest_safe == 0) {
        std::printf("\nNothing in this range kept up. Drain more often, or\n"
                    "record fewer events -- try --no-writes.\n");
        return 1;
    }
    std::printf("\nSmallest ring that loses nothing: %u bytes.\n", smallest_safe);
    std::printf("Ship more than that: this is one seed, and the arrival\n"
                "pattern that overruns it is by definition the one you did\n"
                "not test.\n");
    return 0;
}

int cmd_record(const Args& a) {
    if (a.out.empty()) {
        std::fprintf(stderr, "rewind: record needs --out FILE\n");
        return 2;
    }

    const rwhost::RecordResult rec = rwhost::record_run(to_config(a));
    if (!rec.writer_ok) {
        std::fprintf(stderr, "rewind: trace writer failed: %s\n",
                     rec.writer_error.c_str());
        return 1;
    }
    if (!write_file(a.out.c_str(), rec.trace)) {
        return 1;
    }

    std::printf("seed %llu  %u events  %zu bytes  %.1f bytes/event  %llu cycles\n",
                (unsigned long long)a.seed, rec.events, rec.trace.size(),
                (double)rec.trace.size() / (double)rec.events,
                (unsigned long long)rec.cycles);
    std::printf("uart delivered %u bytes, firmware consumed %u\n",
                rec.tx_sent, rec.fw.consumed);
    std::printf("result: %s\n", rec.bug_triggered() ? "BUG TRIGGERED" : "clean");
    return rec.bug_triggered() ? 1 : 0;
}

int cmd_replay(const Args& a) {
    if (a.file.empty()) {
        std::fprintf(stderr, "rewind: replay needs a trace file\n");
        return 2;
    }
    std::vector<rwd::u8> trace;
    if (!read_file(a.file.c_str(), &trace)) {
        return 1;
    }

    const rwhost::ReplayResult rep = rwhost::replay_run(trace, a.iters);
    if (!rep.loaded) {
        std::fprintf(stderr, "rewind: %s\n", rep.error.c_str());
        return 1;
    }

    std::printf("replayed %zu of %zu events -- no simulator, no hardware\n",
                rep.events_consumed, rep.events_total);
    std::printf("consumed %u bytes  checksum 0x%08X  state hash 0x%016llX\n",
                rep.fw.consumed, rep.fw.checksum,
                (unsigned long long)rep.fw.state_hash);

    if (rep.diverged) {
        std::fprintf(stderr, "\nDIVERGED: %s\n", rep.divergence.c_str());
        std::fprintf(stderr,
                     "the firmware asked for something this trace does not\n"
                     "have. Either it is not the build that recorded it, or\n"
                     "--iters does not match the recording.\n");
        return 1;
    }
    return 0;
}

bool consumed_at_least(const rwhost::Snapshot& snap, void* ctx) {
    return snap.fw.consumed >= *static_cast<const u32*>(ctx);
}

void print_snapshot_header() {
    std::printf("%9s  %9s  %7s  %8s  %8s  %5s  %5s  %10s\n",
                "event", "cycle", "iter", "pending", "consumed", "head",
                "tail", "checksum");
}

void print_snapshot(const rwhost::Snapshot& s) {
    std::printf("%9zu  %9llu  %7u  %8u  %8u  %5u  %5u  0x%08X\n",
                s.event_index, (unsigned long long)s.ts, s.main_iter,
                s.fw.pending, s.fw.consumed, s.fw.head, s.fw.tail,
                s.fw.checksum);
}

// Reverse execution: go to a point in the recorded run and read the
// firmware's state there, then walk backwards from it.
int cmd_inspect(const Args& a) {
    if (a.file.empty()) {
        std::fprintf(stderr, "rewind: inspect needs a trace file\n");
        return 2;
    }
    std::vector<rwd::u8> trace;
    if (!read_file(a.file.c_str(), &trace)) {
        return 1;
    }

    rwhost::Timeline timeline;
    if (!timeline.open(trace, a.iters, a.ckpt)) {
        std::fprintf(stderr, "rewind: %s\n", timeline.error().c_str());
        return 1;
    }

    std::printf("%zu events, %zu checkpoints\n\n",
                timeline.event_count(), timeline.checkpoints());

    rwhost::Snapshot snap;

    if (a.has_find) {
        u32 target = a.find;
        if (!timeline.find_first(&consumed_at_least, &target, &snap)) {
            std::printf("the firmware never consumed %u bytes in this run.\n",
                        a.find);
            return 1;
        }
        std::printf("first consumed %u bytes at event %zu, "
                    "found in %u seeks\n\n",
                    a.find, snap.event_index, timeline.last_search_seeks());
    } else {
        const std::size_t target =
            a.has_at ? (std::size_t)a.at : timeline.event_count();
        if (!timeline.seek(target, &snap)) {
            std::fprintf(stderr, "rewind: %s\n", timeline.error().c_str());
            return 1;
        }
    }

    print_snapshot_header();
    print_snapshot(snap);

    // Walking backwards. Nothing is undone -- each line is a fresh replay
    // from the nearest checkpoint to an earlier point in the same run.
    const u32 stride = a.stride ? a.stride : 1u;
    for (u32 step = 0; step < a.walk; ++step) {
        if (timeline.position() == 0) {
            break;
        }
        if (!timeline.step_back(stride, &snap)) {
            std::fprintf(stderr, "rewind: %s\n", timeline.error().c_str());
            return 1;
        }
        print_snapshot(snap);
    }

    if (a.walk > 0) {
        std::printf("\n%llu events replayed to produce that.\n",
                    (unsigned long long)timeline.events_replayed());
    }
    return 0;
}

int cmd_dump(const Args& a) {
    if (a.file.empty()) {
        std::fprintf(stderr, "rewind: dump needs a trace file\n");
        return 2;
    }
    std::vector<rwd::u8> bytes;
    if (!read_file(a.file.c_str(), &bytes)) {
        return 1;
    }

    rwd::TraceReader reader;
    if (!reader.open(bytes.data(), (u32)bytes.size())) {
        std::fprintf(stderr, "rewind: %s\n", reader.error());
        return 1;
    }

    fw::Variant variant;
    const bool  known = rwhost::variant_for_build_id(reader.header().build_id, &variant);
    std::printf("build 0x%016llX (%s)  seed %llu  flags 0x%04X  %zu bytes\n\n",
                (unsigned long long)reader.header().build_id,
                known ? (variant == fw::kFixed ? "fixed" : "buggy") : "unknown",
                (unsigned long long)reader.header().seed,
                reader.header().flags, bytes.size());
    std::printf("%10s  %-5s  %-10s %-13s %s\n",
                "cycle", "what", "addr", "register", "value");

    rwd::Event ev;
    u32           shown = 0;
    u32           total = 0;
    while (reader.next(&ev)) {
        ++total;
        if (a.limit != 0 && shown >= a.limit) {
            continue;
        }
        ++shown;

        if (ev.type == rwd::EV_IRQ_ENTER) {
            std::printf("%10llu  %-5s  vector %-3u %-13s\n",
                        (unsigned long long)ev.ts, event_name(ev.type), ev.addr,
                        ev.addr == sim::kIrqUartRx ? "UART_RX" : "");
        } else if (ev.type == rwd::EV_STOP) {
            std::printf("%10llu  %-5s\n", (unsigned long long)ev.ts, event_name(ev.type));
        } else {
            std::printf("%10llu  %-5s  0x%08X %-13s 0x%08X\n",
                        (unsigned long long)ev.ts, event_name(ev.type), ev.addr,
                        register_name(ev.addr), ev.value);
        }
    }

    if (!reader.ok()) {
        std::fprintf(stderr, "\nrewind: %s\n", reader.error());
        return 1;
    }
    if (a.limit != 0 && total > shown) {
        std::printf("\n... %u more events (raise --limit to see them)\n", total - shown);
    }
    std::printf("\n%u events\n", total);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }

    const std::string command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        usage();
        return 0;
    }

    const Args args = parse(argc, argv, 2);
    if (args.bad) {
        return 2;
    }

    if (command == "hunt")   return cmd_hunt(args);
    if (command == "record") return cmd_record(args);
    if (command == "replay") return cmd_replay(args);
    if (command == "dump")   return cmd_dump(args);
    if (command == "sizing") return cmd_sizing(args);
    if (command == "inspect") return cmd_inspect(args);

    std::fprintf(stderr, "rewind: unknown command '%s'\n", command.c_str());
    return usage();
}
