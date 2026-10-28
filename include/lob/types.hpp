#pragma once
#include <cstdint>

namespace lob {

// Prices are integer ticks; the tick size belongs to the gateway.
using Price   = std::int64_t;
using Qty     = std::uint32_t;
using OrderId = std::uint64_t;
// Self-trade prevention group. Owner 0 is ungrouped and exempt from STP,
// as with CME SMP and Nasdaq AIQ, which apply only within an assigned group.
// Otherwise two unrelated flows that both default the owner would block
// each other.
using Owner   = std::uint32_t;

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

enum class Tif : std::uint8_t {
    GTC, // rest any unfilled remainder on the book
    IOC, // fill what crosses immediately, discard the remainder
    FOK  // fill the full quantity immediately or do nothing at all
};

enum class Stp : std::uint8_t {
    None,          // allow self-trades (default; backtests often want this)
    CancelResting, // incoming order knocks out own resting order, keeps matching
    RejectIncoming // incoming order is rejected the moment it would self-match
};

// best_bid()/best_ask() on an empty side. Valid prices are positive.
inline constexpr Price kNoPrice = 0;

// filled < requested with rested == false is an IOC or FOK remainder, or,
// for a GTC order, a RejectIncoming STP stop after partial fills. The caller
// knows which TIF it sent, so the flags are unambiguous to it.
struct SubmitResult {
    Qty  filled   = 0;
    bool rested   = false; // remainder is now on the book
    bool rejected = false; // validation or FOK/STP rejection; nothing happened
};

struct ModifyResult {
    bool ok     = false; // order was found and modified
    Qty  filled = 0;     // reprice can cross and fill immediately
    bool rested = false;
    bool found  = false; // false: unknown id; true with !ok: bad amend, order live
};

// Event sink, passed as a template parameter so the calls inline: this one
// for the benchmark, a recorder in tests, a publisher in the gateway.
struct NullListener {
    void on_trade(OrderId /*taker*/, OrderId /*maker*/, Price /*px*/,
                  Qty /*qty*/) {}
    // A resting order removed without its owner asking (STP CancelResting).
    void on_cancel(OrderId /*id*/, Owner /*owner*/) {}
};

} // namespace lob
