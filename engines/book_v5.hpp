#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob::v5 {

// Engine v5 = v4 + flat banded price ladder. ONE measured change.
//
// std::map is a red-black tree: every level touch chases 3-5 scattered heap
// nodes, inserting rebalances, erasing a level costs O(log L) more pointer
// writes. But prices are small integers on a tick grid, and an instrument
// trades inside a bounded band (real venues enforce this: price bands,
// limit-up/limit-down). So the ladder becomes:
//
//   * one flat array of Levels per side, indexed by (price - band_lo):
//     level lookup is one add and one line load, O(1), no rebalancing ever;
//   * a bitmap (one bit per price) with one word covering 64 ticks, so
//     "where is the next non-empty level" is a masked word scan plus a
//     count-leading/trailing-zeros instruction;
//   * a cached best price per side: O(1) reads, updated on the spot when a
//     better order arrives, re-scanned via the bitmap only when the best
//     level empties.
//
// The cost, stated honestly: prices outside [band_lo, band_hi) are REJECTED.
// A production engine centers the band on the previous close and recenters
// on a halt; this one takes the band as constructor parameters. The scan
// after the best level empties is also the new tail-latency source -- see
// the benchmark discussion.
//
// Levels are never allocated or freed after construction: an empty level is
// just a slot with head == nullptr and a cleared bit. Erase-level, the
// O(log L) map operation, is now "clear one bit".
//
// Still untouched: unordered_map id index (v6).
template <class Listener = NullListener>
class BookT {
public:
    explicit BookT(Stp stp = Stp::None,
                   std::size_t pool_capacity = 1u << 17,
                   Price band_lo = 1, Price band_hi = Price{1} << 15)
        : bids_(band_lo, band_hi),
          asks_(band_lo, band_hi),
          stp_(stp),
          pool_(pool_capacity) {}
    ~BookT() = default;
    BookT(const BookT&)            = delete;
    BookT& operator=(const BookT&) = delete;

    Listener& listener() { return listener_; }
    std::size_t pool_grow_count() const { return pool_.grow_count(); }

