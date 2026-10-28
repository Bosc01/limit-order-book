#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>

#include "lob/id_map.hpp"

namespace {

TEST(IdMap, InsertFindErase) {
    lob::IdMap<int> m(16);
    EXPECT_TRUE(m.insert(42, 7));
    ASSERT_NE(m.find(42), nullptr);
    EXPECT_EQ(*m.find(42), 7);
    EXPECT_EQ(m.find(43), nullptr);
    EXPECT_TRUE(m.erase(42));
    EXPECT_EQ(m.find(42), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(IdMap, DuplicateInsertRejected) {
    lob::IdMap<int> m(16);
    EXPECT_TRUE(m.insert(1, 10));
    EXPECT_FALSE(m.insert(1, 20));
    EXPECT_EQ(*m.find(1), 10); // original value untouched
    EXPECT_EQ(m.size(), 1u);
}

TEST(IdMap, EraseMissingReturnsFalse) {
    lob::IdMap<int> m(16);
    EXPECT_FALSE(m.erase(5));
    m.insert(5, 1);
    EXPECT_TRUE(m.erase(5));
    EXPECT_FALSE(m.erase(5));
}

TEST(IdMap, GrowthRehashPreservesEntries) {
    lob::IdMap<int> m(16); // tiny: forces several rehashes
    for (std::uint64_t k = 1; k <= 1000; ++k) ASSERT_TRUE(m.insert(k, int(k * 3)));
    EXPECT_GT(m.grow_count(), 0u);
    for (std::uint64_t k = 1; k <= 1000; ++k) {
        ASSERT_NE(m.find(k), nullptr) << k;
        EXPECT_EQ(*m.find(k), int(k * 3));
    }
}

// The one that catches backward-shift bugs: hammer the map with a random
// insert/erase/find mix and mirror every operation into std::unordered_map.
// Any divergence in visibility or value is a probe-chain corruption.
TEST(IdMap, DifferentialVsUnorderedMap200k) {
    lob::IdMap<std::uint64_t> m(64); // small so clusters + growth both happen
    std::unordered_map<std::uint64_t, std::uint64_t> ref;
    std::mt19937_64 rng(7);

    for (int i = 0; i < 200'000; ++i) {
        const std::uint64_t r  = rng();
        const std::uint64_t op = r % 3;
        // Narrow key space (1..4095) so collisions and re-insertion of
        // recently erased keys are constant.
        const std::uint64_t k = 1 + (rng() % 4095);
        if (op == 0) {
            EXPECT_EQ(m.insert(k, i), ref.emplace(k, i).second) << "step " << i;
        } else if (op == 1) {
            EXPECT_EQ(m.erase(k), ref.erase(k) > 0) << "step " << i;
        } else {
            auto* p  = m.find(k);
            auto  it = ref.find(k);
            ASSERT_EQ(p != nullptr, it != ref.end()) << "step " << i;
            if (p) EXPECT_EQ(*p, it->second) << "step " << i;
        }
        ASSERT_EQ(m.size(), ref.size()) << "step " << i;
    }
}

} // namespace
