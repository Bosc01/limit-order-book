// Differential testing: replay the exact benchmark workload through the
// naive reference and the engine under test, comparing EVERY op result and
// the full trade tape, with periodic structural audits and a final
// level-by-level book comparison. This is the strongest correctness tool in
// the project: any semantic drift an optimization introduces shows up as a
// divergence within a few thousand ops, pinned to the exact op index.
#include <gtest/gtest.h>

#include <cstdint>

#include "engines/naive_book.hpp"
#include "engines/book_v1.hpp"
#include "engines/book_v2.hpp"
#include "engines/book_v3.hpp"
#include "engines/book_v4.hpp"
#include "engines/book_v5.hpp"
#include "lob/order_book.hpp"
#include "test_util.hpp"
#include "workload.hpp"

using lob::Side;
using lobtest::Recorder;

namespace {

template <class RefBook, class TestBook>
void run_differential_books(RefBook& ref, TestBook& tst, std::uint64_t seed,
                            std::size_t n_ops) {
    bench::WorkloadGen gen(seed);
    const auto ops = gen.generate(n_ops);

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const bench::Op& op = ops[i];
        ref.listener().trades.clear();
        tst.listener().trades.clear();

        switch (op.kind) {
            case bench::OpKind::Limit: {
                const auto a = ref.submit_limit(op.id, op.side, op.price, op.qty, op.tif);
                const auto b = tst.submit_limit(op.id, op.side, op.price, op.qty, op.tif);
                ASSERT_EQ(a.filled, b.filled)     << "op " << i;
                ASSERT_EQ(a.rested, b.rested)     << "op " << i;
                ASSERT_EQ(a.rejected, b.rejected) << "op " << i;
                break;
            }
            case bench::OpKind::Market:
                ASSERT_EQ(ref.submit_market(op.id, op.side, op.qty),
                          tst.submit_market(op.id, op.side, op.qty)) << "op " << i;
                break;
            case bench::OpKind::Cancel:
                ASSERT_EQ(ref.cancel(op.id), tst.cancel(op.id)) << "op " << i;
                break;
            case bench::OpKind::Modify: {
                const auto a = ref.modify(op.id, op.price, op.qty);
                const auto b = tst.modify(op.id, op.price, op.qty);
                ASSERT_EQ(a.ok, b.ok)         << "op " << i;
                ASSERT_EQ(a.filled, b.filled) << "op " << i;
                ASSERT_EQ(a.rested, b.rested) << "op " << i;
                ASSERT_EQ(a.found, b.found)   << "op " << i;
                break;
            }
        }
        // The trade tape must match exactly: same makers, same order, same
        // prices, same quantities. This is where price-time priority bugs
        // surface.
        ASSERT_EQ(ref.listener().trades, tst.listener().trades) << "op " << i;

        if ((i & 4095) == 0) {
            ASSERT_EQ(tst.check_invariants(), nullptr) << "op " << i;
            ASSERT_EQ(ref.best_bid(), tst.best_bid())  << "op " << i;
            ASSERT_EQ(ref.best_ask(), tst.best_ask())  << "op " << i;
            ASSERT_EQ(ref.resting_orders(), tst.resting_orders()) << "op " << i;
        }
    }

    // Final: compare aggregate depth on every price the workload can touch.
    ASSERT_EQ(tst.check_invariants(), nullptr);
    for (lob::Price px = 8'990; px <= 11'010; ++px) {
        ASSERT_EQ(ref.qty_at(Side::Bid, px), tst.qty_at(Side::Bid, px)) << px;
        ASSERT_EQ(ref.qty_at(Side::Ask, px), tst.qty_at(Side::Ask, px)) << px;
    }
    ASSERT_EQ(ref.resting_orders(), tst.resting_orders());
}

template <class RefBook, class TestBook>
void run_differential(std::uint64_t seed, std::size_t n_ops) {
    RefBook  ref;
    TestBook tst;
    run_differential_books(ref, tst, seed, n_ops);
}

} // namespace

using Naive = lob::NaiveBookT<Recorder>;

TEST(Differential, NaiveVsV1_Seed42_200k) {
    run_differential<Naive, lob::v1::BookT<Recorder>>(42, 200'000);
}

TEST(Differential, NaiveVsV1_Seed1337_100k) {
    run_differential<Naive, lob::v1::BookT<Recorder>>(1337, 100'000);
}

TEST(Differential, NaiveVsV2_Seed42_200k) {
    run_differential<Naive, lob::v2::BookT<Recorder>>(42, 200'000);
}

TEST(Differential, NaiveVsV3_Seed42_200k) {
    run_differential<Naive, lob::v3::BookT<Recorder>>(42, 200'000);
}

TEST(Differential, NaiveVsV4_Seed42_200k) {
    run_differential<Naive, lob::v4::BookT<Recorder>>(42, 200'000);
}

TEST(Differential, NaiveVsV5_Seed42_200k) {
    run_differential<Naive, lob::v5::BookT<Recorder>>(42, 200'000);
}

TEST(Differential, NaiveVsFinal_Seed42_200k) {
    run_differential<Naive, lob::OrderBookT<Recorder>>(42, 200'000);
}

TEST(Differential, NaiveVsFinal_Seed1337_100k) {
    run_differential<Naive, lob::OrderBookT<Recorder>>(1337, 100'000);
}

// The fast pair: v1 (map-based, heap-allocating) vs final (flat everything)
// at 1M ops -- deeper coverage than the naive pairs can afford.
TEST(Differential, V1VsFinal_Seed7_1M) {
    run_differential<lob::v1::BookT<Recorder>, lob::OrderBookT<Recorder>>(7, 1'000'000);
}

// Market-with-protection armed on BOTH books: the clamp must bite
// identically, including the "no reference price" reject.
TEST(Differential, NaiveVsFinal_Protection3_Seed42_200k) {
    lob::NaiveBookT<Recorder> ref(/*protection=*/3);
    lob::OrderBookT<Recorder> tst(lob::Stp::None, 1u << 17, 1, lob::Price{1} << 15,
                                  std::size_t{1} << 18, /*protection=*/3);
    run_differential_books(ref, tst, 42, 200'000);
}
