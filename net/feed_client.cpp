// Market-data subscriber: binds the UDP feed port, decodes Trade and Book
// messages, and tracks sequence gaps -- the thing every real feed handler
// must do, because UDP delivers best-effort.
//
//   --summary-every N   print one status line per N messages (default 500)
//   --max N             exit after N messages (0 = run until SIGINT)
//   --group A           join multicast group A on the loopback-capable default
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

} // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 9002;
    long          max_msgs = 0;
    long          every = 500;
    std::string   group;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::uint16_t(atoi(argv[++i]));
        else if (a == "--max" && i + 1 < argc) max_msgs = atol(argv[++i]);
        else if (a == "--summary-every" && i + 1 < argc) every = atol(argv[++i]);
        else if (a == "--group" && i + 1 < argc) group = argv[++i];
        else { std::fprintf(stderr, "usage: feed_client [--port P] [--max N] [--summary-every N] [--group A]\n"); return 1; }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }
    if (!group.empty()) {
        ip_mreq req{};
        inet_pton(AF_INET, group.c_str(), &req.imr_multiaddr);
        req.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &req, sizeof(req)) < 0)
            std::perror("join multicast (continuing unicast)");
    }

    // recv timeout so --max/SIGINT are honored even on a silent feed
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::uint64_t last_seq = 0, gaps = 0, trades = 0, books = 0, bad = 0;
    lob::Price    last_bid = 0, last_ask = 0;
    long          n_msgs = 0;

    while (!g_stop && (max_msgs == 0 || n_msgs < max_msgs)) {
        std::uint8_t buf[2048];
        const ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            std::perror("recvfrom");
            break;
        }
        // One frame per datagram: [len u16][type u8][payload]
        if (n < long(lobnet::kHeaderLen + 1)) { ++bad; continue; }
        std::uint16_t len;
        std::memcpy(&len, buf, sizeof(len));
        if (len == 0 || len > lobnet::kMaxFrameLen ||
            std::size_t(n) != lobnet::kHeaderLen + len) { ++bad; continue; }
        const std::uint8_t* body = buf + lobnet::kHeaderLen;

        std::uint64_t seq = 0;
        const auto type = lobnet::peek_type(body, len);
        if (!type) { ++bad; continue; }
        if (*type == lobnet::MsgType::Trade) {
            const auto t = lobnet::decode_trade(body, len);
            if (!t) { ++bad; continue; }
            seq = t->seq;
            ++trades;
        } else if (*type == lobnet::MsgType::Book) {
            const auto b = lobnet::decode_book(body, len);
            if (!b) { ++bad; continue; }
            seq      = b->seq;
            last_bid = b->bid_px[0];
            last_ask = b->ask_px[0];
            ++books;
        } else {
            ++bad;
            continue;
        }

        if (last_seq != 0 && seq != last_seq + 1) ++gaps;
        last_seq = seq;
        ++n_msgs;

        if (every > 0 && n_msgs % every == 0)
            std::printf("feed: %ld msgs (%llu trades, %llu books), gaps=%llu, "
                        "top: %lld / %lld\n",
                        n_msgs, (unsigned long long)trades,
                        (unsigned long long)books, (unsigned long long)gaps,
                        (long long)last_bid, (long long)last_ask);
    }

    std::printf("feed summary: msgs=%ld trades=%llu books=%llu gaps=%llu bad=%llu "
                "final top: %lld / %lld\n",
                n_msgs, (unsigned long long)trades, (unsigned long long)books,
                (unsigned long long)gaps, (unsigned long long)bad,
                (long long)last_bid, (long long)last_ask);
    ::close(fd);
    return 0;
}
