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
using Owner   = std::uint32_t; // participant id, used for self-trade prevention

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
};

} // namespace lob
