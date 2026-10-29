// Property tests: random operation sequences against the final engine, with
// every property checked after EVERY operation. This is the C++ stand-in for
// Hypothesis: a seeded generator instead of strategies, many seeds instead of
// examples, and the seed plus operation index in each failure message instead
// of automatic shrinking.
//
// The test keeps its own model of the book (each resting order's side, price,
// remaining quantity and owner) plus a ledger of quantity in and out, updated
// only from what the engine reports: return values and the trade and cancel
// tape. Every check compares the engine against that model.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/order_book.hpp"
#include "test_util.hpp"

using lob::kNoPrice;
using lob::OrderId;
using lob::Owner;
using lob::Price;
using lob::Qty;
using lob::Side;
using lob::Stp;
using lob::Tif;

namespace {

using Book = lob::OrderBookT<lobtest::Recorder>;

// Narrow band so a full structural audit after every operation stays cheap,
// and a dense price range so crosses, sweeps and emptied levels are common.
constexpr Price kBandLo = 1, kBandHi = 257;
constexpr Price kPxLo = 100, kPxSpan = 40;
constexpr int   kSeeds = 25, kOpsPerSeed = 4000;

Book make_book(Stp stp) { return Book(stp, 1u << 12, kBandLo, kBandHi, 1u << 12); }

struct Op {
    enum Kind { Limit, Market, Cancel, Modify } kind;
    OrderId id;
    Side    side;
    Price   px;
    Qty     qty;
    Tif     tif;
    Owner   owner;
};

// Mix per 100 ops: 45 limit, 10 market, 30 cancel, 15 modify. About 2% of
// prices are out of band and 2% of quantities are zero, so rejection paths
// run inside the same sequences. Cancel and modify target recent ids: some
// live, some already gone, some never issued.
class OpGen {
public:
    explicit OpGen(std::uint64_t seed) : rng_(seed) {}

    Op next() {
        const std::uint64_t r = pick(100);
        if (r < 45) return limit();
        if (r < 55) return {Op::Market, next_id_++, side(), 0, Qty(1 + pick(60)), Tif::IOC, owner()};
        if (r < 85) return {Op::Cancel, target(), Side::Bid, 0, 0, Tif::GTC, 0};
        return {Op::Modify, target(), Side::Bid, price(), qty(), Tif::GTC, 0};
    }

private:
    std::uint64_t pick(std::uint64_t n) { return rng_() % n; }
    Side  side()  { return pick(2) ? Side::Ask : Side::Bid; }
    Owner owner() { return Owner(pick(4)); } // 0 is the STP-exempt owner
    Price price() { return pick(50) == 0 ? kBandHi + 40 : kPxLo + Price(pick(kPxSpan)); }
    Qty   qty()   { return pick(50) == 0 ? 0 : Qty(1 + pick(50)); }

    Op limit() {
        const std::uint64_t t = pick(10);
        const Tif tif = t < 7 ? Tif::GTC : t < 9 ? Tif::IOC : Tif::FOK;
        return {Op::Limit, next_id_++, side(), price(), qty(), tif, owner()};
    }

    OrderId target() {
        const OrderId span = next_id_ < 64 ? next_id_ : 64;
        return next_id_ - pick(span); // may equal next_id_: never issued
    }

