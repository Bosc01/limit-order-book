#pragma once
#include <cstdint>
#include <random>
#include <vector>

#include "lob/types.hpp"

namespace bench {

enum class OpKind : std::uint8_t { Limit, Market, Cancel, Modify };
inline constexpr int kNumOpKinds = 4;

// One pre-generated benchmark operation. Generated entirely before timing
// starts so RNG cost is never inside a measured sample.
struct Op {
    lob::OrderId id;    // new id for Limit/Market; target id for Cancel/Modify
    lob::Price   price; // Limit: limit price; Modify: new price
    lob::Qty     qty;   // Limit/Market: qty; Modify: new qty
    OpKind       kind;
    lob::Side    side;
    lob::Tif     tif;
};

// Deterministic workload generator.
//
// All randomness comes from raw mt19937_64 draws. Deliberately NOT
// std::uniform_int_distribution: the standard specifies the engine's bit
// stream but not the distribution algorithm, so "seed 42" through a
// distribution produces different workloads on libc++ vs libstdc++. Raw
// engine output modulo a bound is portable (the modulo bias is irrelevant
// for a benchmark workload).
//
// Op mix (per 100 ops): 45 passive limit, 10 aggressive (crossing) limit,
// 5 market, 30 cancel, 10 modify. Cancel/modify-heavy on purpose: real
// venues see far more cancels and amends than trades (cancel-to-trade
// ratios above 10:1 are normal), and those are exactly the operations the
// naive book does worst. Aggressive limits carry a TIF mix (70% GTC,
// 20% IOC, 10% FOK); passive orders are GTC.
//
// The generator does not simulate matching, so a cancel/modify may target
// an already-filled order. That is realistic -- a cancel racing a fill loses
// at real exchanges too -- and it keeps generation a pure function of the
// seed, so every engine version replays the identical byte-for-byte stream.
class WorkloadGen {
public:
    explicit WorkloadGen(std::uint64_t seed) : rng_(seed) {}

    std::vector<Op> generate(std::size_t n) {
        std::vector<Op> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            if ((i & 63) == 0) drift_mid();
            const std::uint64_t p = draw() % 100;
            if      (p < 30 && !live_.empty()) out.push_back(cancel_op());
            else if (p < 40 && !live_.empty()) out.push_back(modify_op());
            else if (p < 45)                   out.push_back(market_op());
            else if (p < 55)                   out.push_back(aggressive_limit());
            else                               out.push_back(passive_limit());
        }
        return out;
    }

private:
    struct LiveRef {
        lob::OrderId id;
        lob::Side    side;
        lob::Price   px; // last price we told the engine for this order
    };

    std::uint64_t draw() { return rng_(); }

    lob::Side rand_side() {
        return (draw() & 1) ? lob::Side::Ask : lob::Side::Bid;
    }

    // Rests 1..8 ticks away from mid on its own side of the book.
    Op passive_limit() {
        const lob::Side  s   = rand_side();
        const lob::Price off = 1 + static_cast<lob::Price>(draw() % 8);
        const lob::Price px  = (s == lob::Side::Bid) ? mid_ - off : mid_ + off;
        const lob::Qty   q   = 1 + static_cast<lob::Qty>(draw() % 100);
        const lob::OrderId id = next_id_++;
        track(id, s, px);
        return {id, px, q, OpKind::Limit, s, lob::Tif::GTC};
    }

    // Priced 0..2 ticks *through* the mid, so it crosses resting orders and
    // exercises the matching loop. GTC remainder may rest, so it is tracked
    // (IOC/FOK ids are tracked too -- cancels against them simply miss,
    // which is realistic noise).
    Op aggressive_limit() {
        const lob::Side  s   = rand_side();
        const lob::Price off = static_cast<lob::Price>(draw() % 3);
        const lob::Price px  = (s == lob::Side::Bid) ? mid_ + off : mid_ - off;
        const lob::Qty   q   = 1 + static_cast<lob::Qty>(draw() % 100);
        const std::uint64_t t = draw() % 10;
        const lob::Tif tif = t < 7 ? lob::Tif::GTC
                           : t < 9 ? lob::Tif::IOC
                                   : lob::Tif::FOK;
        const lob::OrderId id = next_id_++;
        track(id, s, px);
        return {id, px, q, OpKind::Limit, s, tif};
    }

    Op market_op() {
        // Market orders never rest -> not tracked as cancel targets.
        const lob::Qty q = 1 + static_cast<lob::Qty>(draw() % 50);
        return {next_id_++, 0, q, OpKind::Market, rand_side(), lob::Tif::IOC};
    }

    Op cancel_op() {
        const std::size_t idx = draw() % live_.size();
        const LiveRef ref = live_[idx];
        live_[idx] = live_.back(); // swap-pop: never cancel the same id twice
        live_.pop_back();
        return {ref.id, 0, 0, OpKind::Cancel, ref.side, lob::Tif::GTC};
    }

    // 50/50 between a size-down at the same price (engine keeps queue
    // position) and a reprice (engine treats as cancel + fresh arrival).
    // The order stays in live_ -- it is still on the book under the same id.
    Op modify_op() {
        const std::size_t idx = draw() % live_.size();
        LiveRef& ref = live_[idx];
        if (draw() & 1) {
            const lob::Qty q = 1 + static_cast<lob::Qty>(draw() % 15);
            return {ref.id, ref.px, q, OpKind::Modify, ref.side, lob::Tif::GTC};
        }
        const lob::Price off = 1 + static_cast<lob::Price>(draw() % 8);
        const lob::Price px =
            (ref.side == lob::Side::Bid) ? mid_ - off : mid_ + off;
        ref.px = px;
        const lob::Qty q = 1 + static_cast<lob::Qty>(draw() % 100);
        return {ref.id, px, q, OpKind::Modify, ref.side, lob::Tif::GTC};
    }

    void track(lob::OrderId id, lob::Side s, lob::Price px) {
        // Cap the target pool so cancels/modifies skew toward recent orders,
        // which mirrors real flow (most cancels hit young orders).
        if (live_.size() >= 50'000) {
            const std::size_t j = draw() % live_.size();
            live_[j] = live_.back();
            live_.pop_back();
        }
        live_.push_back({id, s, px});
    }

    void drift_mid() {
        mid_ += static_cast<lob::Price>(draw() % 3) - 1; // -1, 0, +1
        if (mid_ < 9'000)  mid_ = 9'000;
        if (mid_ > 11'000) mid_ = 11'000;
    }

    std::mt19937_64      rng_;
    lob::Price           mid_     = 10'000;
    lob::OrderId         next_id_ = 1;
    std::vector<LiveRef> live_;
};

} // namespace bench
