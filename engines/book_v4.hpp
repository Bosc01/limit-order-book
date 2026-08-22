#pragma once
#include <cstdint>
#include <map>
#include <unordered_map>

#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob::v4 {

// Engine v4 = v3 + branch discipline in the matching loop. ONE measured
// change, in three parts:
//
//   1. STP specialization: the self-trade check was two extra compares on
//      EVERY fill iteration even for books that never arm STP. The match
//      loop is now templated on <Side, StpOn>; submit dispatches once per
//      order, and with StpOn=false the check does not exist in the emitted
//      code. Side was already a template parameter from v1 — the bid and
//      ask loops are two separately-compiled functions with the comparison
//      direction baked in, not a per-iteration "which side am I" branch.
//
//   2. The inner fill loop is restructured from "compute min, then test if
//      the maker hit zero" into two explicit cases: maker-outlives-taker
//      (the common exit: reduce, report, done) and maker-fully-consumed
//      (unlink and keep walking). One well-predicted branch replaces two
//      data-dependent ones.
//
//   3. [[likely]]/[[unlikely]] annotations on validation rejects and TIF
//      dispatch, so the fall-through path is the straight-line one.
//
// Still untouched: std::map ladder (v5), unordered_map id index (v6).
template <class Listener = NullListener>
class BookT {
public:
    explicit BookT(Stp stp = Stp::None, std::size_t pool_capacity = 1u << 17)
        : orders_(), stp_(stp), pool_(pool_capacity) {}
    // No per-order teardown: the pool releases its slabs wholesale.
    ~BookT() = default;
    BookT(const BookT&)            = delete;
    BookT& operator=(const BookT&) = delete;

    Listener& listener() { return listener_; }
    std::size_t pool_grow_count() const { return pool_.grow_count(); }

    SubmitResult submit_limit(OrderId id, Side side, Price px, Qty qty,
                              Tif tif = Tif::GTC, Owner owner = 0) {
        if (qty == 0 || px <= 0) [[unlikely]] return {0, false, true};
        if (orders_.find(id) != orders_.end()) [[unlikely]] return {0, false, true};

        SubmitResult r;
        if (side == Side::Bid) r = execute<Side::Bid>(id, px, qty, tif, owner);
        else                   r = execute<Side::Ask>(id, px, qty, tif, owner);
        return r;
    }

    Qty submit_market(OrderId id, Side side, Qty qty, Owner owner = 0) {
        // A market order never rests, but its id still prints on the trade
        // tape as the taker: reject an id that aliases a live resting order.
        if (qty == 0 || orders_.find(id) != orders_.end()) [[unlikely]] return 0;
        if (side == Side::Bid)
            return stp_ == Stp::None
                       ? match<Side::Bid, false>(id, kMaxPrice, qty, owner).filled
                       : match<Side::Bid, true>(id, kMaxPrice, qty, owner).filled;
        return stp_ == Stp::None
                   ? match<Side::Ask, false>(id, kMinPrice, qty, owner).filled
                   : match<Side::Ask, true>(id, kMinPrice, qty, owner).filled;
    }

    bool cancel(OrderId id) {
        const auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        Order* o = it->second;
        orders_.erase(it);
        remove_from_book(o);
        pool_.destroy(o);
        return true;
    }

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
        pool_.destroy(o);
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

    // One cache line, hot fields first. See the class comment.
    struct alignas(64) Order {
        // hot: touched every fill iteration in the match loop
        Qty     qty;   // written on every fill
        Owner   owner; // read on every iteration when STP is armed
        Order*  next;  // FIFO walk direction
        OrderId id;    // trade report + id-map erase on full fill
        Price   price; // trade report + modify comparison
        // cold: touched only on cancel/modify/unlink
        Order*  prev;
        Level*  level;
        Side    side;
    };
    static_assert(sizeof(Order) == 64, "Order must fill exactly one cache line");

    struct Level {
        Order*        head = nullptr;
        Order*        tail = nullptr;
        std::uint64_t total_qty = 0;
        std::uint32_t count = 0;
        Price         price = 0;
    };

    using Bids = std::map<Price, Level, std::greater<Price>>;
    using Asks = std::map<Price, Level, std::less<Price>>;

    struct MatchOutcome {
        Qty  filled      = 0;
        bool stp_stopped = false;
    };

