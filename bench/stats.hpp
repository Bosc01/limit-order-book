#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace bench {

struct Summary {
    std::size_t   count = 0;
    double        mean_ns = 0;
    std::uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

// Nearest-rank percentile on a sorted vector. We keep every sample and sort
// once at the end (exact percentiles) instead of using a histogram
// (approximate). At benchmark scale — millions of 8-byte samples — the
// memory cost is trivial and exactness beats cleverness.
inline std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    const auto idx = static_cast<std::size_t>(
        std::llround(q * static_cast<double>(sorted.size() - 1)));
    return sorted[idx];
}

inline Summary summarize(std::vector<std::uint64_t>& samples) {
    Summary s;
    s.count = samples.size();
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    s.mean_ns = std::accumulate(samples.begin(), samples.end(), 0.0) /
                static_cast<double>(samples.size());
    s.p50  = percentile(samples, 0.50);
    s.p90  = percentile(samples, 0.90);
    s.p99  = percentile(samples, 0.99);
    s.p999 = percentile(samples, 0.999);
    s.max  = samples.back();
    return s;
}

} // namespace bench
