#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace bench {

// Raw monotonic tick source, chosen per architecture:
//
//   aarch64: CNTVCT_EL0, the ARM generic timer's virtual counter. Readable
//     from userspace in a single instruction. The preceding `isb` is an
//     instruction barrier: without it the CPU may hoist the counter read
//     above the work we are trying to time (out-of-order execution does not
//     respect our intent, only data dependencies).
//     Granularity = 1/CNTFRQ. On Apple Silicon this is commonly 24 MHz,
//     i.e. ~41.7 ns per tick — we measure and report it rather than assume.
//
//   x86_64: RDTSC. Constant-rate ("invariant TSC") on all modern parts,
//     sub-nanosecond granularity.
//
// Why not std::chrono::steady_clock in the hot loop? It bottoms out in a
// libc call (mach_absolute_time / clock_gettime). That adds overhead *and
// variance* to every sample, and variance in the measuring stick shows up
// as fake tail latency in p99.9.
inline std::uint64_t rdticks() {
#if defined(__aarch64__)
    std::uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v));
    return v;
#elif defined(__x86_64__)
    std::uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (std::uint64_t(hi) << 32) | lo;
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct ClockCalibration {
    double        ns_per_tick;         // measured against steady_clock
    std::uint64_t read_overhead_ticks; // median of back-to-back read deltas
};

// Calibrate ticks -> ns by running both clocks across a ~200 ms busy-wait
// window. Busy-wait, not sleep: sleeping lets the core drop frequency and
// can migrate the thread, both of which pollute the calibration.
inline ClockCalibration calibrate_clock() {
    using namespace std::chrono;

    const auto c0 = steady_clock::now();
    const auto t0 = rdticks();
    while (steady_clock::now() - c0 < milliseconds(200)) { /* spin */ }
    const auto t1 = rdticks();
    const auto c1 = steady_clock::now();

    const double ns = static_cast<double>(
        duration_cast<nanoseconds>(c1 - c0).count());
    const double ns_per_tick = ns / static_cast<double>(t1 - t0);

    // Median back-to-back delta = cost of one timestamp read. Median, not
    // min: min underestimates (lucky pipelining), mean overestimates
    // (interrupts land on some samples).
    std::array<std::uint64_t, 1001> d{};
    for (auto& x : d) {
        const auto a = rdticks();
        const auto b = rdticks();
        x = b - a;
    }
    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    return {ns_per_tick, d[d.size() / 2]};
}

} // namespace bench
