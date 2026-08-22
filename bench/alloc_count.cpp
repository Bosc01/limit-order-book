#include "alloc_count.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
// Relaxed atomic: the sharded (multi-thread) benchmark counts allocations
// from every thread. Relaxed ordering is enough for a counter, and the cost
// is irrelevant because the whole point is that this never fires on the hot
// path.
std::atomic<std::uint64_t> g_allocs{0};
}

namespace bench {
void          alloc_count_reset() { g_allocs.store(0, std::memory_order_relaxed); }
std::uint64_t alloc_count_get()   { return g_allocs.load(std::memory_order_relaxed); }
} // namespace bench

// Replace global operator new/delete. With -fno-exceptions we cannot throw
// std::bad_alloc; allocation failure aborts, which is the right behavior for
// a trading system anyway — running out of memory mid-session is not a
// recoverable condition, and pretending it is just moves the crash somewhere
// less debuggable.
void* operator new(std::size_t sz) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(sz ? sz : 1);
    if (!p) std::abort();
    return p;
}

void* operator new[](std::size_t sz) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(sz ? sz : 1);
    if (!p) std::abort();
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
