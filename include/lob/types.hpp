#pragma once
#include <cstdint>

namespace lob {

// Prices are integer ticks, never floating point. FP prices are a classic
// interview trap: 0.1 + 0.2 != 0.3, and price comparison must be exact for
// matching to be deterministic. The tick size lives at the edge (feed/gateway),
// not in the engine.
using Price   = std::int64_t;
using Qty     = std::uint32_t;
using OrderId = std::uint64_t;
// Participant id for self-trade prevention. Owner 0 is reserved as "no
// owner": orders with owner 0 are EXEMPT from STP, matching venue practice
// (CME SMP / Nasdaq AIQ apply prevention only between orders that share an
// explicitly assigned group; ungrouped flow is exempt). Without this
// reservation, two unrelated flows that both default the owner would
// silently self-match-prevent each other.
using Owner   = std::uint32_t;

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

enum class Tif : std::uint8_t {
    GTC, // rest any unfilled remainder on the book
    IOC, // fill what crosses immediately, discard the remainder
    FOK  // fill the full quantity immediately or do nothing at all
};

// Self-trade prevention policy. Real venues offer these under various names
// (CME "self-match prevention", Nasdaq "anti-internalization").
enum class Stp : std::uint8_t {
    None,          // allow self-trades (default; backtests often want this)
    CancelResting, // incoming order knocks out own resting order, keeps matching
    RejectIncoming // incoming order is rejected the moment it would self-match
};

// "none" sentinel for best_bid()/best_ask() on an empty side. 0 is safe as a
// sentinel because valid prices are strictly positive (validated on entry).
inline constexpr Price kNoPrice = 0;

// Known tradeoff, on purpose: three booleans instead of a status enum. Two
// outcomes look alike from the flags alone — a GTC order partially filled
// then STP-stopped ({filled>0, rested=false}) reads like an IOC partial.
// The gateway's ExecStatus mapping is the disambiguation layer for clients;
// inside the engine the compact struct keeps every submit path branch-light.
// A production API would return an explicit outcome enum.
struct SubmitResult {
    Qty  filled   = 0;
    bool rested   = false; // remainder is now on the book
    bool rejected = false; // validation or FOK/STP rejection; nothing happened
};

struct ModifyResult {
    bool ok     = false; // order was found and modified
    Qty  filled = 0;     // reprice can cross and fill immediately
    bool rested = false;
    // Distinguishes "unknown id: the order is gone, stop working it" from
    // "amend arguments invalid: the order is untouched and still live".
    // Real venues send different reject reasons for exactly this reason.
    bool found  = false;
};

// Engines take a Listener as a template parameter (static polymorphism):
// the benchmark instantiates with NullListener and the calls compile to
// nothing; tests instantiate with a recorder and can assert on the exact
// trade tape; the gateway instantiates with a publisher. A virtual listener
// would put an indirect call inside the matching loop.
struct NullListener {
    void on_trade(OrderId /*taker*/, OrderId /*maker*/, Price /*px*/,
                  Qty /*qty*/) {}
    // Fired when the engine removes a resting order WITHOUT its owner asking
    // (today: STP CancelResting knocking out the resting order). Without
    // this event the order would just vanish — its owner, and any drop-copy
    // consumer, would still believe it is live.
    void on_cancel(OrderId /*id*/, Owner /*owner*/) {}
};

} // namespace lob
