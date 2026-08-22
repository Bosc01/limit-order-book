// Features the naive reference deliberately lacks: duplicate-id rejection
// and self-trade prevention. Typed across every indexed engine (v1 through
// the final OrderBook), because STP corner cases are exactly the kind of
// semantics an optimization rewrite silently breaks.
#include <gtest/gtest.h>

#include "engines/book_v1.hpp"
#include "engines/book_v2.hpp"
#include "engines/book_v3.hpp"
#include "engines/book_v4.hpp"
#include "engines/book_v5.hpp"
#include "lob/order_book.hpp"
#include "test_util.hpp"

using lob::Side;
using lob::Stp;
using lob::Tif;
using lobtest::Recorder;

template <class T>
class IndexedEngineTest : public ::testing::Test {};

using IndexedEngineTypes = ::testing::Types<
    lob::v1::BookT<Recorder>,
    lob::v2::BookT<Recorder>,
    lob::v3::BookT<Recorder>,
    lob::v4::BookT<Recorder>,
    lob::v5::BookT<Recorder>,
    lob::OrderBookT<Recorder>>;
TYPED_TEST_SUITE(IndexedEngineTest, IndexedEngineTypes);

// --- id lifecycle -----------------------------------------------------------

TYPED_TEST(IndexedEngineTest, IdLifecycle_DuplicateRestingIdRejected) {
    TypeParam b;
    ASSERT_TRUE(b.submit_limit(1, Side::Bid, 100, 5).rested);
    auto r = b.submit_limit(1, Side::Bid, 99, 5);
    EXPECT_TRUE(r.rejected);
    EXPECT_EQ(b.qty_at(Side::Bid, 99), 0u);
    EXPECT_EQ(b.qty_at(Side::Bid, 100), 5u); // original untouched
}

TYPED_TEST(IndexedEngineTest, IdLifecycle_IdReusableAfterCancel) {
    TypeParam b;
    b.submit_limit(1, Side::Bid, 100, 5);
    b.cancel(1);
    EXPECT_TRUE(b.submit_limit(1, Side::Bid, 101, 5).rested);
}

TYPED_TEST(IndexedEngineTest, IdLifecycle_IdReusableAfterFullFill) {
    TypeParam b;
    b.submit_limit(1, Side::Ask, 100, 5);
    b.submit_limit(2, Side::Bid, 100, 5); // fills id 1 away
    EXPECT_TRUE(b.submit_limit(1, Side::Ask, 100, 5).rested);
}

// --- self-trade prevention --------------------------------------------------

TYPED_TEST(IndexedEngineTest, SelfTrade_AllowedUnderPolicyNone) {
    TypeParam b(Stp::None);
    b.submit_limit(1, Side::Ask, 100, 5, Tif::GTC, /*owner=*/7);
    auto r = b.submit_limit(2, Side::Bid, 100, 5, Tif::GTC, /*owner=*/7);
    EXPECT_EQ(r.filled, 5u); // self-trade allowed by default
}

TYPED_TEST(IndexedEngineTest, SelfTrade_CancelRestingKnocksOutOwnOrder) {
    TypeParam b(Stp::CancelResting);
    b.submit_limit(1, Side::Ask, 100, 5, Tif::GTC, 7);
    auto r = b.submit_limit(2, Side::Bid, 100, 5, Tif::GTC, 7);
    EXPECT_EQ(r.filled, 0u);                  // no self-fill
    EXPECT_TRUE(r.rested);                    // incoming rests normally
    EXPECT_TRUE(b.listener().trades.empty());
    EXPECT_EQ(b.qty_at(Side::Ask, 100), 0u);  // own resting order canceled
    EXPECT_EQ(b.best_bid(), 100);
    EXPECT_FALSE(b.cancel(1));                // it is really gone
}

TYPED_TEST(IndexedEngineTest, SelfTrade_CancelRestingContinuesToOtherMakers) {
    TypeParam b(Stp::CancelResting);
    b.submit_limit(1, Side::Ask, 100, 5, Tif::GTC, 7); // own, first in queue
    b.submit_limit(2, Side::Ask, 100, 5, Tif::GTC, 8); // someone else behind
    auto r = b.submit_limit(3, Side::Bid, 100, 5, Tif::GTC, 7);
    EXPECT_EQ(r.filled, 5u);
    ASSERT_EQ(b.listener().trades.size(), 1u);
    EXPECT_EQ(b.listener().trades[0].maker, 2u); // filled the other guy
    EXPECT_EQ(b.qty_at(Side::Ask, 100), 0u);     // own order canceled, not filled
}

