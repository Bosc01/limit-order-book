// Order-entry gateway + market-data publisher.
//
//   * TCP  (default :9001): order entry. Reliable, ordered, per-client -- an
//     order MUST arrive exactly once and the sender must know if it did not.
//   * UDP  (default 127.0.0.1:9002, multicast groups supported): market
//     data. One publisher, many subscribers, freshest-data-wins; a lost
//     tick is repaired by the next snapshot, not by retransmission delay.
//
// Single thread, one poll() loop, engine inline: "real, not fast". The
// matching engine itself stays single-threaded on purpose; a production
// system would pin this thread and hand fills to other cores via queues.
//
// Security posture (the peer is hostile):
//   * every frame is length-checked and field-decoded with bounds checks;
//     any malformed byte poisons the connection and it is dropped
//   * the participant id (owner) used for self-trade prevention is ASSIGNED
//     by the gateway per connection -- a client cannot spoof someone else's
//   * order ids are scoped per connection and mapped to engine ids the
//     gateway assigns -- a client can cancel or amend only its own orders,
//     and another client's live id answers exactly like an unused one
//   * slow consumers are disconnected when their outbound buffer fills;
//     a stalled client must never stall the market
//   * SIGPIPE is ignored; a peer resetting mid-send cannot kill the process
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "gateway.hpp"

using namespace lobnet;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int set_nonblock(int fd) { return fcntl(fd, F_SETFL, O_NONBLOCK); }

void flush_outbox(Connection& c) {
    while (!c.outbox.empty()) {
        const ssize_t n = ::send(c.fd, c.outbox.data(), c.outbox.size(), 0);
        if (n > 0) {
            c.outbox.erase(c.outbox.begin(), c.outbox.begin() + n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        c.dead = true; // real error
        return;
    }
}

int make_listener(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("socket"); return -1; }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // demo binds loopback only
    addr.sin_port        = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return -1;
    }
    if (listen(fd, 16) < 0) { std::perror("listen"); return -1; }
    set_nonblock(fd);
    return fd;
}

} // namespace

int main(int argc, char** argv) {
    std::uint16_t port     = 9001;
    std::string   udp_ip   = "127.0.0.1";
    std::uint16_t udp_port = 9002;
    lob::Stp      stp      = lob::Stp::None;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::uint16_t(atoi(argv[++i]));
        else if (a == "--udp-ip" && i + 1 < argc) udp_ip = argv[++i];
        else if (a == "--udp-port" && i + 1 < argc) udp_port = std::uint16_t(atoi(argv[++i]));
        else if (a == "--stp" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "none") stp = lob::Stp::None;
            else if (v == "cancel-resting") stp = lob::Stp::CancelResting;
            else if (v == "reject-incoming") stp = lob::Stp::RejectIncoming;
            else { std::fprintf(stderr, "bad --stp value\n"); return 1; }
        }
        else { std::fprintf(stderr, "usage: gateway [--port N] [--udp-ip A] [--udp-port N] [--stp none|cancel-resting|reject-incoming]\n"); return 1; }
    }

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    Engine book(stp); // FeedPublisher listener wired in below
    FeedPublisher& feed = book.listener();
    if (!feed.open(udp_ip.c_str(), udp_port)) {
        std::fprintf(stderr, "cannot open UDP feed to %s:%u\n", udp_ip.c_str(), udp_port);
        return 1;
    }

    const int listener = make_listener(port);
    if (listener < 0) return 1;
    std::printf("gateway: orders on tcp/127.0.0.1:%u, feed to udp/%s:%u\n",
                port, udp_ip.c_str(), udp_port);

    std::vector<Connection> conns;
    lob::Owner next_owner = 1;

    // Route STP kill notifications back to the owning client as an
    // unsolicited cancel report, under the client's own order id.
    feed.on_unsolicited_cancel = [&conns](lob::OrderId client, lob::Owner owner) {
        for (auto& c : conns)
            if (c.owner == owner && !c.dead) {
                send_exec(c, client, 0, lobnet::ExecStatus::Canceled);
                break;
            }
    };

    while (!g_stop) {
        std::vector<pollfd> pfds;
        pfds.push_back({listener, POLLIN, 0});
        for (auto& c : conns) {
            short ev = POLLIN;
            if (!c.outbox.empty()) ev |= POLLOUT;
            pfds.push_back({c.fd, ev, 0});
        }
        const int rc = ::poll(pfds.data(), nfds_t(pfds.size()), 250);
        if (rc < 0) {
            if (errno == EINTR) continue;
            std::perror("poll");
            break;
        }

        if (pfds[0].revents & POLLIN) {
            const int cfd = ::accept(listener, nullptr, nullptr);
            if (cfd >= 0) {
                set_nonblock(cfd);
                const int one = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                Connection c;
                c.fd    = cfd;
                c.owner = next_owner++;
                conns.push_back(std::move(c));
                std::printf("gateway: client %u connected\n", conns.back().owner);
            }
        }

        for (std::size_t i = 0; i < conns.size(); ++i) {
            Connection& c = conns[i];
            const auto& p = pfds[i + 1];
            if (p.revents & (POLLERR | POLLHUP)) { c.dead = true; continue; }
            if (p.revents & POLLIN) {
                std::uint8_t buf[4096];
                while (true) {
                    const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
                    if (n > 0) {
                        const bool ok = c.frames.feed(
                            buf, std::size_t(n),
                            [&](const std::uint8_t* body, std::size_t len) {
                                handle_frame(book, feed, c, body, len);
                            });
                        if (!ok || c.dead) { c.dead = true; break; }
                        continue;
                    }
                    if (n == 0) { c.dead = true; break; } // orderly close
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    c.dead = true;
                    break;
                }
            }
            if (!c.dead && (p.revents & POLLOUT)) flush_outbox(c);
            if (!c.dead && !c.outbox.empty()) flush_outbox(c);
        }

        for (std::size_t i = conns.size(); i-- > 0;) {
            if (conns[i].dead) {
                std::printf("gateway: client %u disconnected\n", conns[i].owner);
                ::close(conns[i].fd);
                conns.erase(conns.begin() + long(i));
            }
        }
    }

    for (auto& c : conns) ::close(c.fd);
    ::close(listener);
    std::printf("gateway: bye (resting orders: %zu)\n", book.resting_orders());
    return 0;
}
