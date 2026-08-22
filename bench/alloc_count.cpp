#include "alloc_count.hpp"

#include <cstdlib>
#include <new>

namespace {
std::uint64_t g_allocs = 0;
}

namespace bench {
void          alloc_count_reset() { g_allocs = 0; }
std::uint64_t alloc_count_get()   { return g_allocs; }
} // namespace bench

// Replace global operator new/delete. With -fno-exceptions we cannot throw
// std::bad_alloc; allocation failure aborts, which is the right behavior for
// a trading system anyway — running out of memory mid-session is not a
// recoverable condition, and pretending it is just moves the crash somewhere
// less debuggable.
void* operator new(std::size_t sz) {
    ++g_allocs;
    void* p = std::malloc(sz ? sz : 1);
    if (!p) std::abort();
    return p;
}

void* operator new[](std::size_t sz) {
    ++g_allocs;
    void* p = std::malloc(sz ? sz : 1);
    if (!p) std::abort();
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
