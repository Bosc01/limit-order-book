// Behavior specific to the flat banded ladder (v5 and the final engine) and
// to the final engine's flat id map: band enforcement, bitmap scans across
// word boundaries, reserved id 0, and the zero-allocation steady state.
#include <gtest/gtest.h>

#include "engines/book_v5.hpp"
#include "lob/order_book.hpp"
#include "test_util.hpp"

using lob::kNoPrice;
using lob::Side;
using lob::Stp;
using lobtest::Recorder;

template <class T>
class FlatEngineTest : public ::testing::Test {};

using FlatEngineTypes =
    ::testing::Types<lob::v5::BookT<Recorder>, lob::OrderBookT<Recorder>>;
TYPED_TEST_SUITE(FlatEngineTest, FlatEngineTypes);

TYPED_TEST(FlatEngineTest, OutOfBandPriceRejected) {
    TypeParam b; // default band [1, 32768)
    EXPECT_TRUE(b.submit_limit(1, Side::Bid, 40'000, 5).rejected);
    EXPECT_TRUE(b.submit_limit(2, Side::Bid, 32'768, 5).rejected); // hi is exclusive
    EXPECT_TRUE(b.submit_limit(3, Side::Ask, 0, 5).rejected);
    EXPECT_FALSE(b.submit_limit(4, Side::Ask, 32'767, 5).rejected); // last valid tick
    EXPECT_EQ(b.resting_orders(), 1u);
}

TYPED_TEST(FlatEngineTest, ModifyToOutOfBandPriceRejectedInPlace) {
    TypeParam b;
    b.submit_limit(1, Side::Bid, 100, 5);
    auto m = b.modify(1, 50'000, 5);
    EXPECT_FALSE(m.ok);
    EXPECT_EQ(b.qty_at(Side::Bid, 100), 5u); // order untouched
}

TYPED_TEST(FlatEngineTest, BestBidRescanCrossesManyBitmapWords) {
    TypeParam b;
    // Two bids ~310 words apart in the bitmap. Canceling the better one
    // forces the scan to walk hundreds of empty words downward.
    b.submit_limit(1, Side::Bid, 100, 5);
    b.submit_limit(2, Side::Bid, 20'000, 5);
    EXPECT_EQ(b.best_bid(), 20'000);
    b.cancel(2);
    EXPECT_EQ(b.best_bid(), 100);
    b.cancel(1);
    EXPECT_EQ(b.best_bid(), kNoPrice);
}

TYPED_TEST(FlatEngineTest, BestAskRescanCrossesManyBitmapWords) {
    TypeParam b;
    b.submit_limit(1, Side::Ask, 20'000, 5);
    b.submit_limit(2, Side::Ask, 100, 5);
    EXPECT_EQ(b.best_ask(), 100);
    b.cancel(2);
    EXPECT_EQ(b.best_ask(), 20'000);
    b.cancel(1);
    EXPECT_EQ(b.best_ask(), kNoPrice);
}

TYPED_TEST(FlatEngineTest, SweepAcrossWordBoundaryLevels) {
    TypeParam b;
    // Levels at 60..67 straddle the bit-63/bit-0 boundary of words 0 and 1
    // (band starts at 1, so tick t maps to bit t-1).
    for (lob::OrderId i = 0; i < 8; ++i)
        b.submit_limit(100 + i, Side::Ask, 60 + lob::Price(i), 1);
    auto r = b.submit_limit(200, Side::Bid, 67, 8);
    EXPECT_EQ(r.filled, 8u);
    EXPECT_EQ(b.best_ask(), kNoPrice);
    ASSERT_EQ(b.listener().trades.size(), 8u);
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(b.listener().trades[size_t(i)].px, 60 + i); // ascending sweep
}

TYPED_TEST(FlatEngineTest, CustomBandIsHonored) {
    TypeParam b(Stp::None, 1u << 10, /*band_lo=*/500, /*band_hi=*/600);
    EXPECT_TRUE(b.submit_limit(1, Side::Bid, 499, 5).rejected);
    EXPECT_FALSE(b.submit_limit(2, Side::Bid, 500, 5).rejected);
    EXPECT_FALSE(b.submit_limit(3, Side::Ask, 599, 5).rejected);
    EXPECT_TRUE(b.submit_limit(4, Side::Ask, 600, 5).rejected);
}

TEST(FinalEngine, IdZeroReservedAndRejected) {
    lob::OrderBookT<Recorder> b;
    auto r = b.submit_limit(0, Side::Bid, 100, 5);
    EXPECT_TRUE(r.rejected);
    EXPECT_EQ(b.resting_orders(), 0u);
    EXPECT_FALSE(b.cancel(0));
}

TEST(FinalEngine, SteadyStateNeverGrows) {
    // Pool and id map sized for the flow -> zero growth events. This is the
    // unit-test twin of the benchmark's "0 allocations per op" claim.
    lob::OrderBookT<Recorder> b(Stp::None, /*pool=*/1024, 1, 1 << 15,
                                /*id slots=*/4096);
    lob::OrderId next = 1;
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 50; ++i)
            b.submit_limit(next++, i % 2 ? Side::Bid : Side::Ask,
                           10'000 + (i % 7) - 3, 10);
        for (lob::OrderId id = next - 50; id < next; ++id) b.cancel(id);
    }
    EXPECT_EQ(b.pool_grow_count(), 0u);
    EXPECT_EQ(b.id_map_grow_count(), 0u);
    EXPECT_EQ(b.check_invariants(), nullptr);
}
