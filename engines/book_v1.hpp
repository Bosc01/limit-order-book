#pragma once
#include <cstdint>
#include <map>
#include <unordered_map>

#include "lob/types.hpp"

namespace lob::v1 {

// Engine v1 — the target architecture, correct-first, not yet fast.
//
// Three layered structures:
//   1. Price ladder: std::map per side (bids sorted descending, asks
//      ascending) so begin() is always the best price. O(log L) inserts,
//      O(1) best access. Replaced by a flat array in v5.
//   2. Orders within a level: intrusive doubly-linked FIFO. Head = oldest =
//      first to fill (time priority). Intrusive means the links live INSIDE
//      the Order, so there is no separate list-node allocation and an
//      Order* is enough to unlink in O(1).
//   3. Id index: unordered_map<OrderId, Order*> making cancel/modify O(1)
//      lookups instead of the naive full-book scan.
//
// Still deliberately unoptimized: every order is a heap new/delete (pool
// arrives in v2), struct layout is untuned (v3), the match loop branches on
// side via templates but the ladder walk still chases map nodes (v5).
template <class Listener = NullListener>
class BookT {
public:
    explicit BookT(Stp stp = Stp::None) : stp_(stp) {}
    ~BookT() {
        free_all(bids_);
        free_all(asks_);
    }
    BookT(const BookT&)            = delete;
    BookT& operator=(const BookT&) = delete;

    Listener& listener() { return listener_; }

    SubmitResult submit_limit(OrderId id, Side side, Price px, Qty qty,
                              Tif tif = Tif::GTC, Owner owner = 0) {
        if (qty == 0 || px <= 0) return {0, false, true};
        // Reject id reuse while the original still rests. (Ids of orders
        // that already left the book are not tracked — remembering every id
        // ever seen is the gateway's job, not the matching engine's.)
        if (orders_.find(id) != orders_.end()) return {0, false, true};

        SubmitResult r;
        if (side == Side::Bid) r = execute<Side::Bid>(id, px, qty, tif, owner);
        else                   r = execute<Side::Ask>(id, px, qty, tif, owner);
        return r;
    }

    Qty submit_market(OrderId id, Side side, Qty qty, Owner owner = 0) {
        if (qty == 0) return 0;
        // A market order is a limit order with no price constraint that
        // never rests — one matching loop serves both (we pass the most
        // aggressive possible price instead of a special case).
        return (side == Side::Bid)
                   ? match<Side::Bid>(id, kMaxPrice, qty, owner).filled
                   : match<Side::Ask>(id, kMinPrice, qty, owner).filled;
    }

    bool cancel(OrderId id) {
        const auto it = orders_.find(id);        // O(1) — the whole point
        if (it == orders_.end()) return false;
        Order* o = it->second;
        orders_.erase(it);
        remove_from_book(o);
        delete o;
        return true;
    }

    // Exchange-standard semantics: qty decrease at same price amends in
    // place and KEEPS queue position; price change or qty increase loses
    // priority — the order re-enters as a fresh GTC arrival (and may cross).
    ModifyResult modify(OrderId id, Price new_px, Qty new_qty) {
        const auto it = orders_.find(id);
        if (it == orders_.end()) return {};
        if (new_qty == 0 || new_px <= 0)
            return {.ok = false, .filled = 0, .rested = false, .found = true};
        Order* o = it->second;

        if (new_px == o->price && new_qty <= o->qty) {
            o->level->total_qty -= (o->qty - new_qty);
            o->qty = new_qty;
            return {true, 0, true, true};
        }
        const Side  side  = o->side;
        const Owner owner = o->owner;
        orders_.erase(it);
        remove_from_book(o);
        delete o;
        const SubmitResult r = submit_limit(id, side, new_px, new_qty, Tif::GTC, owner);
        return {true, r.filled, r.rested, true};
    }

    Price best_bid() const { return bids_.empty() ? kNoPrice : bids_.begin()->first; }
    Price best_ask() const { return asks_.empty() ? kNoPrice : asks_.begin()->first; }

    std::uint64_t qty_at(Side side, Price px) const {
        if (side == Side::Bid) {
            const auto it = bids_.find(px);
            return it == bids_.end() ? 0 : it->second.total_qty;
        }
        const auto it = asks_.find(px);
        return it == asks_.end() ? 0 : it->second.total_qty;
    }

    std::size_t resting_orders() const { return orders_.size(); }

