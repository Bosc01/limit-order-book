#pragma once
#include <vector>

#include "lob/types.hpp"

namespace lobtest {

struct Trade {
    lob::OrderId taker;
    lob::OrderId maker;
    lob::Price   px;
    lob::Qty     qty;
    bool operator==(const Trade&) const = default;
};

struct Cancel {
    lob::OrderId id;
    lob::Owner   owner;
    bool operator==(const Cancel&) const = default;
};

// Listener that records the trade tape so tests can assert on exact
// maker/taker/price/qty sequences -- including that trades print at the
// RESTING order's price (price improvement goes to the taker) -- and the
// unsolicited-cancel tape (STP CancelResting kills).
struct Recorder {
    std::vector<Trade>  trades;
    std::vector<Cancel> cancels;
    void on_trade(lob::OrderId taker, lob::OrderId maker, lob::Price px,
                  lob::Qty qty) {
        trades.push_back({taker, maker, px, qty});
    }
    void on_cancel(lob::OrderId id, lob::Owner owner) {
        cancels.push_back({id, owner});
    }
};

} // namespace lobtest
