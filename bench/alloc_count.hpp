#pragma once
#include <cstdint>

namespace bench {

// Global-new interposer so the harness can report "heap allocations during
// the measured region". This is the evidence for the Phase 3 claim "no heap
// allocation on the hot path": the naive book shows ~1 allocation per
// resting order, the pooled engine must show 0.
// Single-threaded benchmark -> plain counters, no atomics needed.
void          alloc_count_reset();
std::uint64_t alloc_count_get();

} // namespace bench
