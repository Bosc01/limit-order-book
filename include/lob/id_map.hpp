#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Open-addressing hash map from OrderId to a small value (an Order*).
//
// Why not std::unordered_map? Chaining: every insert heap-allocates a node,
// every lookup chases at least one out-of-line pointer, and erase frees to
// the general allocator. That is one allocation per resting order -- on the
// hottest path in the engine -- plus a cache miss per cancel.
//
// This map is a single flat array of {key, value} slots. Linear probing:
// a lookup lands on the hashed slot and walks forward; with a load factor
// capped at 0.5 the expected probe length is ~1.5 slots, which is usually
// the SAME cache line it already loaded. No allocation ever happens at
// steady state (the table is pre-sized; growth doubles and rehashes, an
// amortized event the benchmark proves never fires when sized right).
//
// Deletion uses backward-shift (Robin-Hood-style compaction) instead of
// tombstones: on erase, subsequent probe-chain members that would become
// unreachable are shifted back into the hole. Tombstones would make probe
// chains grow monotonically under the book's cancel-heavy churn; backward
// shift keeps lookups at their theoretical cost forever.
//
// Key 0 marks an empty slot, so OrderId 0 is reserved -- the engine rejects
// it at the door (real venues also reserve sentinel ids).
template <class V>
class IdMap {
public:
    explicit IdMap(std::size_t min_slots = std::size_t{1} << 18) {
        std::size_t n = 16;
        while (n < min_slots) n <<= 1; // power of two: mask instead of modulo
        slots_.resize(n);
        mask_  = n - 1;
        shift_ = 64 - std::countr_zero(n);
    }

    V* find(OrderId k) {
        std::size_t i = home(k);
        while (slots_[i].key != 0) {
            if (slots_[i].key == k) return &slots_[i].val;
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    // Returns false (and changes nothing) if the key is already present.
    bool insert(OrderId k, V v) {
        if ((count_ + 1) * 2 > slots_.size()) grow(); // keeps load <= 0.5
        std::size_t i = home(k);
        while (slots_[i].key != 0) {
            if (slots_[i].key == k) return false;
            i = (i + 1) & mask_;
        }
        slots_[i] = {k, v};
        ++count_;
        return true;
    }

    bool erase(OrderId k) {
        std::size_t i = home(k);
        while (slots_[i].key != k) {
            if (slots_[i].key == 0) return false;
            i = (i + 1) & mask_;
        }
        // Backward shift: walk the rest of the probe cluster; any element
        // whose ideal slot lies cyclically at-or-before the hole would
        // become unreachable, so it moves into the hole and leaves a new
        // hole where it was.
        std::size_t hole = i;
        std::size_t j    = i;
        while (true) {
            j = (j + 1) & mask_;
            if (slots_[j].key == 0) break;
            const std::size_t ideal = home(slots_[j].key);
            if (in_cyclic_range(ideal, hole, j)) {
                slots_[hole] = slots_[j];
                hole         = j;
            }
        }
        slots_[hole].key = 0;
        --count_;
        return true;
    }

    std::size_t size() const { return count_; }
    std::size_t slot_count() const { return slots_.size(); }
    std::size_t grow_count() const { return grows_; }

private:
    struct Slot {
        OrderId key = 0;
        V       val{};
    };

    // Home slot of a key, by Fibonacci hashing: multiply by 2^64 divided by
    // the golden ratio and keep the top log2(slots) bits. One multiply, and
    // it still scatters SEQUENTIAL ids (which is what gateways generate):
    // any run of consecutive ids splits the table into gaps of at most three
    // sizes, and the golden ratio keeps those sizes as close to equal as any
    // multiplier can (the three-gap theorem). Identity hashing sequential
    // keys would fill the table in runs and make probe clusters explode.
    std::size_t home(OrderId k) const {
        return static_cast<std::size_t>((k * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    // Is `i` in the cyclic half-open range [ideal, j)?
    static bool in_cyclic_range(std::size_t ideal, std::size_t i, std::size_t j) {
        if (ideal <= j) return ideal <= i && i < j;
        return ideal <= i || i < j; // range wraps past zero
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.size() * 2, Slot{});
        mask_  = slots_.size() - 1;
        shift_ = 64 - std::countr_zero(slots_.size());
        count_ = 0;
        ++grows_;
        for (const Slot& s : old)
            if (s.key != 0) insert(s.key, s.val);
    }

    std::vector<Slot> slots_;
    std::size_t       mask_  = 0;
    int               shift_ = 64; // 64 - log2(slot count)
    std::size_t       count_ = 0;
    std::size_t       grows_ = 0;
};

} // namespace lob
