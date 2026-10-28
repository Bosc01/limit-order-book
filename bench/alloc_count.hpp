#pragma once
#include <cstdint>

namespace bench {

// Counts calls to the replaced global operator new (every overload, in
// alloc_count.cpp), so the harness can report heap allocations in the
// measured region.
void          alloc_count_reset();
std::uint64_t alloc_count_get();

} // namespace bench
