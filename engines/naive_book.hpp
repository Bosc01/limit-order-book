#pragma once
#include <list>
#include <map>

#include "lob/types.hpp"

namespace lob {

// Phase-1 baseline and the SEMANTIC REFERENCE for differential testing.
// DELIBERATELY naive mechanics:
//
//   * std::map price levels        -> red-black tree, pointer-chasing,
//                                     one heap node per level
//   * std::list orders per level   -> one heap allocation per resting order,
//                                     nodes scattered across the heap
//   * cancel/modify = linear scan  -> O(total resting orders)
//     of the entire book
//
// The matching semantics are exactly what every optimized engine must
// reproduce: price-time priority, partial fills, GTC/IOC/FOK, and the modify
// rule (qty decrease at same price keeps queue position; anything else is
// cancel + fresh submit). Only the mechanics are slow.
//
// Not implemented here (checked only in the real engines): duplicate-id
// rejection (needs an id index the naive book deliberately lacks) and
// self-trade prevention (owner is accepted but ignored; differential tests
// run with Stp::None).
template <class Listener = NullListener>
class NaiveBookT {
public:
    // market_protection_ticks > 0 caps how far through the book a market
    // order may sweep past the touch (CME market-with-protection). 0 = off.
    explicit NaiveBookT(Price market_protection_ticks = 0)
        : protection_(market_protection_ticks) {}

    Listener& listener() { return listener_; }

    SubmitResult submit_limit(OrderId id, Side side, Price px, Qty qty,
                              Tif tif = Tif::GTC, Owner = 0) {
        if (qty == 0 || px <= 0) return {0, false, true};
        if (tif == Tif::FOK && crossable_qty(side, px) < qty)
            return {0, false, true};

        SubmitResult r;
        r.filled =
            (side == Side::Bid)
                ? match(asks_, id, qty, [px](Price b) { return b <= px; })
                : match(bids_, id, qty, [px](Price b) { return b >= px; });
        const Qty rem = qty - r.filled;
        if (rem > 0 && tif == Tif::GTC) {
            rest(side, id, px, rem);
            r.rested = true;
        }
        return r;
    }

    // Market = "fill what you can, discard the rest" (IOC by nature). With
    // protection armed, "what you can" stops protection_ ticks past the
    // touch; the remainder is discarded (a venue may instead rest it at the
    // protection price — documented simplification).
    Qty submit_market(OrderId id, Side side, Qty qty, Owner = 0) {
        if (qty == 0) return 0;
        if (protection_ > 0) {
            const Price ref = (side == Side::Bid) ? best_ask() : best_bid();
            if (ref == kNoPrice) return 0; // no reference price: reject
            const Price lim =
                (side == Side::Bid) ? ref + protection_ : ref - protection_;
            return (side == Side::Bid)
                       ? match(asks_, id, qty, [lim](Price b) { return b <= lim; })
                       : match(bids_, id, qty, [lim](Price b) { return b >= lim; });
        }
        return (side == Side::Bid)
                   ? match(asks_, id, qty, [](Price) { return true; })
                   : match(bids_, id, qty, [](Price) { return true; });
    }

    bool cancel(OrderId id) {
        return erase_from(bids_, id) || erase_from(asks_, id);
    }

    // Exchange-standard modify semantics:
    //   * same price, quantity decrease -> amend in place, KEEP queue position
    //   * price change or quantity increase -> lose priority: remove, then
    //     re-enter as a brand-new GTC arrival (which may cross and fill)
    // `found` disambiguates "id unknown" from "bad amend args, order live".
    ModifyResult modify(OrderId id, Price new_px, Qty new_qty) {
        const bool bad_args = (new_qty == 0 || new_px <= 0);
        if (auto f = find(bids_, id); f.order) {
            if (bad_args) return {.ok = false, .filled = 0, .rested = false, .found = true};
            return modify_found(bids_, Side::Bid, f, id, new_px, new_qty);
        }
        if (auto f = find(asks_, id); f.order) {
            if (bad_args) return {.ok = false, .filled = 0, .rested = false, .found = true};
            return modify_found(asks_, Side::Ask, f, id, new_px, new_qty);
        }
        return {};
    }

    Price best_bid() const { return bids_.empty() ? kNoPrice : bids_.begin()->first; }
    Price best_ask() const { return asks_.empty() ? kNoPrice : asks_.begin()->first; }

