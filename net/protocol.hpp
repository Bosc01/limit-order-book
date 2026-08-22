#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>

#include "lob/types.hpp"

// Wire protocol. Little-endian binary, length-prefixed frames:
//
//   [len : u16][type : u8][payload ...]      len counts type + payload
//
// Real venues do the same shape (ITCH/OUCH are length-prefixed binary; ITCH
// happens to be big-endian). We pick little-endian and assert the host
// matches — both deployment targets (arm64, x86_64) are LE, and a protocol
// spec that matches the host means encode/decode is a copy, not a swap.
//
// Every decode is bounds-checked field-by-field via memcpy — never by
// casting the receive buffer to a packed struct. memcpy of known-size
// fields is UB-free on any alignment, optimizes to plain loads, and makes
// the "attacker controls the bytes" review tractable.
namespace lobnet {

static_assert(std::endian::native == std::endian::little,
              "wire format is little-endian; add byte swaps for BE hosts");

// Largest legal frame body. Sized by the biggest real message (BookMsg:
// 1 type + 8 seq + 2 sides * 5 levels * 16 bytes = 169) with headroom —
// found the hard way when the first smoke test rejected every book snapshot
// as hostile input. Every other message is under 32 bytes.
inline constexpr std::uint16_t kMaxFrameLen = 256;
inline constexpr std::size_t   kHeaderLen   = 2; // the u16 length prefix

enum class MsgType : std::uint8_t {
    // client -> gateway
    NewOrder = 1,
    Cancel   = 2,
    Modify   = 3,
    Market   = 4,
    // gateway -> client (TCP)
    ExecReport = 16,
    // gateway -> world (UDP)
    Trade = 32,
    Book  = 33,
};

// --- payload structs (host-side representations) ---------------------------

struct NewOrderMsg {
    lob::OrderId id;
    lob::Price   px;
    lob::Qty     qty;
    lob::Side    side;
    lob::Tif     tif;
};

struct CancelMsg {
    lob::OrderId id;
};

struct ModifyMsg {
    lob::OrderId id;
    lob::Price   new_px;
    lob::Qty     new_qty;
};

struct MarketMsg {
    lob::OrderId id;
    lob::Qty     qty;
    lob::Side    side;
};

enum class ExecStatus : std::uint8_t {
    Accepted    = 0, // rested on the book
    Filled      = 1, // fully filled on arrival
    PartialRest = 2, // partial fill, remainder rested
    PartialDone = 3, // partial fill, remainder discarded (IOC/STP)
    Rejected    = 4,
    Canceled    = 5, // cancel acknowledged
    Replaced    = 6, // modify acknowledged
    Unknown     = 7, // cancel/modify target not found
};

struct ExecReportMsg {
    lob::OrderId id;
    lob::Qty     filled;
    ExecStatus   status;
};

struct TradeMsg {
    std::uint64_t seq; // gap detection: UDP delivers best-effort
    lob::Price    px;
    lob::Qty      qty;
    lob::Side     taker_side;
};

inline constexpr int kBookDepth = 5;
struct BookMsg {
    std::uint64_t seq;
    lob::Price    bid_px[kBookDepth];
    std::uint64_t bid_qty[kBookDepth];
    lob::Price    ask_px[kBookDepth];
    std::uint64_t ask_qty[kBookDepth];
};

// --- byte-level helpers -----------------------------------------------------

class Writer {
public:
    Writer(std::uint8_t* buf, std::size_t cap) : buf_(buf), cap_(cap) {}

    template <class T>
    void put(T v) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (pos_ + sizeof(T) <= cap_) std::memcpy(buf_ + pos_, &v, sizeof(T));
        pos_ += sizeof(T); // overflow is caught by ok() — never writes past cap
    }

    bool        ok() const   { return pos_ <= cap_; }
    std::size_t size() const { return pos_; }

private:
    std::uint8_t* buf_;
    std::size_t   cap_;
    std::size_t   pos_ = 0;
};

class Reader {
public:
    Reader(const std::uint8_t* buf, std::size_t len) : buf_(buf), len_(len) {}

    template <class T>
    bool get(T& out) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (pos_ + sizeof(T) > len_) return false;
        std::memcpy(&out, buf_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }

    bool exhausted() const { return pos_ == len_; }

private:
    const std::uint8_t* buf_;
    std::size_t         len_;
    std::size_t         pos_ = 0;
};

// --- encode -----------------------------------------------------------------
// Each encode_* writes a complete frame (length prefix included) and returns
// the frame's total size, or 0 if the buffer was too small.

inline std::size_t finish(Writer& w, std::uint8_t* buf, std::size_t body_start) {
    if (!w.ok()) return 0;
    const std::size_t body = w.size() - body_start;
    const std::uint16_t len = static_cast<std::uint16_t>(body);
    std::memcpy(buf, &len, sizeof(len));
    return w.size();
}

template <class F>
inline std::size_t encode_frame(std::uint8_t* buf, std::size_t cap, MsgType t, F&& fill) {
    Writer w(buf, cap);
    w.put(std::uint16_t{0}); // patched by finish()
    w.put(static_cast<std::uint8_t>(t));
    fill(w);
    return finish(w, buf, kHeaderLen);
}

inline std::size_t encode(std::uint8_t* b, std::size_t c, const NewOrderMsg& m) {
    return encode_frame(b, c, MsgType::NewOrder, [&](Writer& w) {
        w.put(m.id); w.put(m.px); w.put(m.qty);
        w.put(static_cast<std::uint8_t>(m.side));
        w.put(static_cast<std::uint8_t>(m.tif));
    });
}

inline std::size_t encode(std::uint8_t* b, std::size_t c, const CancelMsg& m) {
    return encode_frame(b, c, MsgType::Cancel, [&](Writer& w) { w.put(m.id); });
}

