// Matching semantics shared by EVERY engine, naive included. These tests are
// the executable specification: when an optimized engine v_n lands, it joins
// EngineTypes and must pass this file unchanged.
#include <gtest/gtest.h>

#include "engines/naive_book.hpp"
#include "engines/book_v1.hpp"
#include "engines/book_v2.hpp"
#include "engines/book_v3.hpp"
#include "engines/book_v4.hpp"
#include "engines/book_v5.hpp"
#include "lob/order_book.hpp"
#include "test_util.hpp"

using lob::kNoPrice;
using lob::Side;
using lob::Tif;
using lobtest::Recorder;
using lobtest::Trade;

template <class T>
class EngineTest : public ::testing::Test {
protected:
    T book;
};

using EngineTypes = ::testing::Types<
    lob::NaiveBookT<Recorder>,
    lob::v1::BookT<Recorder>,
    lob::v2::BookT<Recorder>,
    lob::v3::BookT<Recorder>,
    lob::v4::BookT<Recorder>,
    lob::v5::BookT<Recorder>,
    lob::OrderBookT<Recorder>>;
TYPED_TEST_SUITE(EngineTest, EngineTypes);

// --- resting and best-price basics -----------------------------------------

TYPED_TEST(EngineTest, EmptyBookHasNoBest) {
    EXPECT_EQ(this->book.best_bid(), kNoPrice);
    EXPECT_EQ(this->book.best_ask(), kNoPrice);
}

TYPED_TEST(EngineTest, RestingBidSetsBestBid) {
    auto r = this->book.submit_limit(1, Side::Bid, 100, 10);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_TRUE(r.rested);
    EXPECT_FALSE(r.rejected);
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.qty_at(Side::Bid, 100), 10u);
}

TYPED_TEST(EngineTest, RestingAskSetsBestAsk) {
    auto r = this->book.submit_limit(1, Side::Ask, 101, 7);
    EXPECT_TRUE(r.rested);
    EXPECT_EQ(this->book.best_ask(), 101);
    EXPECT_EQ(this->book.qty_at(Side::Ask, 101), 7u);
}

TYPED_TEST(EngineTest, NonCrossingLimitsBothRest) {
    this->book.submit_limit(1, Side::Bid, 99, 5);
    this->book.submit_limit(2, Side::Ask, 101, 5);
    EXPECT_EQ(this->book.best_bid(), 99);
    EXPECT_EQ(this->book.best_ask(), 101);
    EXPECT_TRUE(this->book.listener().trades.empty());
}

TYPED_TEST(EngineTest, BestBidIsHighestBid) {
    this->book.submit_limit(1, Side::Bid, 98, 5);
    this->book.submit_limit(2, Side::Bid, 100, 5);
    this->book.submit_limit(3, Side::Bid, 99, 5);
    EXPECT_EQ(this->book.best_bid(), 100);
}

TYPED_TEST(EngineTest, BestAskIsLowestAsk) {
    this->book.submit_limit(1, Side::Ask, 103, 5);
    this->book.submit_limit(2, Side::Ask, 101, 5);
    this->book.submit_limit(3, Side::Ask, 102, 5);
    EXPECT_EQ(this->book.best_ask(), 101);
}

// --- crossing, fills, price-time priority ----------------------------------

TYPED_TEST(EngineTest, CrossingLimitTradesAtRestingPrice) {
    this->book.submit_limit(1, Side::Ask, 100, 10);
    auto r = this->book.submit_limit(2, Side::Bid, 105, 10); // through the ask
    EXPECT_EQ(r.filled, 10u);
    EXPECT_FALSE(r.rested);
    // Price improvement goes to the taker: the trade prints at 100, not 105.
    ASSERT_EQ(this->book.listener().trades.size(), 1u);
    EXPECT_EQ(this->book.listener().trades[0], (Trade{2, 1, 100, 10}));
    EXPECT_EQ(this->book.best_ask(), kNoPrice);
    EXPECT_EQ(this->book.best_bid(), kNoPrice);
}

TYPED_TEST(EngineTest, FifoWithinLevel) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    this->book.submit_limit(2, Side::Ask, 100, 5);
    this->book.submit_limit(3, Side::Bid, 100, 5);
    ASSERT_EQ(this->book.listener().trades.size(), 1u);
    EXPECT_EQ(this->book.listener().trades[0].maker, 1u); // older fills first
    EXPECT_EQ(this->book.qty_at(Side::Ask, 100), 5u);     // id 2 remains
}