    SubmitResult submit_limit(OrderId id, Side side, Price px, Qty qty,
                              Tif tif = Tif::GTC, Owner owner = 0) {
        if (qty == 0 || !bids_.in_band(px)) [[unlikely]] return {0, false, true};
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
        if (new_qty == 0 || !bids_.in_band(new_px)) [[unlikely]]
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

    Price best_bid() const { return bids_.best(); }
    Price best_ask() const { return asks_.best(); }

    std::uint64_t qty_at(Side side, Price px) const {
        if (!bids_.in_band(px)) return 0;
        return side == Side::Bid ? bids_.at(px).total_qty
                                 : asks_.at(px).total_qty;
    }

    std::size_t resting_orders() const { return orders_.size(); }

    const char* check_invariants() const {
        std::size_t seen = 0;
        if (const char* e = check_side(bids_, Side::Bid, seen)) return e;
        if (const char* e = check_side(asks_, Side::Ask, seen)) return e;
        if (seen != orders_.size()) return "id-map size != orders on book";
        if (bids_.best() != kNoPrice && asks_.best() != kNoPrice &&
            bids_.best() >= asks_.best())
            return "book is crossed";
        return nullptr;
    }

private:
    static constexpr Price kMaxPrice = INT64_MAX;
    static constexpr Price kMinPrice = INT64_MIN;

    struct Level;

    // One cache line, hot fields first (see v3).
    struct alignas(64) Order {
        Qty     qty;
        Owner   owner;
        Order*  next;
        OrderId id;
        Price   price;
        Order*  prev;
        Level*  level;
        Side    side;
    };
    static_assert(sizeof(Order) == 64, "Order must fill exactly one cache line");

    // One line per level too: a level touch (append, fill, deplete) stays
    // within a single line, and neighboring levels never false-share it.
    struct alignas(64) Level {
        Order*        head = nullptr;
        Order*        tail = nullptr;
        std::uint64_t total_qty = 0;
        std::uint32_t count = 0;
        Price         price = 0;
    };

    // Flat per-side ladder: Level array + occupancy bitmap + cached best.
    template <Side S>
    class Ladder {
    public:
        Ladder(Price lo, Price hi) : lo_(lo), hi_(hi) {
            levels_.resize(static_cast<std::size_t>(hi - lo));
            for (std::size_t i = 0; i < levels_.size(); ++i)
                levels_[i].price = lo_ + static_cast<Price>(i);
            words_.resize((levels_.size() + 63) / 64, 0);
        }

        bool  in_band(Price px) const { return px >= lo_ && px < hi_; }
        Price best() const { return best_; }

        Level&       at(Price px)       { return levels_[static_cast<std::size_t>(px - lo_)]; }
        const Level& at(Price px) const { return levels_[static_cast<std::size_t>(px - lo_)]; }

        // Level just gained its first order: set its bit, and take the best
        // cursor if this price is better (or the side was empty).
        void on_nonempty(Price px) {
            const std::size_t i = static_cast<std::size_t>(px - lo_);
            words_[i >> 6] |= (1ull << (i & 63));
            if (best_ == kNoPrice || better(px, best_)) best_ = px;
        }

        // Level just emptied: clear its bit; if it held the best cursor,
        // scan worse-ward for the next occupied level. This scan is the flat
        // ladder's tail-latency source: usually the next bit is in the same
        // word, but a deep sweep can walk several words.
        void on_empty(Price px) {
            const std::size_t i = static_cast<std::size_t>(px - lo_);
            words_[i >> 6] &= ~(1ull << (i & 63));
            if (best_ == px) best_ = scan_worse(px);
        }

        // First occupied price strictly worse than `from` (lower for bids,
        // higher for asks), or kNoPrice.
        Price scan_worse(Price from) const {
            std::size_t i = static_cast<std::size_t>(from - lo_);
            std::size_t w = i >> 6;
            const std::size_t b = i & 63;
            if constexpr (S == Side::Bid) {
                std::uint64_t word =
                    words_[w] & (b == 0 ? 0 : (~0ull >> (64 - b)));
                while (true) {
                    if (word)
                        return lo_ + static_cast<Price>(
                            (w << 6) + 63 - static_cast<std::size_t>(__builtin_clzll(word)));
                    if (w == 0) return kNoPrice;
                    word = words_[--w];
                }
            } else {
                std::uint64_t word =
                    words_[w] & (b == 63 ? 0 : (~0ull << (b + 1)));
                while (true) {
                    if (word)
                        return lo_ + static_cast<Price>(
                            (w << 6) + static_cast<std::size_t>(__builtin_ctzll(word)));
                    if (++w == words_.size()) return kNoPrice;
                    word = words_[w];
                }
            }
        }

        bool bit_set(Price px) const {
            const std::size_t i = static_cast<std::size_t>(px - lo_);
            return (words_[i >> 6] >> (i & 63)) & 1u;
        }
        Price lo() const { return lo_; }
        Price hi() const { return hi_; }

    private:
        static bool better(Price a, Price b) {
            if constexpr (S == Side::Bid) return a > b;
            else                          return a < b;
        }

        std::vector<Level>         levels_;
        std::vector<std::uint64_t> words_;
        Price                      lo_;
        Price                      hi_;
        Price                      best_ = kNoPrice;
    };

    struct MatchOutcome {
        Qty  filled      = 0;
        bool stp_stopped = false;
    };

    template <Side S>
    SubmitResult execute(OrderId id, Price px, Qty qty, Tif tif, Owner owner) {
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
        auto& opp = opposite_ladder<S>();
        MatchOutcome m;
        Qty remaining = want;
        while (remaining > 0) {
            const Price best = opp.best();
            if (best == kNoPrice || !crosses<S>(best, limit_px)) break;
            Level& lvl = opp.at(best);

            Order* o = lvl.head;
            while (o != nullptr) {
                if constexpr (StpOn) {
                    // owner 0 = ungrouped flow, exempt from STP by contract
                    if (owner != 0 && o->owner == owner) {
                        if (stp_ == Stp::RejectIncoming) {
                            m.stp_stopped = true;
                            break;
                        }
                        // CancelResting: own order leaves unfilled.
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
                    o->qty        -= remaining;
                    lvl.total_qty -= remaining;
                    listener_.on_trade(taker, o->id, lvl.price, remaining);
                    remaining = 0;
                    break;
                }
                const Qty take = o->qty;
                Order* next = o->next;
                remaining -= take;
                listener_.on_trade(taker, o->id, lvl.price, take);
                orders_.erase(o->id);
                unlink(lvl, o);
                pool_.destroy(o);
                o = next;
                if (remaining == 0) break;
            }
            if (lvl.head == nullptr) opp.on_empty(lvl.price);
            if (m.stp_stopped) break;
        }
        m.filled = want - remaining;
        return m;
    }

    template <Side S, bool StpOn>
    bool fok_fillable(Price px, Qty qty, Owner owner) const {
        const auto& opp = opposite_ladder_c<S>();
        std::uint64_t avail = 0;
        Price lvl_px = opp.best();
        while (lvl_px != kNoPrice && crosses<S>(lvl_px, px)) {
            const Level& lvl = opp.at(lvl_px);
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
            lvl_px = opp.scan_worse(lvl_px);
        }
        return avail >= qty;
    }

    template <Side S>
    void rest(OrderId id, Price px, Qty qty, Owner owner) {
        Level& lvl = own_ladder<S>().at(px);
        const bool was_empty = (lvl.head == nullptr);

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
        if (was_empty) own_ladder<S>().on_nonempty(px);
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
            if (o->side == Side::Bid) bids_.on_empty(lvl->price);
            else                      asks_.on_empty(lvl->price);
        }
    }

    template <Side S> auto&       own_ladder()            { if constexpr (S == Side::Bid) return bids_; else return asks_; }
    template <Side S> auto&       opposite_ladder()       { if constexpr (S == Side::Bid) return asks_; else return bids_; }
    template <Side S> const auto& opposite_ladder_c() const { if constexpr (S == Side::Bid) return asks_; else return bids_; }

    template <Side S>
    static bool crosses(Price book_px, Price limit_px) {
        if constexpr (S == Side::Bid) return book_px <= limit_px;
        else                          return book_px >= limit_px;
    }

    template <class LadderT>
    const char* check_side(const LadderT& lad, Side which, std::size_t& seen) const {
        Price found_best = kNoPrice;
        for (Price px = lad.lo(); px < lad.hi(); ++px) {
            const Level& lvl = lad.at(px);
            const bool bit = lad.bit_set(px);
            if (bit != (lvl.head != nullptr))
                return "bitmap out of sync with level";
            if (lvl.head == nullptr) {
                if (lvl.total_qty != 0 || lvl.count != 0)
                    return "empty level with nonzero totals";
                continue;
            }
            // Ascending scan: for bids the LAST occupied price is best, for
            // asks the FIRST one is.
            if (which == Side::Bid)              found_best = px;
            else if (found_best == kNoPrice)     found_best = px;
            if (lvl.price != px) return "level price != slot price";
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
        if (lad.best() != found_best) return "cached best is stale";
        return nullptr;
    }

    Ladder<Side::Bid> bids_;
    Ladder<Side::Ask> asks_;
    std::unordered_map<OrderId, Order*> orders_;
    Stp stp_;
    Pool<Order> pool_;
    [[no_unique_address]] Listener listener_;
};

using Book = BookT<>;

} // namespace lob::v5
