// Benchmark driver. Usage:
//   bench [engine] [--ops N] [--warmup N] [--seed S] [--label STR] [--csv PATH]
// Engines: naive (more added in later phases).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
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
    std::size_t ops     = 200'000; // measured ops PER SHARD
    std::size_t warmup  = 20'000;
    std::uint64_t seed  = 42;
    std::string label   = "baseline";
    std::string csv     = "results/bench.csv";
    int         threads = 1; // shards; each runs its own engine instance
    std::size_t pool    = 1u << 17;          // final engine: order-pool slots
    std::size_t idmap   = std::size_t{1} << 18; // final engine: id-map slots
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
        else if (a == "--threads") c.threads = std::atoi(next());
        else if (a == "--pool")   c.pool   = std::strtoull(next(), nullptr, 10);
        else if (a == "--idmap")  c.idmap  = std::strtoull(next(), nullptr, 10);
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

// The final engine takes capacity parameters (pool slots, id-map slots) so
// the endurance run can be sized for its session, which is the documented
// production posture: growth is a safety valve, not a plan.
template <class Book>
std::unique_ptr<Book> make_book(const Config&) {
    return std::make_unique<Book>();
}
template <>
std::unique_ptr<lob::OrderBook> make_book<lob::OrderBook>(const Config& cfg) {
    return std::make_unique<lob::OrderBook>(
        lob::Stp::None, cfg.pool, lob::Price{1}, lob::Price{1} << 15, cfg.idmap);
}

// Sharded mode: one engine instance per thread, no shared state, the way
// real venues scale matching (one instrument, one engine, one core; cross-
// instrument flow never contends). Aggregate throughput is measured
// conservatively as total ops / wall time of the SLOWEST shard, from a
// common start barrier.
template <class Book>
int run_benchmark_sharded(const Config& cfg) {
    const auto cal = bench::calibrate_clock();
    const int  T   = cfg.threads;
    std::printf("clock: %.4f ns/tick; %d shards, %zu measured ops each\n",
                cal.ns_per_tick, T, cfg.ops);

    struct Shard {
        std::vector<bench::Op> ops;
        bench::RunResult       res;
    };
    std::vector<Shard> shards(static_cast<std::size_t>(T));
    for (int t = 0; t < T; ++t) {
        bench::WorkloadGen gen(cfg.seed + static_cast<std::uint64_t>(t));
        shards[std::size_t(t)].ops = gen.generate(cfg.warmup + cfg.ops);
    }

    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    for (int t = 0; t < T; ++t) {
        pool.emplace_back([&, t] {
            auto book = make_book<Book>(cfg);
            auto& sh = shards[std::size_t(t)];
            sh.res = bench::run_ops_gated(*book, sh.ops, cfg.warmup, [&] {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (!go.load(std::memory_order_acquire)) { /* spin */ }
            });
        });
    }
    while (ready.load(std::memory_order_acquire) < T) { /* spin */ }
    bench::alloc_count_reset();
    const auto w0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : pool) th.join();
    const auto w1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(w1 - w0).count();
    const std::uint64_t allocs = bench::alloc_count_get();

    double per_min = 1e18, per_max = 0;
    std::vector<std::uint64_t> all;
    all.reserve(std::size_t(T) * cfg.ops);
    for (auto& sh : shards) {
        const double thpt = double(cfg.ops) / sh.res.wall_seconds;
        per_min = thpt < per_min ? thpt : per_min;
        per_max = thpt > per_max ? thpt : per_max;
        for (const auto ticks : sh.res.latency_ticks)
            all.push_back(static_cast<std::uint64_t>(
                std::llround(double(ticks) * cal.ns_per_tick)));
    }
    const double agg = double(cfg.ops) * T / wall;

    auto s_all = bench::summarize(all);
    std::printf("\nengine=%s label=%s shards=%d\n", cfg.engine.c_str(),
                cfg.label.c_str(), T);
    std::printf("  %-8s %9s  %10s %8s %8s %8s %8s %10s\n",
                "kind", "count", "mean_ns", "p50", "p90", "p99", "p99.9", "max");
    print_row("ALL", s_all);
    append_csv(cfg, "ALL", s_all, agg);
    std::printf("\naggregate throughput: %.0f ops/s (%.1fM) over %.2f s wall\n",
                agg, agg / 1e6, wall);
    std::printf("per-shard throughput: %.1fM min .. %.1fM max ops/s\n",
                per_min / 1e6, per_max / 1e6);
    std::printf("heap allocations in measured region: %llu (%.6f per op)\n",
                (unsigned long long)allocs,
                double(allocs) / (double(cfg.ops) * T));
    return 0;
}

template <class Book>
int run_benchmark(const Config& cfg) {
    if (cfg.threads > 1) return run_benchmark_sharded<Book>(cfg);
    const auto cal = bench::calibrate_clock();
    std::printf("clock: %.4f ns/tick (%.1f MHz), read overhead ~%llu ticks (%.0f ns)\n",
                cal.ns_per_tick, 1000.0 / cal.ns_per_tick,
                (unsigned long long)cal.read_overhead_ticks,
                cal.read_overhead_ticks * cal.ns_per_tick);

    bench::WorkloadGen gen(cfg.seed);
    const auto ops = gen.generate(cfg.warmup + cfg.ops);
    std::printf("workload: %zu ops (%zu warmup + %zu measured), seed %llu\n",
                ops.size(), cfg.warmup, cfg.ops, (unsigned long long)cfg.seed);

    auto bookp = make_book<Book>(cfg);
    Book& book = *bookp;
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
