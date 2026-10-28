#pragma once
// Gateway core: the engine listener, per-connection state, and the frame ->
// engine op -> exec report step. No sockets are opened here, so tests can
// drive handle_frame() directly; the poll() loop lives in gateway.cpp.
#include <arpa/inet.h>
#include <cstring>
#include <functional>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/order_book.hpp"
#include "framing.hpp"
#include "protocol.hpp"

namespace lobnet {

// --- order ownership --------------------------------------------------------

// Each connection names its orders in its own id space; the engine only ever
// sees ids the gateway assigns from one counter. A client therefore cannot
// cancel or amend another client's order, or learn which ids another client
// has live: an id it does not own reads exactly like an id that does not
// exist. Keyed by owner, which the gateway assigns once per connection and
// never reuses. An entry lives exactly as long as its order rests.
class OrderIds {
public:
    // Engine id for a new order, or 0 -- which the engine rejects, as it
    // always has -- when the client id is 0 or already live for this owner.
    lob::OrderId assign(lob::Owner owner, lob::OrderId client) {
        return client == 0 || find(owner, client) != 0 ? 0 : ++last_;
    }

    // Engine id of one of this owner's resting orders, or 0, which the
    // engine answers as unknown.
    lob::OrderId find(lob::Owner owner, lob::OrderId client) const {
        const auto it = by_client_.find({owner, client});
        return it == by_client_.end() ? 0 : it->second;
    }

    // Records a resting order, or refreshes its open quantity.
    void rest(lob::OrderId id, lob::Owner owner, lob::OrderId client,
              lob::Qty open, lob::Side side) {
        by_client_[{owner, client}] = id;
        live_[id] = {owner, client, open, side};
    }

    std::optional<lob::Side> side(lob::OrderId id) const {
        const auto it = live_.find(id);
        if (it == live_.end()) return std::nullopt;
        return it->second.side;
    }

    // A resting order traded; once its open quantity is used up it has left
    // the book.
    void fill(lob::OrderId id, lob::Qty qty) {
        const auto it = live_.find(id);
        if (it != live_.end() && (it->second.open -= qty) == 0) forget(id);
    }

    // The order left the book. Returns its client id (0 if untracked).
    lob::OrderId forget(lob::OrderId id) {
        const auto it = live_.find(id);
        if (it == live_.end()) return 0;
        const lob::OrderId client = it->second.client;
        by_client_.erase({it->second.owner, client});
        live_.erase(it);
        return client;
    }

    // Engine ids of every order this owner has resting.
    std::vector<lob::OrderId> owned_by(lob::Owner owner) const {
        std::vector<lob::OrderId> ids;
        for (auto it = by_client_.lower_bound({owner, 0});
             it != by_client_.end() && it->first.first == owner; ++it)
            ids.push_back(it->second);
        return ids;
    }

    std::size_t size() const { return live_.size(); }

private:
    struct Live {
        lob::Owner   owner;
        lob::OrderId client;
        lob::Qty     open;
        lob::Side    side;
    };
    std::map<std::pair<lob::Owner, lob::OrderId>, lob::OrderId> by_client_;
    std::unordered_map<lob::OrderId, Live>                      live_;
    lob::OrderId last_ = 0; // engine ids start at 1; 0 is reserved
};

// --- market data ------------------------------------------------------------

class FeedPublisher {
public:
    bool open(const char* ip, std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;
        std::memset(&dest_, 0, sizeof(dest_));
        dest_.sin_family = AF_INET;
        dest_.sin_port   = htons(port);
        if (inet_pton(AF_INET, ip, &dest_.sin_addr) != 1) return false;

        // Multicast destination? Keep it on-host for the demo: TTL 1 and
        // loopback enabled so a subscriber on this machine receives it.
        const auto addr = ntohl(dest_.sin_addr.s_addr);
        if ((addr >> 28) == 0xE) {
            const unsigned char ttl = 1, loop = 1;
            setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
            setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        }
        return true;
    }

