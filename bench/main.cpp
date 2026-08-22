// Benchmark driver. Usage:
//   bench [engine] [--ops N] [--warmup N] [--seed S] [--label STR] [--csv PATH]
// Engines: naive (more added in later phases).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "alloc_count.hpp"
#include "clock.hpp"
#include "harness.hpp"
#include "stats.hpp"
#include "workload.hpp"

#include "../engines/naive_book.hpp"
#include "../engines/book_v1.hpp"
#include "../engines/book_v2.hpp"
#include "../engines/book_v3.hpp"
#include "../engines/book_v4.hpp"
#include "../engines/book_v5.hpp"
#include "lob/order_book.hpp"

namespace {

struct Config {
    std::string engine  = "naive";
    std::size_t ops     = 200'000; // measured ops
    std::size_t warmup  = 20'000;
    std::uint64_t seed  = 42;
    std::string label   = "baseline";
    std::string csv     = "results/bench.csv";
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--ops")    c.ops    = std::strtoull(next(), nullptr, 10);
        else if (a == "--warmup") c.warmup = std::strtoull(next(), nullptr, 10);
        else if (a == "--seed")   c.seed   = std::strtoull(next(), nullptr, 10);
        else if (a == "--label")  c.label  = next();
        else if (a == "--csv")    c.csv    = next();
        else if (a[0] != '-')     c.engine = a;
        else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); std::exit(1); }
    }
    return c;
}

const char* kind_name(bench::OpKind k) {
    switch (k) {
        case bench::OpKind::Limit:  return "limit";
        case bench::OpKind::Market: return "market";
        case bench::OpKind::Cancel: return "cancel";
        case bench::OpKind::Modify: return "modify";
    }
    return "?";
}

void print_row(const char* kind, const bench::Summary& s) {
    std::printf("  %-8s %9zu  %10.0f %8llu %8llu %8llu %8llu %10llu\n",
                kind, s.count, s.mean_ns,
                (unsigned long long)s.p50, (unsigned long long)s.p90,
                (unsigned long long)s.p99, (unsigned long long)s.p999,
                (unsigned long long)s.max);
}

void append_csv(const Config& cfg, const char* kind, const bench::Summary& s,
                double throughput) {
    ::mkdir("results", 0755); // ok if it already exists
    FILE* f = std::fopen(cfg.csv.c_str(), "a");
    if (!f) { std::perror("fopen csv"); return; }
    // header only when the file is empty
    if (std::ftell(f) == 0)
        std::fprintf(f, "label,engine,seed,measured_ops,kind,count,mean_ns,"
                        "p50_ns,p90_ns,p99_ns,p999_ns,max_ns,throughput_ops_s\n");
    std::fprintf(f, "%s,%s,%llu,%zu,%s,%zu,%.1f,%llu,%llu,%llu,%llu,%llu,",
                 cfg.label.c_str(), cfg.engine.c_str(),
                 (unsigned long long)cfg.seed, cfg.ops, kind, s.count, s.mean_ns,
                 (unsigned long long)s.p50, (unsigned long long)s.p90,
                 (unsigned long long)s.p99, (unsigned long long)s.p999,
                 (unsigned long long)s.max);
    if (throughput > 0) std::fprintf(f, "%.0f\n", throughput);
    else                std::fprintf(f, "\n");
    std::fclose(f);
}

template <class Book>
int run_benchmark(const Config& cfg) {
    const auto cal = bench::calibrate_clock();
    std::printf("clock: %.4f ns/tick (%.1f MHz), read overhead ~%llu ticks (%.0f ns)\n",
                cal.ns_per_tick, 1000.0 / cal.ns_per_tick,
                (unsigned long long)cal.read_overhead_ticks,
                cal.read_overhead_ticks * cal.ns_per_tick);

    bench::WorkloadGen gen(cfg.seed);
    const auto ops = gen.generate(cfg.warmup + cfg.ops);
    std::printf("workload: %zu ops (%zu warmup + %zu measured), seed %llu\n",
                ops.size(), cfg.warmup, cfg.ops, (unsigned long long)cfg.seed);

    Book book;
    auto run = bench::run_ops(book, ops, cfg.warmup); // resets alloc counter internally
    const std::uint64_t allocs = bench::alloc_count_get();

    // Convert ticks -> ns and split samples per op kind. The timestamp-read
    // overhead is included in every sample identically across engines, so
    // before/after deltas are still apples-to-apples.
    std::vector<std::uint64_t> all, per_kind[bench::kNumOpKinds];
    all.reserve(run.latency_ticks.size());
    for (std::size_t i = 0; i < run.latency_ticks.size(); ++i) {
        const auto ns = static_cast<std::uint64_t>(
            std::llround(run.latency_ticks[i] * cal.ns_per_tick));
        all.push_back(ns);
        per_kind[static_cast<int>(ops[cfg.warmup + i].kind)].push_back(ns);
    }

    const double throughput = cfg.ops / run.wall_seconds;

    std::printf("\nengine=%s label=%s\n", cfg.engine.c_str(), cfg.label.c_str());
    std::printf("  %-8s %9s  %10s %8s %8s %8s %8s %10s\n",
                "kind", "count", "mean_ns", "p50", "p90", "p99", "p99.9", "max");
    for (int k = 0; k < bench::kNumOpKinds; ++k) {
        auto s = bench::summarize(per_kind[k]);
        print_row(kind_name(static_cast<bench::OpKind>(k)), s);
        append_csv(cfg, kind_name(static_cast<bench::OpKind>(k)), s, 0);
    }
    auto s_all = bench::summarize(all);
    print_row("ALL", s_all);
    append_csv(cfg, "ALL", s_all, throughput);

    std::printf("\nthroughput: %.0f ops/s (%.2f s wall for %zu ops)\n",
                throughput, run.wall_seconds, cfg.ops);
    std::printf("heap allocations in measured region: %llu (%.3f per op)\n",
                (unsigned long long)allocs, double(allocs) / cfg.ops);
    std::printf("resting orders at end: %zu\n", book.resting_orders());
    std::printf("csv appended: %s\n", cfg.csv.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);
    if (cfg.engine == "naive") return run_benchmark<lob::NaiveBook>(cfg);
    if (cfg.engine == "v1")    return run_benchmark<lob::v1::Book>(cfg);
    if (cfg.engine == "v2")    return run_benchmark<lob::v2::Book>(cfg);
    if (cfg.engine == "v3")    return run_benchmark<lob::v3::Book>(cfg);
    if (cfg.engine == "v4")    return run_benchmark<lob::v4::Book>(cfg);
    if (cfg.engine == "v5")    return run_benchmark<lob::v5::Book>(cfg);
    if (cfg.engine == "final") return run_benchmark<lob::OrderBook>(cfg);
    std::fprintf(stderr,
                 "unknown engine '%s' (have: naive v1 v2 v3 v4 v5 final)\n",
                 cfg.engine.c_str());
    return 1;
}