    std::mt19937_64 rng_;
    OrderId         next_id_ = 1;
};

bool in_band(Price px) { return px >= kBandLo && px < kBandHi; }

struct Resting {
    Side  side;
    Price px;
    Qty   qty;
    Owner owner;
    std::uint64_t seq; // arrival order; time priority within a price
};

struct Model {
    std::unordered_map<OrderId, Resting> orders;
    std::uint64_t arrivals = 0;
};

// Proof that the random sequences exercised what the properties are about.
struct Coverage {
    std::uint64_t trades = 0, live_cancels = 0, live_modifies = 0, stp_kills = 0;
};

// Apply one operation to the engine and the model, checking the engine's
// answers along the way.
//
// Each order's quantity is accounted for against the input, not against
// what the engine says about itself: op.qty must split into the reported
// fill, the quantity that appeared on the engine's book at the order's
// price, and a discarded remainder the test works out from the order type.
// A cancel or modify must take exactly the model's quantity off the book.
void step(Book& b, Model& m, Coverage& cov, const Op& op, Stp stp) {
    auto& tape = b.listener();
    tape.trades.clear();
    tape.cancels.clear();

    Side  taker_side  = op.side;
    Owner taker_owner = op.owner;
    Qty   filled = 0;
    bool  rested = false;
    bool  gtc    = false; // the remainder rests unless STP stops the order
    bool  arrives = true; // false for a rejected order: nothing may happen
    // Depth at the taker's own price before the call, minus the order itself
    // when a modify leaves it at that price.
    std::uint64_t before = 0;
    // Arrival number the order keeps if it rests; 0 means it arrives now.
    std::uint64_t seq = 0;

    switch (op.kind) {
    case Op::Limit: {
        before = b.qty_at(op.side, op.px);
        const auto r = b.submit_limit(op.id, op.side, op.px, op.qty, op.tif, op.owner);
        arrives = op.qty > 0 && in_band(op.px);
        if (!arrives) { ASSERT_TRUE(r.rejected); }
        if (r.rejected) { ASSERT_EQ(r.filled, 0u); }
        if (r.rested) { ASSERT_EQ(op.tif, Tif::GTC) << "IOC or FOK remainder rested"; }
        if (op.tif == Tif::FOK) {
            ASSERT_TRUE(r.filled == 0 || r.filled == op.qty) << "FOK partially filled";
        }
        filled = r.filled;
        rested = r.rested;
        gtc    = op.tif == Tif::GTC;
        break;
    }
    case Op::Market:
        filled = b.submit_market(op.id, op.side, op.qty, op.owner);
        arrives = op.qty > 0;
        break;
    case Op::Cancel: {
        const auto it   = m.orders.find(op.id);
        const bool live = it != m.orders.end();
        const std::uint64_t at = live ? b.qty_at(it->second.side, it->second.px) : 0;
        ASSERT_EQ(b.cancel(op.id), live);
        if (live) {
            ASSERT_EQ(at - b.qty_at(it->second.side, it->second.px), it->second.qty)
                << "cancel took the wrong quantity off the book";
            m.orders.erase(it);
            ++cov.live_cancels;
        }
        ASSERT_TRUE(tape.trades.empty() && tape.cancels.empty());
        return;
    }
    case Op::Modify: {
        const auto it   = m.orders.find(op.id);
        const bool live = it != m.orders.end();
        const Resting old = live ? it->second : Resting{};
        std::uint64_t at_old = 0;
        if (live) {
            at_old = b.qty_at(old.side, old.px);
            before = b.qty_at(old.side, op.px) - (old.px == op.px ? old.qty : 0);
        }
        const auto r = b.modify(op.id, op.px, op.qty);
        ASSERT_EQ(r.found, live);
        ASSERT_EQ(r.ok, live && op.qty > 0 && in_band(op.px));
        if (!r.ok) {
            ASSERT_TRUE(tape.trades.empty() && tape.cancels.empty());
            return;
        }
        if (old.px != op.px) {
            ASSERT_EQ(at_old - b.qty_at(old.side, old.px), old.qty)
                << "modify took the wrong quantity off the old price";
        }
        // The order leaves at its old size and, if it rests, re-enters at
        // the new one; an in-place size-down nets out the same way.
        taker_side  = old.side;
        taker_owner = old.owner;
        // Same price and no larger: amended in place, priority kept. Any
        // other change is a fresh arrival.
        if (old.px == op.px && op.qty <= old.qty) seq = old.seq;
        m.orders.erase(it);
        ++cov.live_modifies;
        filled = r.filled;
        rested = r.rested;
        gtc    = true;
        break;
    }
    }

    // RejectIncoming may stop a GTC order at its own resting order and
    // discard the remainder. The model says whether such an order is in
    // reach; which comes first in priority it cannot say, so then the test
    // accepts either a full split or a discarded remainder.
    bool stop_possible = false;
    if (gtc && stp == Stp::RejectIncoming && taker_owner != 0) {
        for (const auto& [id, o] : m.orders) {
            const bool crosses = taker_side == Side::Bid ? o.px <= op.px : o.px >= op.px;
            if (o.side != taker_side && o.owner == taker_owner && crosses) stop_possible = true;
        }
    }

    for (const auto& c : tape.cancels) {
        const auto it = m.orders.find(c.id);
        ASSERT_NE(it, m.orders.end()) << "STP removed an order that was not resting";
        ASSERT_EQ(stp, Stp::CancelResting);
        ASSERT_NE(taker_owner, 0u) << "owner 0 must be exempt from STP";
        ASSERT_EQ(it->second.owner, taker_owner);
        m.orders.erase(it);
        ++cov.stp_kills;
    }

    std::uint64_t tape_qty = 0;
    for (const auto& t : tape.trades) {
        const auto it = m.orders.find(t.maker);
        ASSERT_NE(it, m.orders.end()) << "trade against an order that was not resting";
        Resting& maker = it->second;
        ASSERT_EQ(t.taker, op.id);
        ASSERT_NE(maker.side, taker_side) << "maker on the taker's own side";
        ASSERT_EQ(t.px, maker.px) << "trade not at the resting order's price";
        ASSERT_LE(t.qty, maker.qty);
        for (const auto& [oid, o] : m.orders) {
            if (o.side == maker.side && o.px == maker.px) {
                ASSERT_GE(o.seq, maker.seq) << "trade skipped an earlier order at its price";
            }
        }
        if (stp != Stp::None && taker_owner != 0) {
            ASSERT_NE(maker.owner, taker_owner) << "self-trade with STP armed";
        }
        tape_qty += t.qty;
        maker.qty -= t.qty;
        if (maker.qty == 0) m.orders.erase(it);
        ++cov.trades;
    }
    // What the engine says was filled is exactly what the tape says traded.
    ASSERT_EQ(tape_qty, filled);

    // Quantity in: op.qty = filled + rested + discarded. Rested is measured
    // on the book at the order's price; discarded follows from the type: all
    // of a rejected order, the remainder of an IOC, FOK or market order, and
    // nothing of a GTC order unless STP stopped it.
    if (!arrives) { ASSERT_EQ(filled, 0u); }
    const std::uint64_t on_book =
        op.kind == Op::Market || !in_band(op.px) ? 0 : b.qty_at(taker_side, op.px) - before;
    ASSERT_EQ(rested, on_book > 0) << "rested flag disagrees with the book";
    if (!gtc || !arrives) {
        ASSERT_EQ(on_book, 0u) << "a non-GTC or rejected order left quantity on the book";
    } else if (stop_possible) {
        ASSERT_TRUE(filled + on_book == op.qty || on_book == 0)
            << "GTC order neither rested its remainder nor was stopped";
    } else {
        ASSERT_EQ(filled + on_book, std::uint64_t(op.qty))
            << "GTC quantity not accounted for: filled " << filled << ", on book " << on_book;
    }

    if (rested) {
        ASSERT_LT(filled, op.qty);
        m.orders[op.id] = {taker_side, op.px, Qty(on_book), taker_owner,
                           seq != 0 ? seq : ++m.arrivals};
    }
}

// After every operation: the structure is sound, the book is not crossed,
// the cached best prices are right, every price level holds exactly the
// model's quantity.
void audit(const Book& b, const Model& m) {
    ASSERT_EQ(b.check_invariants(), nullptr);

    std::uint64_t depth[2][kBandHi] = {};
    Price best_bid = kNoPrice, best_ask = kNoPrice;
    for (const auto& [id, o] : m.orders) {
        depth[int(o.side)][o.px] += o.qty;
        if (o.side == Side::Bid) best_bid = std::max(best_bid, o.px);
        else best_ask = (best_ask == kNoPrice) ? o.px : std::min(best_ask, o.px);
    }
    ASSERT_EQ(b.best_bid(), best_bid);
    ASSERT_EQ(b.best_ask(), best_ask);
    if (b.best_bid() != kNoPrice && b.best_ask() != kNoPrice) {
        ASSERT_LT(b.best_bid(), b.best_ask()) << "book is crossed or locked";
    }

    for (Price px = kBandLo; px < kBandHi; ++px)
        for (const Side s : {Side::Bid, Side::Ask}) {
            ASSERT_EQ(b.qty_at(s, px), depth[int(s)][px]) << "depth at " << px;
        }
    ASSERT_EQ(b.resting_orders(), m.orders.size());
    // No ledger check here. Once every level matches the model, the book's
    // total equals the model's by construction, so a check of that total
    // against entered - traded - removed could not fail on its own; it was
    // removed. Conservation is checked per operation in step(), against
    // each order's own quantity.
}

void run_checked(std::uint64_t seed, Stp stp, Coverage& cov) {
    Book  b = make_book(stp);
    Model m;
    OpGen gen(seed);
    for (int i = 0; i < kOpsPerSeed; ++i) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed << ", op " << i);
        step(b, m, cov, gen.next(), stp);
        if (::testing::Test::HasFatalFailure()) return;
        audit(b, m);
        if (::testing::Test::HasFatalFailure()) return;
    }
}

