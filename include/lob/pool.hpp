#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

namespace lob {

// Fixed-type slab pool with an intrusive free list.
//
// Why not new/delete per order?
//   * general-purpose malloc takes locks, maintains size classes, and can
//     syscall (mmap) at arbitrary moments -- all invisible until they land in
//     your p99.9;
//   * consecutive allocations land wherever the heap has holes, so walking a
//     price level pointer-chases across random pages (TLB + cache misses).
// The pool hands out fixed-size slots from big contiguous slabs: allocation
// is "pop a pointer", free is "push a pointer", and orders created close in
// time sit close in memory.
//
// Where the memory comes from: slabs are allocated up front (constructor) via
// aligned operator new. If the pool runs dry it allocates one more slab of
// double the size -- a deliberate safety valve. A production engine sizes the
// pool for the whole session (you know your venue's order-rate ceiling) and
// treats growth as an incident; rejecting orders would also be defensible,
// crashing would not. grow_count() exposes how often the valve opened so the
// benchmark can prove it stayed shut.
//
// The free list is intrusive: a dead slot's first 8 bytes store the pointer
// to the next dead slot, so bookkeeping costs zero extra memory -- the same
// trick the order book's intrusive lists use.
template <class T>
class Pool {
    static_assert(std::is_trivially_destructible_v<T>,
                  "pool never runs destructors on reclaim");
    static_assert(sizeof(T) >= sizeof(void*), "slot must hold a free-list link");

public:
    explicit Pool(std::size_t initial_capacity) { add_slab(initial_capacity); }

    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;

    ~Pool() {
        for (auto& s : slabs_)
            ::operator delete(s.mem, std::align_val_t{alignof(T)});
    }

    // Hot path: one predictable branch, no syscalls, no locks. Placement-new
    // into recycled storage keeps this UB-free (assigning through a T* that
    // was never constructed would not be).
    template <class... Args>
    T* create(Args&&... args) {
        return ::new (raw_slot()) T{static_cast<Args&&>(args)...};
    }

    void destroy(T* p) {
        // T is trivially destructible; the dead object's storage becomes the
        // free-list node.
        auto* n = reinterpret_cast<FreeNode*>(p);
        n->next = free_;
        free_   = n;
    }

    std::size_t capacity() const { return capacity_; }
    std::size_t grow_count() const { return slabs_.size() - 1; }

private:
    struct FreeNode {
        FreeNode* next;
    };
    struct Slab {
        void* mem;
    };

    void* raw_slot() {
        if (free_ != nullptr) {
            FreeNode* n = free_;
            free_ = n->next;
            return n;
        }
        if (cursor_ == slab_end_) add_slab(last_slab_elems_ * 2); // safety valve
        void* p = cursor_;
        cursor_ += sizeof(T);
        return p;
    }

    void add_slab(std::size_t elems) {
        void* mem = ::operator new(elems * sizeof(T), std::align_val_t{alignof(T)});
        // Pre-fault: touch every page now so first-use of a slot never takes
        // a page fault on the hot path (real engines pre-fault and mlock
        // their pools for the same reason).
        std::memset(mem, 0, elems * sizeof(T));
        slabs_.push_back({mem});
        cursor_          = static_cast<std::byte*>(mem);
        slab_end_        = cursor_ + elems * sizeof(T);
        last_slab_elems_ = elems;
        capacity_       += elems;
    }

    FreeNode*         free_            = nullptr;
    std::byte*        cursor_          = nullptr;
    std::byte*        slab_end_        = nullptr;
    std::size_t       last_slab_elems_ = 0;
    std::size_t       capacity_        = 0;
    std::vector<Slab> slabs_;
};

} // namespace lob