TYPED_TEST(EngineTest, PriceBeatsTime) {
    this->book.submit_limit(1, Side::Ask, 101, 5); // earlier but worse price
    this->book.submit_limit(2, Side::Ask, 100, 5); // later but better price
    this->book.submit_limit(3, Side::Bid, 101, 10);
    ASSERT_EQ(this->book.listener().trades.size(), 2u);
    EXPECT_EQ(this->book.listener().trades[0], (Trade{3, 2, 100, 5}));
    EXPECT_EQ(this->book.listener().trades[1], (Trade{3, 1, 101, 5}));
}

TYPED_TEST(EngineTest, PartialFillLeavesMakerRemainder) {
    this->book.submit_limit(1, Side::Ask, 100, 10);
    auto r = this->book.submit_limit(2, Side::Bid, 100, 4);
    EXPECT_EQ(r.filled, 4u);
    EXPECT_EQ(this->book.qty_at(Side::Ask, 100), 6u);
}

TYPED_TEST(EngineTest, TakerRemainderRests) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    auto r = this->book.submit_limit(2, Side::Bid, 100, 8);
    EXPECT_EQ(r.filled, 5u);
    EXPECT_TRUE(r.rested);
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.qty_at(Side::Bid, 100), 3u);
}

TYPED_TEST(EngineTest, MultiLevelSweepBestPriceFirst) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    this->book.submit_limit(2, Side::Ask, 101, 5);
    auto r = this->book.submit_limit(3, Side::Bid, 101, 10);
    EXPECT_EQ(r.filled, 10u);
    ASSERT_EQ(this->book.listener().trades.size(), 2u);
    EXPECT_EQ(this->book.listener().trades[0].px, 100);
    EXPECT_EQ(this->book.listener().trades[1].px, 101);
    EXPECT_EQ(this->book.best_ask(), kNoPrice);
}

TYPED_TEST(EngineTest, SweepStopsAtLimitPrice) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    this->book.submit_limit(2, Side::Ask, 102, 5);
    auto r = this->book.submit_limit(3, Side::Bid, 101, 10);
    EXPECT_EQ(r.filled, 5u);        // only the 100 level crosses
    EXPECT_TRUE(r.rested);          // remainder rests at 101
    EXPECT_EQ(this->book.best_bid(), 101);
    EXPECT_EQ(this->book.best_ask(), 102);
}

// --- time in force ----------------------------------------------------------

TYPED_TEST(EngineTest, IocFillsThenDiscardsRemainder) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    auto r = this->book.submit_limit(2, Side::Bid, 100, 8, Tif::IOC);
    EXPECT_EQ(r.filled, 5u);
    EXPECT_FALSE(r.rested);
    EXPECT_EQ(this->book.best_bid(), kNoPrice);
}

TYPED_TEST(EngineTest, IocWithNoCrossDoesNothing) {
    this->book.submit_limit(1, Side::Ask, 101, 5);
    auto r = this->book.submit_limit(2, Side::Bid, 100, 5, Tif::IOC);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_FALSE(r.rested);
    EXPECT_FALSE(r.rejected);
    EXPECT_EQ(this->book.qty_at(Side::Ask, 101), 5u);
    EXPECT_EQ(this->book.best_bid(), kNoPrice);
}

TYPED_TEST(EngineTest, FokRejectsWhenInsufficientLiquidity) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    auto r = this->book.submit_limit(2, Side::Bid, 100, 6, Tif::FOK);
    EXPECT_TRUE(r.rejected);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(this->book.qty_at(Side::Ask, 100), 5u); // book untouched
    EXPECT_TRUE(this->book.listener().trades.empty());
}

TYPED_TEST(EngineTest, FokFillsWhenExactlyEnough) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    auto r = this->book.submit_limit(2, Side::Bid, 100, 5, Tif::FOK);
    EXPECT_EQ(r.filled, 5u);
    EXPECT_FALSE(r.rejected);
    EXPECT_EQ(this->book.best_ask(), kNoPrice);
}

TYPED_TEST(EngineTest, FokFillsAcrossLevels) {
    this->book.submit_limit(1, Side::Ask, 100, 3);
    this->book.submit_limit(2, Side::Ask, 101, 3);
    auto r = this->book.submit_limit(3, Side::Bid, 101, 6, Tif::FOK);
    EXPECT_EQ(r.filled, 6u);
    EXPECT_EQ(this->book.best_ask(), kNoPrice);
}