TYPED_TEST(IndexedEngineTest, SelfTrade_RejectIncomingWithZeroFillIsRejected) {
    TypeParam b(Stp::RejectIncoming);
    b.submit_limit(1, Side::Ask, 100, 5, Tif::GTC, 7);
    auto r = b.submit_limit(2, Side::Bid, 100, 5, Tif::GTC, 7);
    EXPECT_TRUE(r.rejected);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_FALSE(r.rested);
    EXPECT_EQ(b.qty_at(Side::Ask, 100), 5u); // resting order untouched
}

TYPED_TEST(IndexedEngineTest, SelfTrade_RejectIncomingAfterPartialFillKeepsFills) {
    TypeParam b(Stp::RejectIncoming);
    b.submit_limit(1, Side::Ask, 100, 3, Tif::GTC, 8); // other participant first
    b.submit_limit(2, Side::Ask, 100, 5, Tif::GTC, 7); // own order behind
    auto r = b.submit_limit(3, Side::Bid, 100, 8, Tif::GTC, 7);
    EXPECT_EQ(r.filled, 3u);      // fill against the other participant stands
    EXPECT_FALSE(r.rejected);     // not a pure reject: something executed
    EXPECT_FALSE(r.rested);       // but the remainder dies, never rests
    EXPECT_EQ(b.qty_at(Side::Ask, 100), 5u); // own order untouched
    EXPECT_EQ(b.best_bid(), lob::kNoPrice);
}

TYPED_TEST(IndexedEngineTest, SelfTrade_FokCancelRestingExcludesOwnQtyFromLiquidity) {
    TypeParam b(Stp::CancelResting);
    b.submit_limit(1, Side::Ask, 100, 5, Tif::GTC, 7); // own qty doesn't count
    b.submit_limit(2, Side::Ask, 100, 3, Tif::GTC, 8);
    auto r = b.submit_limit(3, Side::Bid, 100, 5, Tif::FOK, 7);
    EXPECT_TRUE(r.rejected);                  // only 3 reachable, needs 5
    EXPECT_EQ(b.qty_at(Side::Ask, 100), 8u);  // pre-check must not mutate
    EXPECT_TRUE(b.listener().trades.empty());
}

TYPED_TEST(IndexedEngineTest, SelfTrade_FokRejectIncomingBlockedByOwnOrderInPath) {
    TypeParam b(Stp::RejectIncoming);
    b.submit_limit(1, Side::Ask, 100, 5, Tif::GTC, 7); // own order at the front
    b.submit_limit(2, Side::Ask, 100, 9, Tif::GTC, 8);
    auto r = b.submit_limit(3, Side::Bid, 100, 9, Tif::FOK, 7);
    EXPECT_TRUE(r.rejected);
    EXPECT_EQ(b.qty_at(Side::Ask, 100), 14u); // untouched
}

TYPED_TEST(IndexedEngineTest, SelfTrade_FokRejectIncomingPassesWhenOthersSuffice) {
    TypeParam b(Stp::RejectIncoming);
    b.submit_limit(1, Side::Ask, 100, 5, Tif::GTC, 8); // enough before own order
    b.submit_limit(2, Side::Ask, 101, 5, Tif::GTC, 7); // own, but never reached
    auto r = b.submit_limit(3, Side::Bid, 100, 5, Tif::FOK, 7);
    EXPECT_EQ(r.filled, 5u);
    EXPECT_FALSE(r.rejected);
    EXPECT_EQ(b.qty_at(Side::Ask, 101), 5u);
}

// --- structural audit --------------------------------------------------------

TYPED_TEST(IndexedEngineTest, Invariants_CleanAfterScriptedFlow) {
    TypeParam b;
    b.submit_limit(1, Side::Bid, 100, 50);
    b.submit_limit(2, Side::Bid, 100, 30);
    b.submit_limit(3, Side::Bid, 99, 20);
    b.submit_limit(4, Side::Ask, 101, 40);
    b.submit_limit(5, Side::Ask, 100, 60);   // crosses both bids at 100
    EXPECT_EQ(b.check_invariants(), nullptr);
    b.modify(3, 98, 25);
    b.cancel(4);
    EXPECT_EQ(b.check_invariants(), nullptr);
    EXPECT_EQ(b.resting_orders(), 2u); // id 3 at 98, id 5 remainder? check:
    // id5: 60 against 50+30=80 -> fills 60, nothing rests. Left: id3 (25), and
    // id2 remainder 80-60=20 at 100.
    EXPECT_EQ(b.qty_at(Side::Bid, 100), 20u);
    EXPECT_EQ(b.qty_at(Side::Bid, 98), 25u);
}