    template <Side S>
    SubmitResult execute(OrderId id, Price px, Qty qty, Tif tif, Owner owner) {
        // One runtime STP dispatch per order; zero per fill iteration.
        const bool stp_on = (stp_ != Stp::None);
        if (tif == Tif::FOK) [[unlikely]] {
            const bool fillable = stp_on ? fok_fillable<S, true>(px, qty, owner)
                                         : fok_fillable<S, false>(px, qty, owner);
            if (!fillable) return {0, false, true};
        }

        const MatchOutcome m = stp_on ? match<S, true>(id, px, qty, owner)
                                      : match<S, false>(id, px, qty, owner);
        SubmitResult r{m.filled, false, false};
        if (m.stp_stopped) [[unlikely]] {
            r.rejected = (m.filled == 0);
            return r;
        }
        if (m.filled < qty && tif == Tif::GTC) [[likely]] {
            rest<S>(id, px, qty - m.filled, owner);
            r.rested = true;
        }
        return r;
    }

    template <Side S, bool StpOn>
    MatchOutcome match(OrderId taker, Price limit_px, Qty want, Owner owner) {
        auto& opp = opposite_book<S>();
        MatchOutcome m;
        Qty remaining = want;
        while (remaining > 0 && !opp.empty()) {
            const auto it = opp.begin();
            if (!crosses<S>(it->first, limit_px)) break;
            Level& lvl = it->second;

            Order* o = lvl.head;
            while (o != nullptr) {
                if constexpr (StpOn) {
                    // owner 0 = ungrouped flow, exempt from STP by contract
                    if (owner != 0 && o->owner == owner) {
                        if (stp_ == Stp::RejectIncoming) {
                            m.stp_stopped = true;
                            break;
                        }
                        // CancelResting: own order leaves the book unfilled.
                        listener_.on_cancel(o->id, o->owner);
                        Order* next = o->next;
                        orders_.erase(o->id);
                        unlink(lvl, o);
                        pool_.destroy(o);
                        o = next;
                        continue;
                    }
                }
                if (o->qty > remaining) {
                    // Maker outlives taker: the common exit at the last
                    // touched level. Reduce in place, report, done.
                    o->qty        -= remaining;
                    lvl.total_qty -= remaining;
                    listener_.on_trade(taker, o->id, lvl.price, remaining);
                    remaining = 0;
                    break;
                }
                // Maker fully consumed: report, unlink, keep walking.
                const Qty take = o->qty;
                Order* next = o->next;
                remaining -= take;
                listener_.on_trade(taker, o->id, lvl.price, take);
                orders_.erase(o->id);
                unlink(lvl, o); // subtracts o->qty (== take) from the level
                pool_.destroy(o);
                o = next;
                if (remaining == 0) break;
            }
            if (lvl.head == nullptr) opp.erase(it);
            if (m.stp_stopped) break;
        }
        m.filled = want - remaining;
        return m;
    }

    template <Side S, bool StpOn>
    bool fok_fillable(Price px, Qty qty, Owner owner) const {
        const auto& opp = opposite_book<S>();
        std::uint64_t avail = 0;
        for (const auto& [lvl_px, lvl] : opp) {
            if (!crosses<S>(lvl_px, px)) break;
            if constexpr (!StpOn) {
                avail += lvl.total_qty;
            } else {
                for (const Order* o = lvl.head; o; o = o->next) {
                    if (owner != 0 && o->owner == owner) {
                        if (stp_ == Stp::RejectIncoming) return false;
                        continue;
                    }
                    avail += o->qty;
                    if (avail >= qty) return true;
                }
            }
            if (avail >= qty) return true;
        }
        return avail >= qty;
    }

    template <Side S>
    void rest(OrderId id, Price px, Qty qty, Owner owner) {
        auto& side_book = own_book<S>();
        Level& lvl = side_book[px];
        lvl.price = px;

        Order* o = pool_.create(Order{.qty   = qty,
                                      .owner = owner,
                                      .next  = nullptr,
                                      .id    = id,
                                      .price = px,
                                      .prev  = lvl.tail,
                                      .level = &lvl,
                                      .side  = S});
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

    void remove_from_book(Order* o) {
        Level* lvl = o->level;
        unlink(*lvl, o);
        if (lvl->head == nullptr) {
            if (o->side == Side::Bid) bids_.erase(lvl->price);
            else                      asks_.erase(lvl->price);
        }
    }

    template <Side S> auto&       own_book()            { if constexpr (S == Side::Bid) return bids_; else return asks_; }
    template <Side S> auto&       opposite_book()       { if constexpr (S == Side::Bid) return asks_; else return bids_; }
    template <Side S> const auto& opposite_book() const { if constexpr (S == Side::Bid) return asks_; else return bids_; }

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
    Pool<Order> pool_;
    [[no_unique_address]] Listener listener_;
};

using Book = BookT<>;

} // namespace lob::v4