TYPED_TEST(EngineTest, FokRespectsLimitPrice) {
    this->book.submit_limit(1, Side::Ask, 100, 3);
    this->book.submit_limit(2, Side::Ask, 102, 3); // beyond the limit
    auto r = this->book.submit_limit(3, Side::Bid, 101, 6, Tif::FOK);
    EXPECT_TRUE(r.rejected);
    EXPECT_EQ(this->book.qty_at(Side::Ask, 100), 3u);
    EXPECT_EQ(this->book.qty_at(Side::Ask, 102), 3u);
}

// --- market orders ----------------------------------------------------------

TYPED_TEST(EngineTest, MarketSweepsBestFirst) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    this->book.submit_limit(2, Side::Ask, 101, 5);
    auto filled = this->book.submit_market(9, Side::Bid, 7);
    EXPECT_EQ(filled, 7u);
    EXPECT_EQ(this->book.qty_at(Side::Ask, 101), 3u);
    ASSERT_EQ(this->book.listener().trades.size(), 2u);
    EXPECT_EQ(this->book.listener().trades[0].px, 100);
}

TYPED_TEST(EngineTest, MarketOnEmptyBookFillsNothing) {
    EXPECT_EQ(this->book.submit_market(9, Side::Bid, 5), 0u);
    EXPECT_EQ(this->book.best_bid(), kNoPrice);
}

TYPED_TEST(EngineTest, MarketPartialOnThinBookNeverRests) {
    this->book.submit_limit(1, Side::Ask, 100, 3);
    EXPECT_EQ(this->book.submit_market(9, Side::Bid, 10), 3u);
    EXPECT_EQ(this->book.best_ask(), kNoPrice);
    EXPECT_EQ(this->book.best_bid(), kNoPrice); // remainder discarded
}

// --- cancel -----------------------------------------------------------------

TYPED_TEST(EngineTest, CancelRemovesOrder) {
    this->book.submit_limit(1, Side::Bid, 100, 5);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_EQ(this->book.best_bid(), kNoPrice);
    EXPECT_EQ(this->book.resting_orders(), 0u);
}

TYPED_TEST(EngineTest, CancelUnknownIdFails) {
    EXPECT_FALSE(this->book.cancel(99));
}

TYPED_TEST(EngineTest, CancelPartiallyFilledOrderRemovesRemainder) {
    this->book.submit_limit(1, Side::Ask, 100, 10);
    this->book.submit_limit(2, Side::Bid, 100, 4); // 6 left on id 1
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_EQ(this->book.best_ask(), kNoPrice);
}

TYPED_TEST(EngineTest, CancelTwiceSecondFails) {
    this->book.submit_limit(1, Side::Bid, 100, 5);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_FALSE(this->book.cancel(1));
}

TYPED_TEST(EngineTest, CancelLastOrderAtLevelPromotesNextBest) {
    this->book.submit_limit(1, Side::Bid, 100, 5);
    this->book.submit_limit(2, Side::Bid, 99, 5);
    this->book.cancel(1);
    EXPECT_EQ(this->book.best_bid(), 99);
}

TYPED_TEST(EngineTest, CancelFullyFilledOrderFails) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    this->book.submit_limit(2, Side::Bid, 100, 5); // fills id 1 completely
    EXPECT_FALSE(this->book.cancel(1));            // it is gone
}

// --- modify -----------------------------------------------------------------

TYPED_TEST(EngineTest, ModifyDecreaseKeepsQueuePosition) {
    this->book.submit_limit(1, Side::Ask, 100, 10);
    this->book.submit_limit(2, Side::Ask, 100, 10);
    auto m = this->book.modify(1, 100, 4); // size down in place
    EXPECT_TRUE(m.ok);
    EXPECT_EQ(this->book.qty_at(Side::Ask, 100), 14u);
    this->book.submit_limit(3, Side::Bid, 100, 4);
    ASSERT_EQ(this->book.listener().trades.size(), 1u);
    EXPECT_EQ(this->book.listener().trades[0].maker, 1u); // still first
}

TYPED_TEST(EngineTest, ModifyIncreaseLosesQueuePosition) {
    this->book.submit_limit(1, Side::Ask, 100, 5);
    this->book.submit_limit(2, Side::Ask, 100, 5);
    auto m = this->book.modify(1, 100, 8); // size UP -> back of the queue
    EXPECT_TRUE(m.ok);
    this->book.submit_limit(3, Side::Bid, 100, 5);
    ASSERT_EQ(this->book.listener().trades.size(), 1u);
    EXPECT_EQ(this->book.listener().trades[0].maker, 2u); // id 2 now first
}

