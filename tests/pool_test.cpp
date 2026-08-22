#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "lob/pool.hpp"

namespace {

struct Blob {
    std::uint64_t a;
    std::uint64_t b;
};

struct alignas(64) AlignedBlob {
    std::uint64_t a;
};

TEST(Pool, CreateReturnsDistinctConstructedObjects) {
    lob::Pool<Blob> p(8);
    std::set<Blob*> seen;
    for (std::uint64_t i = 0; i < 8; ++i) {
        Blob* b = p.create(i, i * 2);
        EXPECT_EQ(b->a, i);
        EXPECT_EQ(b->b, i * 2);
        EXPECT_TRUE(seen.insert(b).second) << "duplicate slot handed out";
    }
    EXPECT_EQ(p.grow_count(), 0u);
}

TEST(Pool, DestroyedSlotsAreReused) {
    lob::Pool<Blob> p(4);
    Blob* a = p.create(1ull, 1ull);
    p.destroy(a);
    Blob* b = p.create(2ull, 2ull); // LIFO free list: same slot comes back
    EXPECT_EQ(static_cast<void*>(a), static_cast<void*>(b));
}

TEST(Pool, GrowsWhenExhaustedAndCountsIt) {
    lob::Pool<Blob> p(2);
    std::vector<Blob*> all;
    for (std::uint64_t i = 0; i < 100; ++i) all.push_back(p.create(i, i));
    EXPECT_GE(p.capacity(), 100u);
    EXPECT_GT(p.grow_count(), 0u);
    // Slots must remain valid across growth (old slabs are never moved).
    for (std::uint64_t i = 0; i < 100; ++i) EXPECT_EQ(all[i]->a, i);
}

TEST(Pool, RespectsOveralignedTypes) {
    lob::Pool<AlignedBlob> p(16);
    for (int i = 0; i < 32; ++i) {
        AlignedBlob* b = p.create(std::uint64_t(i));
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b) % 64, 0u)
            << "slot not 64-byte aligned";
    }
}

TEST(Pool, ChurnReusesBoundedMemory) {
    lob::Pool<Blob> p(1024);
    std::vector<Blob*> live;
    for (std::uint64_t round = 0; round < 50; ++round) {
        for (std::uint64_t i = 0; i < 1000; ++i)
            live.push_back(p.create(round, i));
        for (Blob* b : live) p.destroy(b);
        live.clear();
    }
    // 50k create/destroy cycles with <=1000 live must never grow a
    // 1024-capacity pool.
    EXPECT_EQ(p.grow_count(), 0u);
    EXPECT_EQ(p.capacity(), 1024u);
}

} // namespace
