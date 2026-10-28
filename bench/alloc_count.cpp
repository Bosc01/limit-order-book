#include "alloc_count.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
// Atomic because sharded runs allocate from several threads; relaxed is
// enough for a counter.
std::atomic<std::uint64_t> g_allocs{0};
}

namespace bench {
void          alloc_count_reset() { g_allocs.store(0, std::memory_order_relaxed); }
std::uint64_t alloc_count_get()   { return g_allocs.load(std::memory_order_relaxed); }
} // namespace bench

// Built with -fno-exceptions, so allocation failure aborts instead of
// throwing std::bad_alloc.
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

// Aligned overloads: the order pool allocates its slabs with
// operator new(size, align_val_t); without these, slab growth would bypass
// the counter and the "allocations in measured region" line would
// undercount. std::aligned_alloc requires size to be a multiple of the
// alignment, so round up.
void* operator new(std::size_t sz, std::align_val_t al) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    const std::size_t a = static_cast<std::size_t>(al);
    void* p = std::aligned_alloc(a, (sz + a - 1) / a * a);
    if (!p) std::abort();
    return p;
}

void* operator new[](std::size_t sz, std::align_val_t al) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    const std::size_t a = static_cast<std::size_t>(al);
    void* p = std::aligned_alloc(a, (sz + a - 1) / a * a);
    if (!p) std::abort();
    return p;
}

void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