    // Debug/test-only structural audit. Returns nullptr if healthy, else a
    // description of the first violated invariant. Never called on the hot
    // path.
    const char* check_invariants() const {
        std::size_t seen = 0;
        if (const char* e = check_side(bids_, Side::Bid, seen)) return e;
        if (const char* e = check_side(asks_, Side::Ask, seen)) return e;
        if (seen != orders_.size()) return "id-map size != orders on book";
        if (!bids_.empty() && !asks_.empty() &&
            bids_.begin()->first >= asks_.begin()->first)
            return "book is crossed";
        return nullptr;
    }

private:
    static constexpr Price kMaxPrice = INT64_MAX;
    static constexpr Price kMinPrice = INT64_MIN;

    struct Level;

    struct Order {
        Order*  prev;
        Order*  next;
        Level*  level; // back-pointer: cancel updates level totals in O(1)
        OrderId id;
        Price   price;
        Qty     qty;   // remaining
        Side    side;
        Owner   owner;
    };

    struct Level {
        Order*        head = nullptr; // oldest — fills first
        Order*        tail = nullptr; // newest — arrivals append here
        std::uint64_t total_qty = 0;  // u64: sum of many u32s must not wrap
        std::uint32_t count = 0;
        Price         price = 0;
    };

    using Bids = std::map<Price, Level, std::greater<Price>>;
    using Asks = std::map<Price, Level, std::less<Price>>;

    // --- matching core ---------------------------------------------------

    struct MatchOutcome {
        Qty  filled       = 0;
        bool stp_stopped  = false; // RejectIncoming tripped mid-walk
    };

    template <Side S>
    SubmitResult execute(OrderId id, Price px, Qty qty, Tif tif, Owner owner) {
        if (tif == Tif::FOK && !fok_fillable<S>(px, qty, owner))
            return {0, false, true};

        const MatchOutcome m = match<S>(id, px, qty, owner);
        SubmitResult r{m.filled, false, false};
        const Qty rem = qty - m.filled;
        if (m.stp_stopped) {
            // RejectIncoming: the incoming order dies where it met its own
            // resting order. Fills already executed stand (they were against
            // other participants). Pure reject only if nothing filled.
            r.rejected = (m.filled == 0);
            return r;
        }
        if (rem > 0 && tif == Tif::GTC) {
            rest<S>(id, px, rem, owner);
            r.rested = true;
        }
        return r;
    }

    // Walk the opposite side best-first while the limit price crosses,
    // consuming orders head-to-tail within each level (FIFO time priority).
    template <Side S>
    MatchOutcome match(OrderId taker, Price limit_px, Qty want, Owner owner) {
        auto& opp = opposite_book<S>();
        MatchOutcome m;
        while (m.filled < want && !opp.empty()) {
            const auto it  = opp.begin();
            if (!crosses<S>(it->first, limit_px)) break;
            Level& lvl = it->second;

            Order* o = lvl.head;
            while (o != nullptr && m.filled < want) {
                Order* next = o->next; // o may be unlinked below
                if (o->owner == owner && stp_ != Stp::None) {
                    if (stp_ == Stp::RejectIncoming) {
                        m.stp_stopped = true;
                        break;
                    }
                    // CancelResting: own order leaves the book unfilled.
                    orders_.erase(o->id);
                    unlink(lvl, o);
                    delete o;
                } else {
                    const Qty take = static_cast<Qty>(
                        o->qty < want - m.filled ? o->qty : want - m.filled);
                    o->qty         -= take;
                    lvl.total_qty  -= take;
                    m.filled       += take;
                    listener_.on_trade(taker, o->id, lvl.price, take);
                    if (o->qty == 0) {
                        orders_.erase(o->id);
                        unlink(lvl, o);
                        delete o;
                    }
                }
                o = next;
            }
            if (lvl.head == nullptr) opp.erase(it);
            if (m.stp_stopped) break;
        }
        return m;
    }

