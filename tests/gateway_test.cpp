// Gateway order ownership: each connection has its own order id space, and
// no client can cancel, amend, or detect another client's orders. Frames go
// straight into handle_frame(); exec reports are read back from the
// connection's outbox. No sockets are opened.
#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "net/gateway.hpp"

using namespace lobnet;
using lob::Side;
using lob::Tif;

namespace {

// Every exec report queued for `c` since the last call.
std::vector<ExecReportMsg> replies(Connection& c) {
    std::vector<ExecReportMsg> out;
    FrameBuffer fb;
    EXPECT_TRUE(fb.feed(c.outbox.data(), c.outbox.size(),
                        [&](const std::uint8_t* body, std::size_t len) {
                            const auto m = decode_exec_report(body, len);
                            ASSERT_TRUE(m.has_value());
                            out.push_back(*m);
                        }));
    c.outbox.clear();
    return out;
}

// Sends one frame from `c` and returns the single exec report it produces.
template <class Msg>
ExecReportMsg request(Engine& book, Connection& c, const Msg& msg) {
    std::uint8_t buf[128];
    const auto n = encode(buf, sizeof(buf), msg);
    handle_frame(book, book.listener(), c, buf + kHeaderLen, n - kHeaderLen);
    EXPECT_FALSE(c.dead);
    const auto r = replies(c);
    EXPECT_EQ(r.size(), 1u);
    return r.empty() ? ExecReportMsg{} : r.back();
}

TEST(GatewayOwnership, ForeignOrderAnswersUnknownAndStaysOnBook) {
    Engine book;
    Connection a{.owner = 1}, b{.owner = 2};

    EXPECT_EQ(request(book, b, NewOrderMsg{7, 100, 10, Side::Bid, Tif::GTC}).status,
              ExecStatus::Accepted);

    // Client A names B's live id 7: the same answer as for an id nobody uses.
    const auto cancel = request(book, a, CancelMsg{7});
    EXPECT_EQ(cancel.id, 7u);
    EXPECT_EQ(cancel.status, ExecStatus::Unknown);
    EXPECT_EQ(request(book, a, CancelMsg{8}).status, ExecStatus::Unknown);
    EXPECT_EQ(request(book, a, ModifyMsg{7, 100, 1}).status, ExecStatus::Unknown);
    EXPECT_EQ(request(book, a, ModifyMsg{7, 120, 10}).status, ExecStatus::Unknown);

    // B's order is untouched, in size and price, and B still controls it.
    EXPECT_EQ(book.resting_orders(), 1u);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.qty_at(Side::Bid, 100), 10u);
    EXPECT_EQ(request(book, b, CancelMsg{7}).status, ExecStatus::Canceled);
    EXPECT_EQ(book.resting_orders(), 0u);
}

TEST(GatewayOwnership, TwoClientsCanBothUseId1) {
    Engine book;
    Connection a{.owner = 1}, b{.owner = 2};

    // B's id 1 is not a duplicate of A's, so it is not rejected (which
    // would also tell B that A has an order 1 live).
    EXPECT_EQ(request(book, a, NewOrderMsg{1, 100, 10, Side::Bid, Tif::GTC}).status,
              ExecStatus::Accepted);
    EXPECT_EQ(request(book, b, NewOrderMsg{1, 105, 20, Side::Ask, Tif::GTC}).status,
              ExecStatus::Accepted);
    EXPECT_EQ(book.resting_orders(), 2u);

    // Each client's id 1 reaches that client's own order and nothing else.
    const auto mod = request(book, b, ModifyMsg{1, 104, 20});
    EXPECT_EQ(mod.id, 1u);
    EXPECT_EQ(mod.status, ExecStatus::Replaced);
    EXPECT_EQ(book.qty_at(Side::Ask, 104), 20u);
    EXPECT_EQ(book.qty_at(Side::Bid, 100), 10u);

    EXPECT_EQ(request(book, a, CancelMsg{1}).status, ExecStatus::Canceled);
    EXPECT_EQ(book.qty_at(Side::Bid, 100), 0u);
    EXPECT_EQ(book.qty_at(Side::Ask, 104), 20u);

    // Within one client's own id space a live id is still a duplicate.
    EXPECT_EQ(request(book, b, NewOrderMsg{1, 106, 5, Side::Ask, Tif::GTC}).status,
              ExecStatus::Rejected);
    EXPECT_EQ(request(book, b, MarketMsg{1, 5, Side::Bid}).status,
              ExecStatus::Canceled);
    EXPECT_EQ(book.qty_at(Side::Ask, 104), 20u);
}