    std::uint64_t qty_at(Side side, Price px) const {
        std::uint64_t n = 0;
        auto sum = [&](const auto& book) {
            if (auto it = book.find(px); it != book.end())
                for (const auto& o : it->second) n += o.qty;
        };
        if (side == Side::Bid) sum(bids_); else sum(asks_);
        return n;
    }

    std::size_t resting_orders() const {
        std::size_t n = 0;
        for (const auto& [px, lvl] : bids_) n += lvl.size();
        for (const auto& [px, lvl] : asks_) n += lvl.size();
        return n;
    }

    const char* check_invariants() const {
        if (!bids_.empty() && !asks_.empty() &&
            bids_.begin()->first >= asks_.begin()->first)
            return "book is crossed";
        for (const auto& [px, lvl] : bids_)
            if (lvl.empty()) return "empty level left in ladder";
        for (const auto& [px, lvl] : asks_)
            if (lvl.empty()) return "empty level left in ladder";
        return nullptr;
    }

private:
    struct RestingOrder {
        OrderId id;
        Qty     qty;
    };
    using Level = std::list<RestingOrder>;

    // Comparators chosen so begin() is always the best price on both sides:
    // highest bid, lowest ask. One rule, no special cases downstream.
    using Bids = std::map<Price, Level, std::greater<Price>>;
    using Asks = std::map<Price, Level, std::less<Price>>;

    // Liquidity reachable by a limit order at px, for the FOK pre-check.
    std::uint64_t crossable_qty(Side side, Price px) const {
        std::uint64_t n = 0;
        auto walk = [&](const auto& book, auto crosses) {
            for (const auto& [lvl_px, lvl] : book) {
                if (!crosses(lvl_px)) break;
                for (const auto& o : lvl) n += o.qty;
            }
        };
        if (side == Side::Bid)
            walk(asks_, [px](Price b) { return b <= px; });
        else
            walk(bids_, [px](Price b) { return b >= px; });
        return n;
    }

    // Walk the opposite side from best price outward while `crosses` holds,
    // consuming orders front-to-back within each level (FIFO = time priority).
    template <class BookSide, class Crosses>
    Qty match(BookSide& opposite, OrderId taker, Qty want, Crosses crosses) {
        Qty filled = 0;
        while (filled < want && !opposite.empty()) {
            const auto it = opposite.begin();
            if (!crosses(it->first)) break;
            Level& lvl = it->second;
            while (filled < want && !lvl.empty()) {
                RestingOrder& top = lvl.front();
                const Qty take = std::min(top.qty, want - filled);
                top.qty -= take;
                filled += take;
                listener_.on_trade(taker, top.id, it->first, take);
                if (top.qty == 0) lvl.pop_front();
            }
            if (lvl.empty()) opposite.erase(it);
        }
        return filled;
    }

    void rest(Side side, OrderId id, Price px, Qty qty) {
        if (side == Side::Bid) bids_[px].push_back({id, qty});
        else                   asks_[px].push_back({id, qty});
    }

    struct Found {
        RestingOrder* order = nullptr;
        Price         px    = 0;
    };

    template <class BookSide>
    Found find(BookSide& side, OrderId id) {
        for (auto& [px, lvl] : side)
            for (auto& o : lvl)
                if (o.id == id) return {&o, px};
        return {};
    }

    template <class BookSide>
    ModifyResult modify_found(BookSide& book, Side side, Found f,
                              OrderId id, Price new_px, Qty new_qty) {
        if (new_px == f.px && new_qty <= f.order->qty) {
            f.order->qty = new_qty; // in place: queue position preserved
            return {true, 0, true, true};
        }
        erase_from(book, id);
        const SubmitResult r = submit_limit(id, side, new_px, new_qty, Tif::GTC);
        return {true, r.filled, r.rested, true};
    }

    // The naive part: no index from id -> order, so this walks every level
    // and every order until it finds the id (or walks the WHOLE book to
    // conclude a miss — and misses are common, because cancels race fills).
    template <class BookSide>
    static bool erase_from(BookSide& side, OrderId id) {
        for (auto lit = side.begin(); lit != side.end(); ++lit) {
            Level& lvl = lit->second;
            for (auto oit = lvl.begin(); oit != lvl.end(); ++oit) {
                if (oit->id == id) {
                    lvl.erase(oit);
                    if (lvl.empty()) side.erase(lit);
                    return true;
                }
            }
        }
        return false;
    }

    Bids bids_;
    Asks asks_;
    Price protection_ = 0;
    [[no_unique_address]] Listener listener_;
};

using NaiveBook = NaiveBookT<>;

} // namespace lob