    // FOK pre-check: is `qty` reachable at prices crossing `px`, under the
    // active STP policy? Fast path (no STP) sums level aggregates; with STP
    // we must walk orders because our own resting qty doesn't count
    // (CancelResting) or blocks entirely (RejectIncoming).
    template <Side S>
    bool fok_fillable(Price px, Qty qty, Owner owner) const {
        const auto& opp = opposite_book<S>();
        std::uint64_t avail = 0;
        for (const auto& [lvl_px, lvl] : opp) {
            if (!crosses<S>(lvl_px, px)) break;
            if (stp_ == Stp::None) {
                avail += lvl.total_qty;
            } else {
                for (const Order* o = lvl.head; o; o = o->next) {
                    if (o->owner == owner) {
                        if (stp_ == Stp::RejectIncoming) return false;
                        continue; // CancelResting: own qty is not liquidity
                    }
                    avail += o->qty;
                    if (avail >= qty) return true;
                }
            }
            if (avail >= qty) return true;
        }
        return avail >= qty;
    }

    // --- book maintenance ------------------------------------------------

    template <Side S>
    void rest(OrderId id, Price px, Qty qty, Owner owner) {
        auto& side_book = own_book<S>();
        Level& lvl = side_book[px]; // creates the level on first order
        lvl.price = px;

        Order* o = new Order{lvl.tail, nullptr, &lvl, id, px, qty, S, owner};
        if (lvl.tail) lvl.tail->next = o;
        else          lvl.head = o;
        lvl.tail = o;
        lvl.total_qty += qty;
        ++lvl.count;
        orders_.emplace(id, o);
    }

    static void unlink(Level& lvl, Order* o) {
        if (o->prev) o->prev->next = o->next; else lvl.head = o->next;
        if (o->next) o->next->prev = o->prev; else lvl.tail = o->prev;
        lvl.total_qty -= o->qty;
        --lvl.count;
    }

    // Cancel path: O(1) unlink via the intrusive links, then O(log L) map
    // erase only when the level just became empty.
    void remove_from_book(Order* o) {
        Level* lvl = o->level;
        unlink(*lvl, o);
        if (lvl->head == nullptr) {
            if (o->side == Side::Bid) bids_.erase(lvl->price);
            else                      asks_.erase(lvl->price);
        }
    }

    template <class BookSide>
    void free_all(BookSide& side) {
        for (auto& [px, lvl] : side)
            for (Order* o = lvl.head; o != nullptr;) {
                Order* n = o->next;
                delete o;
                o = n;
            }
    }

    // --- side helpers (compile-time, no runtime branch in the loops) -----

    template <Side S> auto&       own_book()            { if constexpr (S == Side::Bid) return bids_; else return asks_; }
    template <Side S> auto&       opposite_book()       { if constexpr (S == Side::Bid) return asks_; else return bids_; }
    template <Side S> const auto& opposite_book() const { if constexpr (S == Side::Bid) return asks_; else return bids_; }

    // Does a resting level at `book_px` cross an incoming order limited at
    // `limit_px`? Bid taker crosses asks priced <= limit; ask taker crosses
    // bids priced >= limit.
    template <Side S>
    static bool crosses(Price book_px, Price limit_px) {
        if constexpr (S == Side::Bid) return book_px <= limit_px;
        else                          return book_px >= limit_px;
    }

    template <class BookSide>
    const char* check_side(const BookSide& side, Side which, std::size_t& seen) const {
        Price prev_px = kNoPrice;
        bool  first   = true;
        for (const auto& [px, lvl] : side) {
            if (!first) {
                const bool ordered = (which == Side::Bid) ? px < prev_px : px > prev_px;
                if (!ordered) return "ladder out of order";
            }
            first = false;
            prev_px = px;
            if (lvl.price != px) return "level price != map key";
            if (lvl.head == nullptr) return "empty level left in ladder";
            std::uint64_t sum = 0;
            std::uint32_t cnt = 0;
            const Order* prev = nullptr;
            for (const Order* o = lvl.head; o; o = o->next) {
                if (o->prev != prev) return "broken prev link";
                if (o->level != &lvl) return "order->level mismatch";
                if (o->side != which) return "order on wrong side";
                if (o->price != px) return "order price != level price";
                if (o->qty == 0) return "zero-qty order resting";
                const auto idit = orders_.find(o->id);
                if (idit == orders_.end() || idit->second != o)
                    return "order missing from id map";
                sum += o->qty;
                ++cnt;
                ++seen;
                prev = o;
            }
            if (lvl.tail != prev) return "tail != last node";
            if (sum != lvl.total_qty) return "level total_qty wrong";
            if (cnt != lvl.count) return "level count wrong";
        }
        return nullptr;
    }

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Order*> orders_;
    Stp stp_;
    [[no_unique_address]] Listener listener_;
};

using Book = BookT<>;

} // namespace lob::v1
