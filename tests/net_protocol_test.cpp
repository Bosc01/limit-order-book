// Wire-protocol tests: round-trips, hostile input, and re-framing under
// adversarial TCP chunking. No sockets involved -- that is the point of
// keeping the parser pure.
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "net/framing.hpp"
#include "net/protocol.hpp"

using namespace lobnet;

namespace {

TEST(Protocol, NewOrderRoundTrip) {
    std::uint8_t buf[128];
    const NewOrderMsg in{77, 10'001, 25, lob::Side::Ask, lob::Tif::IOC};
    const auto n = encode(buf, sizeof(buf), in);
    ASSERT_GT(n, 0u);
    const auto out = decode_new_order(buf + kHeaderLen, n - kHeaderLen);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, 77u);
    EXPECT_EQ(out->px, 10'001);
    EXPECT_EQ(out->qty, 25u);
    EXPECT_EQ(out->side, lob::Side::Ask);
    EXPECT_EQ(out->tif, lob::Tif::IOC);
}

TEST(Protocol, AllTypesRoundTrip) {
    std::uint8_t buf[512];
    {
        const auto n = encode(buf, sizeof(buf), CancelMsg{5});
        ASSERT_GT(n, 0u);
        ASSERT_TRUE(decode_cancel(buf + kHeaderLen, n - kHeaderLen).has_value());
    }
    {
        const auto n = encode(buf, sizeof(buf), ModifyMsg{5, 99, 10});
        const auto m = decode_modify(buf + kHeaderLen, n - kHeaderLen);
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->new_px, 99);
    }
    {
        const auto n = encode(buf, sizeof(buf), MarketMsg{6, 12, lob::Side::Bid});
        ASSERT_TRUE(decode_market(buf + kHeaderLen, n - kHeaderLen).has_value());
    }
    {
        const auto n = encode(buf, sizeof(buf),
                              ExecReportMsg{9, 4, ExecStatus::PartialRest});
        const auto m = decode_exec_report(buf + kHeaderLen, n - kHeaderLen);
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->status, ExecStatus::PartialRest);
    }
    {
        const auto n = encode(buf, sizeof(buf), TradeMsg{1, 100, 5, lob::Side::Bid});
        ASSERT_TRUE(decode_trade(buf + kHeaderLen, n - kHeaderLen).has_value());
    }
    {
        BookMsg b{};
        b.seq = 3;
        b.bid_px[0] = 100;
        b.bid_qty[0] = 55;
        const auto n = encode(buf, sizeof(buf), b);
        const auto m = decode_book(buf + kHeaderLen, n - kHeaderLen);
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->bid_qty[0], 55u);
    }
}

TEST(Protocol, RejectsBadEnumValues) {
    std::uint8_t buf[128];
    const auto n = encode(buf, sizeof(buf),
                          NewOrderMsg{1, 100, 5, lob::Side::Bid, lob::Tif::GTC});
    ASSERT_GT(n, 0u);
    buf[n - 2] = 9; // stomp the side byte
    EXPECT_FALSE(decode_new_order(buf + kHeaderLen, n - kHeaderLen).has_value());
    buf[n - 2] = 0;
    buf[n - 1] = 7; // stomp the tif byte
    EXPECT_FALSE(decode_new_order(buf + kHeaderLen, n - kHeaderLen).has_value());
}

TEST(Protocol, RejectsTruncatedAndOversizedBodies) {
    std::uint8_t buf[128];
    const auto n = encode(buf, sizeof(buf),
                          NewOrderMsg{1, 100, 5, lob::Side::Bid, lob::Tif::GTC});
    ASSERT_GT(n, 0u);
    // Truncated: every prefix of the body must fail, not crash.
    for (std::size_t len = 1; len < n - kHeaderLen; ++len)
        EXPECT_FALSE(decode_new_order(buf + kHeaderLen, len).has_value()) << len;
    // Trailing garbage must fail too (exact-length discipline).
    std::uint8_t big[130];
    std::memcpy(big, buf, n);
    big[n] = 0xAB;
    EXPECT_FALSE(decode_new_order(big + kHeaderLen, n - kHeaderLen + 1).has_value());
}

TEST(Framing, ReassemblesArbitraryChunking) {
    // Build a stream of valid frames, then feed it in every chunk size from
    // 1 byte to the whole stream: the parser must always produce the same
    // frame sequence.
    std::vector<std::uint8_t> stream;
    std::uint8_t buf[128];
    for (std::uint64_t id = 1; id <= 20; ++id) {
        const auto n = encode(buf, sizeof(buf),
                              NewOrderMsg{id, lob::Price(100 + id),
                                          lob::Qty(id), lob::Side::Bid,
                                          lob::Tif::GTC});
        stream.insert(stream.end(), buf, buf + n);
    }

    for (std::size_t chunk = 1; chunk <= stream.size(); ++chunk) {
        FrameBuffer fb;
        std::vector<std::uint64_t> ids;
        for (std::size_t off = 0; off < stream.size(); off += chunk) {
            const std::size_t n = std::min(chunk, stream.size() - off);
            ASSERT_TRUE(fb.feed(stream.data() + off, n,
                [&](const std::uint8_t* body, std::size_t len) {
                    const auto m = decode_new_order(body, len);
                    ASSERT_TRUE(m.has_value());
                    ids.push_back(m->id);
                })) << "chunk size " << chunk;
        }
        ASSERT_EQ(ids.size(), 20u) << "chunk size " << chunk;
        for (std::uint64_t id = 1; id <= 20; ++id) EXPECT_EQ(ids[id - 1], id);
    }
}

TEST(Framing, PoisonsOnOversizedLength) {
    FrameBuffer fb;
    // length prefix 0xFFFF: hostile
    const std::uint8_t evil[] = {0xFF, 0xFF, 0x01};
    int frames = 0;
    EXPECT_FALSE(fb.feed(evil, sizeof(evil),
                         [&](const std::uint8_t*, std::size_t) { ++frames; }));
    EXPECT_EQ(frames, 0);
    // Poisoned forever, even for a now-valid frame.
    std::uint8_t buf[64];
    const auto n = encode(buf, sizeof(buf), CancelMsg{1});
    EXPECT_FALSE(fb.feed(buf, n, [&](const std::uint8_t*, std::size_t) { ++frames; }));
    EXPECT_EQ(frames, 0);
}

TEST(Framing, PoisonsOnZeroLength) {
    FrameBuffer fb;
    const std::uint8_t evil[] = {0x00, 0x00};
    EXPECT_FALSE(fb.feed(evil, sizeof(evil),
                         [](const std::uint8_t*, std::size_t) {}));
}

TEST(Framing, RandomGarbageNeverCrashes) {
    // 100 random garbage streams: the parser may reject (usually will) but
    // must never crash, hang, or emit an oversized frame. ASan/UBSan builds
    // make this a real memory-safety test.
    std::mt19937_64 rng(99);
    for (int round = 0; round < 100; ++round) {
        FrameBuffer fb;
        bool alive = true;
        for (int chunk = 0; chunk < 50 && alive; ++chunk) {
            std::uint8_t junk[257];
            const std::size_t n = 1 + rng() % 256;
            for (std::size_t i = 0; i < n; ++i) junk[i] = std::uint8_t(rng());
            alive = fb.feed(junk, n, [](const std::uint8_t* body, std::size_t len) {
                ASSERT_GE(len, 1u);
                ASSERT_LE(len, kMaxFrameLen);
                (void)body;
            });
        }
    }
}

} // namespace