    // Engine listener hook: every fill becomes a Trade message. Order ids
    // are deliberately NOT published -- public feeds are anonymous; ids go
    // only to the owning client via its exec report.
    void on_trade(lob::OrderId /*taker*/, lob::OrderId maker, lob::Price px,
                  lob::Qty qty) {
        orders.fill(maker, qty);
        lobnet::TradeMsg t{next_seq(), px, qty, taker_side_};
        std::uint8_t buf[64];
        const auto n = lobnet::encode(buf, sizeof(buf), t);
        if (n) send_datagram(buf, n);
    }

    void publish_book(const lob::OrderBookT<FeedPublisher>& book) {
        lobnet::BookMsg m{};
        m.seq = next_seq();
        const int nb = book.top_levels(lob::Side::Bid, lobnet::kBookDepth,
                                       m.bid_px, m.bid_qty);
        const int na = book.top_levels(lob::Side::Ask, lobnet::kBookDepth,
                                       m.ask_px, m.ask_qty);
        for (int i = nb; i < lobnet::kBookDepth; ++i) m.bid_px[i] = lob::kNoPrice;
        for (int i = na; i < lobnet::kBookDepth; ++i) m.ask_px[i] = lob::kNoPrice;
        std::uint8_t buf[256];
        const auto n = lobnet::encode(buf, sizeof(buf), m);
        if (n) send_datagram(buf, n);
    }

    // Engine listener hook: the engine removed a resting order without its
    // owner asking (STP CancelResting). The owner must be told, or the
    // order silently vanishes from their point of view.
    void on_cancel(lob::OrderId id, lob::Owner owner) {
        const lob::OrderId client = orders.forget(id);
        if (on_unsolicited_cancel) on_unsolicited_cancel(client, owner);
    }

    // The taker side of the op currently being processed (trades inherit it).
    void set_taker_side(lob::Side s) { taker_side_ = s; }

    // Receives the killed order's CLIENT id and its owner.
    std::function<void(lob::OrderId, lob::Owner)> on_unsolicited_cancel;

    // As the engine's listener, this sees every fill and STP kill, which
    // are the ways an order leaves the book without its owner asking.
    OrderIds orders;

private:
    std::uint64_t next_seq() { return ++seq_; }

    void send_datagram(const std::uint8_t* p, std::size_t n) {
        // Best effort by design: UDP either delivers or it does not, and the
        // publisher never blocks on subscribers.
        (void)::sendto(fd_, p, n, 0, reinterpret_cast<sockaddr*>(&dest_),
                       sizeof(dest_));
    }