// Everything one run produced, flattened: each operation's result and its
// full trade and cancel tape.
std::vector<std::uint64_t> transcript(std::uint64_t seed, Stp stp) {
    Book  b = make_book(stp);
    OpGen gen(seed);
    std::vector<std::uint64_t> out;
    auto& tape = b.listener();
    for (int i = 0; i < kOpsPerSeed; ++i) {
        const Op op = gen.next();
        tape.trades.clear();
        tape.cancels.clear();
        switch (op.kind) {
        case Op::Limit: {
            const auto r = b.submit_limit(op.id, op.side, op.px, op.qty, op.tif, op.owner);
            out.insert(out.end(), {r.filled, r.rested, r.rejected});
            break;
        }
        case Op::Market:
            out.push_back(b.submit_market(op.id, op.side, op.qty, op.owner));
            break;
        case Op::Cancel:
            out.push_back(b.cancel(op.id));
            break;
        case Op::Modify: {
            const auto r = b.modify(op.id, op.px, op.qty);
            out.insert(out.end(), {r.ok, r.filled, r.rested, r.found});
            break;
        }
        }
        for (const auto& t : tape.trades)
            out.insert(out.end(), {t.taker, t.maker, std::uint64_t(t.px), t.qty});
        for (const auto& c : tape.cancels)
            out.insert(out.end(), {c.id, c.owner});
    }
    return out;
}

class Property : public ::testing::TestWithParam<Stp> {};

TEST_P(Property, NeverCrossedAndQuantityConserved) {
    Coverage cov;
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        run_checked(seed, GetParam(), cov);
        if (HasFatalFailure()) return;
    }
    EXPECT_GT(cov.trades, 0u);
    EXPECT_GT(cov.live_cancels, 0u);
    EXPECT_GT(cov.live_modifies, 0u);
    if (GetParam() == Stp::CancelResting) { EXPECT_GT(cov.stp_kills, 0u); }
}

TEST_P(Property, SameInputSameTrades) {
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        const auto first  = transcript(seed, GetParam());
        const auto second = transcript(seed, GetParam());
        ASSERT_EQ(first, second) << "seed " << seed;
    }
}

INSTANTIATE_TEST_SUITE_P(StpPolicy, Property,
    ::testing::Values(Stp::None, Stp::CancelResting, Stp::RejectIncoming),
    [](const ::testing::TestParamInfo<Stp>& info) -> std::string {
        switch (info.param) {
        case Stp::None:          return "None";
        case Stp::CancelResting: return "CancelResting";
        default:                 return "RejectIncoming";
        }
    });

} // namespace