TEST(GatewayOwnership, MappingLeavesWithTheOrder) {
    Engine book;
    FeedPublisher& feed = book.listener();
    Connection a{.owner = 1}, b{.owner = 2};
    // The id map holds exactly the orders on the book, after every step.
    auto in_sync = [&] { return feed.orders.size() == book.resting_orders(); };

    request(book, a, NewOrderMsg{5, 100, 10, Side::Ask, Tif::GTC});
    request(book, a, NewOrderMsg{6, 90, 10, Side::Bid, Tif::GTC});

    // Partial fill: A's 5 keeps resting with 6 open.
    EXPECT_EQ(request(book, b, NewOrderMsg{1, 100, 4, Side::Bid, Tif::IOC}).status,
              ExecStatus::Filled);
    EXPECT_TRUE(in_sync());
    EXPECT_EQ(book.qty_at(Side::Ask, 100), 6u);

    // Full fill of the remaining 6 by a market order: A's 5 is gone, and A
    // may use id 5 again.
    EXPECT_EQ(request(book, b, MarketMsg{2, 6, Side::Bid}).status, ExecStatus::Filled);
    EXPECT_TRUE(in_sync());
    EXPECT_EQ(request(book, a, CancelMsg{5}).status, ExecStatus::Unknown);
    EXPECT_EQ(request(book, a, NewOrderMsg{5, 110, 3, Side::Ask, Tif::GTC}).status,
              ExecStatus::Accepted);
    EXPECT_TRUE(in_sync());

    // IOC and FOK remainders never rest, so nothing is recorded for them.
    EXPECT_EQ(request(book, b, NewOrderMsg{3, 50, 5, Side::Bid, Tif::IOC}).status,
              ExecStatus::Canceled);
    EXPECT_EQ(request(book, b, NewOrderMsg{4, 110, 9, Side::Bid, Tif::FOK}).status,
              ExecStatus::Rejected);
    EXPECT_TRUE(in_sync());
    EXPECT_EQ(request(book, b, CancelMsg{3}).status, ExecStatus::Unknown);
    EXPECT_EQ(request(book, b, CancelMsg{4}).status, ExecStatus::Unknown);

    // A reprice that trades away completely leaves the book too.
    const auto mod = request(book, a, ModifyMsg{6, 110, 3});
    EXPECT_EQ(mod.status, ExecStatus::Replaced);
    EXPECT_EQ(mod.filled, 3u);
    EXPECT_TRUE(in_sync());
    EXPECT_EQ(request(book, a, CancelMsg{6}).status, ExecStatus::Unknown);

    // Cancel removes the last one.
    request(book, b, NewOrderMsg{9, 80, 1, Side::Bid, Tif::GTC});
    EXPECT_EQ(request(book, b, CancelMsg{9}).status, ExecStatus::Canceled);
    EXPECT_EQ(book.resting_orders(), 0u);
    EXPECT_TRUE(in_sync());
}

TEST(GatewayOwnership, StpCancelReportsTheClientId) {
    Engine book(lob::Stp::CancelResting);
    FeedPublisher& feed = book.listener();
    std::vector<std::pair<lob::Owner, lob::OrderId>> killed;
    feed.on_unsolicited_cancel = [&](lob::OrderId client, lob::Owner owner) {
        killed.emplace_back(owner, client);
    };
    Connection a{.owner = 1}, b{.owner = 2};

    // Both use id 1. A's order gets engine id 2 (B's took 1), so only a
    // translated report names it 1.
    request(book, b, NewOrderMsg{1, 100, 10, Side::Bid, Tif::GTC});
    request(book, a, NewOrderMsg{1, 101, 10, Side::Bid, Tif::GTC});

    // A's sell crosses A's own bid first: STP kills it and A hears about it
    // under its own id 1. The sell then trades with B's bid.
    EXPECT_EQ(request(book, a, NewOrderMsg{2, 100, 4, Side::Ask, Tif::GTC}).status,
              ExecStatus::Filled);
    ASSERT_EQ(killed.size(), 1u);
    EXPECT_EQ(killed[0], std::make_pair(lob::Owner{1}, lob::OrderId{1}));
    EXPECT_EQ(feed.orders.size(), book.resting_orders());

    // A's id 1 is gone; B's id 1 is live with 6 left.
    EXPECT_EQ(request(book, a, CancelMsg{1}).status, ExecStatus::Unknown);
    EXPECT_EQ(book.qty_at(Side::Bid, 100), 6u);
    EXPECT_EQ(request(book, b, CancelMsg{1}).status, ExecStatus::Canceled);
    EXPECT_EQ(book.resting_orders(), 0u);
}

} // namespace