    int           fd_ = -1;
    sockaddr_in   dest_{};
    std::uint64_t seq_ = 0;
    lob::Side     taker_side_ = lob::Side::Bid;
};

using Engine = lob::OrderBookT<FeedPublisher>;

// --- per-connection state ---------------------------------------------------

struct Connection {
    int                  fd = -1;
    lob::Owner           owner = 0;
    lobnet::FrameBuffer  frames{};
    std::vector<std::uint8_t> outbox{}; // pending bytes, capped at kMaxOutbox
    bool                 dead = false;
};

inline constexpr std::size_t kMaxOutbox = 64 * 1024;

inline void queue_send(Connection& c, const std::uint8_t* p, std::size_t n) {
    if (c.outbox.size() + n > kMaxOutbox) {
        c.dead = true; // slow consumer: drop it rather than buffer without bound
        return;
    }
    c.outbox.insert(c.outbox.end(), p, p + n);
}

inline void send_exec(Connection& c, lob::OrderId id, lob::Qty filled,
                      lobnet::ExecStatus st) {
    std::uint8_t buf[64]{};
    const auto n = lobnet::encode(buf, sizeof(buf),
                                  lobnet::ExecReportMsg{id, filled, st});
    if (n) queue_send(c, buf, n);
}

inline lobnet::ExecStatus submit_status(const lob::SubmitResult& r, lob::Qty want) {
    if (r.rejected) return lobnet::ExecStatus::Rejected;
    if (r.filled == want) return lobnet::ExecStatus::Filled;
    if (r.rested)
        return r.filled ? lobnet::ExecStatus::PartialRest
                        : lobnet::ExecStatus::Accepted;
    return r.filled ? lobnet::ExecStatus::PartialDone
                    : lobnet::ExecStatus::Canceled; // e.g. IOC with no cross
}

// --- inbound frame -> engine op --> exec report -----------------------------
// Inbound ids are client ids, translated through OrderIds before the engine
// sees them; every exec report carries the client id back.

inline void handle_frame(Engine& book, FeedPublisher& feed, Connection& c,
                         const std::uint8_t* body, std::size_t len) {
    if (c.dead) return; // later frames in the same read are not executed
    const auto type = lobnet::peek_type(body, len);
    if (!type) { c.dead = true; return; }
    OrderIds& ids = feed.orders;

    switch (*type) {
        case lobnet::MsgType::NewOrder: {
            const auto m = lobnet::decode_new_order(body, len);
            if (!m) { c.dead = true; return; }
            feed.set_taker_side(m->side);
            const lob::OrderId id = ids.assign(c.owner, m->id);
            const auto r =
                book.submit_limit(id, m->side, m->px, m->qty, m->tif, c.owner);
            if (r.rested) ids.rest(id, c.owner, m->id, m->qty - r.filled, m->side);
            send_exec(c, m->id, r.filled, submit_status(r, m->qty));
            break;
        }
        case lobnet::MsgType::Market: {
            const auto m = lobnet::decode_market(body, len);
            if (!m) { c.dead = true; return; }
            feed.set_taker_side(m->side);
            const lob::OrderId id = ids.assign(c.owner, m->id);
            const lob::Qty filled =
                id ? book.submit_market(id, m->side, m->qty, c.owner) : 0;
            send_exec(c, m->id, filled,
                      id == 0          ? lobnet::ExecStatus::Rejected
                      : filled == m->qty ? lobnet::ExecStatus::Filled
                      : filled         ? lobnet::ExecStatus::PartialDone
                                       : lobnet::ExecStatus::Canceled);
            break;
        }
        case lobnet::MsgType::Cancel: {
            const auto m = lobnet::decode_cancel(body, len);
            if (!m) { c.dead = true; return; }
            const lob::OrderId id = ids.find(c.owner, m->id);
            const bool ok = book.cancel(id);
            if (ok) ids.forget(id);
            send_exec(c, m->id, 0,
                      ok ? lobnet::ExecStatus::Canceled
                         : lobnet::ExecStatus::Unknown);
            break;
        }
        case lobnet::MsgType::Modify: {
            const auto m = lobnet::decode_modify(body, len);
            if (!m) { c.dead = true; return; }
            const lob::OrderId id = ids.find(c.owner, m->id);
            const auto side = ids.side(id);
            if (side) feed.set_taker_side(*side); // a reprice can trade
            const auto r = book.modify(id, m->new_px, m->new_qty);
            // A reprice re-enters under the same engine id. It may rest, or
            // trade away completely, or be stopped by STP.
            if (r.ok && r.rested)
                ids.rest(id, c.owner, m->id, m->new_qty - r.filled, *side);
            else if (r.ok)
                ids.forget(id);
            // Rejected means the amend failed and the order is still live,
            // so an order that is gone after a reprice is reported by how
            // it ended.
            lobnet::ExecStatus st = lobnet::ExecStatus::Unknown;
            if (!r.ok)                       st = r.found ? lobnet::ExecStatus::Rejected
                                                          : lobnet::ExecStatus::Unknown;
            else if (r.rested)               st = lobnet::ExecStatus::Replaced;
            else if (r.filled == m->new_qty) st = lobnet::ExecStatus::Filled;
            else                             st = r.filled ? lobnet::ExecStatus::PartialDone
                                                           : lobnet::ExecStatus::Canceled;
            send_exec(c, m->id, r.filled, st);
            break;
        }
        default:
            c.dead = true; // client->gateway direction never carries other types
            return;
    }
    feed.publish_book(book);
}

// --- disconnect -------------------------------------------------------------
// Ids are scoped to the connection, so once it is gone nobody can name its
// orders. Cancel them rather than leave them resting with no owner. Returns
// how many were canceled; one book snapshot follows if any were.

inline std::size_t cancel_on_disconnect(Engine& book, FeedPublisher& feed,
                                        lob::Owner owner) {
    const auto ids = feed.orders.owned_by(owner);
    for (const lob::OrderId id : ids) {
        book.cancel(id);
        feed.orders.forget(id);
    }
    if (!ids.empty()) feed.publish_book(book);
    return ids.size();
}

} // namespace lobnet
