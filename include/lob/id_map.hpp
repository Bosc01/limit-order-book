#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Open-addressing map from OrderId to a small value, in one flat array of
// {key, value} slots. Linear probing over a power-of-two table with the load
// factor held at or below 0.5: 1.5 expected probes per hit, 2.5 per miss.
// Erase shifts later members of the probe run back into the hole instead of
// leaving a tombstone, so runs do not lengthen under cancel churn. Growth
// doubles and rehashes, and is counted; a table sized for the session never
// grows. Key 0 marks an empty slot.
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
        // Backward shift: a later member of the run whose home is at or
        // before the hole (cyclically) moves into it, and its old slot
        // becomes the hole.
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

    // Fibonacci hashing: multiply by 2^64/phi and keep the top log2(slots)
    // bits. Consecutive ids, which is what a gateway issues, land in evenly
    // spread slots (Knuth, TAOCP 6.4); identity hashing would fill the table
    // in runs.
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
