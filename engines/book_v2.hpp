#pragma once
#include <cstdint>
#include <map>
#include <unordered_map>

#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob::v2 {

// Engine v2 = v1 + memory pool for orders. ONE measured change.
//
// v1 paid a heap new on every resting order and a delete on every fill-out,
// cancel, and reprice. Those hit the general-purpose allocator: size-class
// bookkeeping, occasional madvise/mmap, and allocation addresses scattered
// across the heap (so a level walk pointer-chases cold lines). The pool
// replaces that with pop/push on a free list inside pre-reserved slabs.
//
// Still untouched: struct layout (v3), branch structure (v4), std::map
// ladder (v5), unordered_map id index — which still allocates a node per
// insert (v6).
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
        if (qty == 0 || px <= 0) return {0, false, true};
        if (orders_.find(id) != orders_.end()) return {0, false, true};

        SubmitResult r;
        if (side == Side::Bid) r = execute<Side::Bid>(id, px, qty, tif, owner);
        else                   r = execute<Side::Ask>(id, px, qty, tif, owner);
        return r;
    }

    Qty submit_market(OrderId id, Side side, Qty qty, Owner owner = 0) {
        if (qty == 0) return 0;
        return (side == Side::Bid)
                   ? match<Side::Bid>(id, kMaxPrice, qty, owner).filled
                   : match<Side::Ask>(id, kMinPrice, qty, owner).filled;
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

    struct Order {
        Order*  prev;
        Order*  next;
        Level*  level;
        OrderId id;
        Price   price;
        Qty     qty;
        Side    side;
        Owner   owner;
    };

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
        if (tif == Tif::FOK && !fok_fillable<S>(px, qty, owner))
            return {0, false, true};

        const MatchOutcome m = match<S>(id, px, qty, owner);
        SubmitResult r{m.filled, false, false};
        const Qty rem = qty - m.filled;
        if (m.stp_stopped) {
            r.rejected = (m.filled == 0);
            return r;
        }
        if (rem > 0 && tif == Tif::GTC) {
            rest<S>(id, px, rem, owner);
            r.rested = true;
        }
        return r;
    }

    template <Side S>
    MatchOutcome match(OrderId taker, Price limit_px, Qty want, Owner owner) {
        auto& opp = opposite_book<S>();
        MatchOutcome m;
        while (m.filled < want && !opp.empty()) {
            const auto it = opp.begin();
            if (!crosses<S>(it->first, limit_px)) break;
            Level& lvl = it->second;

            Order* o = lvl.head;
            while (o != nullptr && m.filled < want) {
                Order* next = o->next;
                if (o->owner == owner && stp_ != Stp::None) {
                    if (stp_ == Stp::RejectIncoming) {
                        m.stp_stopped = true;
                        break;
                    }
                    orders_.erase(o->id);
                    unlink(lvl, o);
                    pool_.destroy(o);
                } else {
                    const Qty take = static_cast<Qty>(
                        o->qty < want - m.filled ? o->qty : want - m.filled);
                    o->qty        -= take;
                    lvl.total_qty -= take;
                    m.filled      += take;
                    listener_.on_trade(taker, o->id, lvl.price, take);
                    if (o->qty == 0) {
                        orders_.erase(o->id);
                        unlink(lvl, o);
                        pool_.destroy(o);
                    }
                }
                o = next;
            }
            if (lvl.head == nullptr) opp.erase(it);
            if (m.stp_stopped) break;
        }
        return m;
    }

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

        Order* o = pool_.create(lvl.tail, nullptr, &lvl, id, px, qty, S, owner);
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

} // namespace lob::v2