TYPED_TEST(EngineTest, ModifyRepriceMovesOrder) {
    this->book.submit_limit(1, Side::Bid, 100, 5);
    auto m = this->book.modify(1, 99, 5);
    EXPECT_TRUE(m.ok);
    EXPECT_EQ(this->book.qty_at(Side::Bid, 100), 0u);
    EXPECT_EQ(this->book.qty_at(Side::Bid, 99), 5u);
}

TYPED_TEST(EngineTest, ModifyRepriceCanCrossAndFill) {
    this->book.submit_limit(2, Side::Ask, 101, 5);
    this->book.submit_limit(1, Side::Bid, 100, 5);
    auto m = this->book.modify(1, 101, 5); // reprice bid into the ask
    EXPECT_TRUE(m.ok);
    EXPECT_EQ(m.filled, 5u);
    ASSERT_EQ(this->book.listener().trades.size(), 1u);
    EXPECT_EQ(this->book.listener().trades[0], (Trade{1, 2, 101, 5}));
    EXPECT_EQ(this->book.resting_orders(), 0u);
}

TYPED_TEST(EngineTest, ModifyUnknownIdFails) {
    auto m = this->book.modify(42, 100, 5);
    EXPECT_FALSE(m.ok);
    EXPECT_FALSE(m.found); // the id is gone: stop working the order
}

TYPED_TEST(EngineTest, ModifyToZeroQtyIsInvalidButOrderLives) {
    this->book.submit_limit(1, Side::Bid, 100, 5);
    auto m = this->book.modify(1, 100, 0);
    EXPECT_FALSE(m.ok);
    EXPECT_TRUE(m.found); // amend rejected, order still live on the book
    EXPECT_EQ(this->book.qty_at(Side::Bid, 100), 5u); // unchanged
}

// --- validation and aggregates ----------------------------------------------

TYPED_TEST(EngineTest, ZeroQtyLimitRejected) {
    auto r = this->book.submit_limit(1, Side::Bid, 100, 0);
    EXPECT_TRUE(r.rejected);
    EXPECT_EQ(this->book.resting_orders(), 0u);
}

TYPED_TEST(EngineTest, NonPositivePriceRejected) {
    EXPECT_TRUE(this->book.submit_limit(1, Side::Bid, 0, 5).rejected);
    EXPECT_TRUE(this->book.submit_limit(2, Side::Bid, -5, 5).rejected);
}

TYPED_TEST(EngineTest, QtyAtAggregatesAcrossOrders) {
    this->book.submit_limit(1, Side::Bid, 100, 5);
    this->book.submit_limit(2, Side::Bid, 100, 7);
    EXPECT_EQ(this->book.qty_at(Side::Bid, 100), 12u);
}

TYPED_TEST(EngineTest, LevelTotalsSurviveU32Overflow) {
    // Two orders whose sum exceeds uint32: aggregate must be 64-bit.
    const lob::Qty big = 4'000'000'000u;
    this->book.submit_limit(1, Side::Ask, 100, big);
    this->book.submit_limit(2, Side::Ask, 100, big);
    EXPECT_EQ(this->book.qty_at(Side::Ask, 100), 8'000'000'000ull);
}

TYPED_TEST(EngineTest, MixedFlowKeepsInvariants) {
    auto& b = this->book;
    int step = 0;
    auto ok = [&](const char* what) {
        const char* err = b.check_invariants();
        EXPECT_EQ(err, nullptr) << "step " << step << " (" << what << "): " << err;
        ++step;
        if (b.best_bid() != kNoPrice && b.best_ask() != kNoPrice)
            EXPECT_LT(b.best_bid(), b.best_ask());
    };
    b.submit_limit(1, Side::Bid, 100, 50);          ok("bid rests");
    b.submit_limit(2, Side::Ask, 102, 30);          ok("ask rests");
    b.submit_limit(3, Side::Ask, 101, 20);          ok("better ask");
    b.submit_limit(4, Side::Bid, 101, 25);          ok("cross fills 20, rests 5");
    b.modify(1, 100, 10);                           ok("size down");
    b.modify(2, 103, 30);                           ok("reprice away");
    b.submit_market(5, Side::Bid, 10);              ok("market eats ask");
    b.cancel(4);                                    ok("cancel");
    b.submit_limit(6, Side::Ask, 100, 100);         ok("ask through bid fills, rests");
    b.submit_limit(7, Side::Bid, 89, 5, Tif::FOK);  ok("fok below market rejected");
    b.submit_limit(8, Side::Ask, 89, 5, Tif::IOC);  ok("ioc with empty bids");
    b.cancel(1);                                    ok("cancel already-filled id misses");
}