inline std::size_t encode(std::uint8_t* b, std::size_t c, const ModifyMsg& m) {
    return encode_frame(b, c, MsgType::Modify, [&](Writer& w) {
        w.put(m.id); w.put(m.new_px); w.put(m.new_qty);
    });
}

inline std::size_t encode(std::uint8_t* b, std::size_t c, const MarketMsg& m) {
    return encode_frame(b, c, MsgType::Market, [&](Writer& w) {
        w.put(m.id); w.put(m.qty); w.put(static_cast<std::uint8_t>(m.side));
    });
}

inline std::size_t encode(std::uint8_t* b, std::size_t c, const ExecReportMsg& m) {
    return encode_frame(b, c, MsgType::ExecReport, [&](Writer& w) {
        w.put(m.id); w.put(m.filled); w.put(static_cast<std::uint8_t>(m.status));
    });
}

inline std::size_t encode(std::uint8_t* b, std::size_t c, const TradeMsg& m) {
    return encode_frame(b, c, MsgType::Trade, [&](Writer& w) {
        w.put(m.seq); w.put(m.px); w.put(m.qty);
        w.put(static_cast<std::uint8_t>(m.taker_side));
    });
}

inline std::size_t encode(std::uint8_t* b, std::size_t c, const BookMsg& m) {
    return encode_frame(b, c, MsgType::Book, [&](Writer& w) {
        w.put(m.seq);
        for (int i = 0; i < kBookDepth; ++i) { w.put(m.bid_px[i]); w.put(m.bid_qty[i]); }
        for (int i = 0; i < kBookDepth; ++i) { w.put(m.ask_px[i]); w.put(m.ask_qty[i]); }
    });
}

// --- decode -----------------------------------------------------------------
// `body` excludes the length prefix but INCLUDES the type byte. Attacker
// controls every byte: each decoder checks type, field bounds, enum ranges,
// and rejects trailing garbage.

inline bool valid_side(std::uint8_t s) { return s <= 1; }
inline bool valid_tif(std::uint8_t t)  { return t <= 2; }

inline std::optional<MsgType> peek_type(const std::uint8_t* body, std::size_t len) {
    if (len < 1) return std::nullopt;
    return static_cast<MsgType>(body[0]);
}

template <class Msg, class F>
inline std::optional<Msg> decode_body(const std::uint8_t* body, std::size_t len, F&& parse) {
    Reader r(body + 1, len - 1); // caller already peeked the type byte
    Msg m{};
    if (!parse(r, m) || !r.exhausted()) return std::nullopt;
    return m;
}

inline std::optional<NewOrderMsg> decode_new_order(const std::uint8_t* body, std::size_t len) {
    return decode_body<NewOrderMsg>(body, len, [](Reader& r, NewOrderMsg& m) {
        std::uint8_t side, tif;
        if (!r.get(m.id) || !r.get(m.px) || !r.get(m.qty) ||
            !r.get(side) || !r.get(tif)) return false;
        if (!valid_side(side) || !valid_tif(tif)) return false;
        m.side = static_cast<lob::Side>(side);
        m.tif  = static_cast<lob::Tif>(tif);
        return true;
    });
}

inline std::optional<CancelMsg> decode_cancel(const std::uint8_t* body, std::size_t len) {
    return decode_body<CancelMsg>(body, len, [](Reader& r, CancelMsg& m) {
        return r.get(m.id);
    });
}

inline std::optional<ModifyMsg> decode_modify(const std::uint8_t* body, std::size_t len) {
    return decode_body<ModifyMsg>(body, len, [](Reader& r, ModifyMsg& m) {
        return r.get(m.id) && r.get(m.new_px) && r.get(m.new_qty);
    });
}

inline std::optional<MarketMsg> decode_market(const std::uint8_t* body, std::size_t len) {
    return decode_body<MarketMsg>(body, len, [](Reader& r, MarketMsg& m) {
        std::uint8_t side;
        if (!r.get(m.id) || !r.get(m.qty) || !r.get(side)) return false;
        if (!valid_side(side)) return false;
        m.side = static_cast<lob::Side>(side);
        return true;
    });
}

inline std::optional<ExecReportMsg> decode_exec_report(const std::uint8_t* body, std::size_t len) {
    return decode_body<ExecReportMsg>(body, len, [](Reader& r, ExecReportMsg& m) {
        std::uint8_t st;
        if (!r.get(m.id) || !r.get(m.filled) || !r.get(st)) return false;
        if (st > static_cast<std::uint8_t>(ExecStatus::Unknown)) return false;
        m.status = static_cast<ExecStatus>(st);
        return true;
    });
}

inline std::optional<TradeMsg> decode_trade(const std::uint8_t* body, std::size_t len) {
    return decode_body<TradeMsg>(body, len, [](Reader& r, TradeMsg& m) {
        std::uint8_t side;
        if (!r.get(m.seq) || !r.get(m.px) || !r.get(m.qty) || !r.get(side)) return false;
        if (!valid_side(side)) return false;
        m.taker_side = static_cast<lob::Side>(side);
        return true;
    });
}

inline std::optional<BookMsg> decode_book(const std::uint8_t* body, std::size_t len) {
    return decode_body<BookMsg>(body, len, [](Reader& r, BookMsg& m) {
        if (!r.get(m.seq)) return false;
        for (int i = 0; i < kBookDepth; ++i)
            if (!r.get(m.bid_px[i]) || !r.get(m.bid_qty[i])) return false;
        for (int i = 0; i < kBookDepth; ++i)
            if (!r.get(m.ask_px[i]) || !r.get(m.ask_qty[i])) return false;
        return true;
    });
}

} // namespace lobnet
