#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace lob {

// Fixed-type slab pool. A free slot's first word links to the next free
// slot. Slabs are pre-faulted when added, so first use of a slot never takes
// a page fault. When the pool runs dry it adds a slab twice the size of the
// last and counts it in grow_count(); a pool sized for the session should
// never grow, and growth is preferred to rejecting orders mid-session.
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

    // Placement new starts the object's lifetime; assigning through a T*
    // into recycled storage would not.
    template <class... Args>
    T* create(Args&&... args) {
        return ::new (raw_slot()) T{std::forward<Args>(args)...};
    }

    void destroy(T* p) {
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
        if (cursor_ == slab_end_) add_slab(last_slab_elems_ * 2);
        void* p = cursor_;
        cursor_ += sizeof(T);
        return p;
    }

    void add_slab(std::size_t elems) {
        void* mem = ::operator new(elems * sizeof(T), std::align_val_t{alignof(T)});
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
