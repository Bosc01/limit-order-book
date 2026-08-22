// Order-entry client. Two modes:
//   --auto N   deterministic random burst of N orders (seeded), then a
//              summary of exec-report statuses — used by the smoke test
//   (default)  reads simple commands from stdin:
//              buy <px> <qty> | sell <px> <qty> | mkt <b|s> <qty>
//              cancel <id> | modify <id> <px> <qty> | quit
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "framing.hpp"
#include "protocol.hpp"

namespace {

int connect_to(const char* ip, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("socket"); return -1; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        std::perror("connect");
        return -1;
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

bool send_all(int fd, const std::uint8_t* p, std::size_t n) {
    while (n > 0) {
        const ssize_t k = ::send(fd, p, n, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += k;
        n -= std::size_t(k);
    }
    return true;
}

const char* status_name(lobnet::ExecStatus s) {
    switch (s) {
        case lobnet::ExecStatus::Accepted:    return "ACCEPTED";
        case lobnet::ExecStatus::Filled:      return "FILLED";
        case lobnet::ExecStatus::PartialRest: return "PARTIAL_REST";
        case lobnet::ExecStatus::PartialDone: return "PARTIAL_DONE";
        case lobnet::ExecStatus::Rejected:    return "REJECTED";
        case lobnet::ExecStatus::Canceled:    return "CANCELED";
        case lobnet::ExecStatus::Replaced:    return "REPLACED";
        case lobnet::ExecStatus::Unknown:     return "UNKNOWN_ID";
    }
    return "?";
}

// Drain pending exec reports; expect_at_least blocks until that many arrive.
int read_reports(int fd, lobnet::FrameBuffer& fb, int expect_at_least,
                 bool verbose, int counts[8]) {
    int seen = 0;
    std::uint8_t buf[4096];
    while (seen < expect_at_least) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        const bool ok = fb.feed(buf, std::size_t(n),
            [&](const std::uint8_t* body, std::size_t len) {
                const auto m = lobnet::decode_exec_report(body, len);
                if (!m) return;
                ++seen;
                ++counts[static_cast<int>(m->status)];
                if (verbose)
                    std::printf("  exec: id=%llu %s filled=%u\n",
                                (unsigned long long)m->id,
                                status_name(m->status), m->filled);
            });
        if (!ok) return -1;
    }
    return seen;
}

int run_auto(int fd, int n_orders, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    lobnet::FrameBuffer fb;
    int counts[8] = {0};
    std::uint64_t next_id = 1;
    std::uint8_t buf[128];

    for (int i = 0; i < n_orders; ++i) {
        const std::uint64_t r = rng() % 100;
        std::size_t len = 0;
        if (r < 60 || next_id < 10) { // passive-ish limit
            const lob::Side s = (rng() & 1) ? lob::Side::Ask : lob::Side::Bid;
            const lob::Price px =
                10'000 + (s == lob::Side::Bid ? -1 : 1) * lob::Price(1 + rng() % 8);
            len = lobnet::encode(buf, sizeof(buf),
                                 lobnet::NewOrderMsg{next_id++, px,
                                                     lob::Qty(1 + rng() % 100),
                                                     s, lob::Tif::GTC});
        } else if (r < 75) { // aggressive limit
            const lob::Side s = (rng() & 1) ? lob::Side::Ask : lob::Side::Bid;
            const lob::Price px =
                10'000 + (s == lob::Side::Bid ? 1 : -1) * lob::Price(rng() % 3);
            len = lobnet::encode(buf, sizeof(buf),
                                 lobnet::NewOrderMsg{next_id++, px,
                                                     lob::Qty(1 + rng() % 50),
                                                     s, lob::Tif::GTC});
        } else if (r < 85) {
            len = lobnet::encode(buf, sizeof(buf),
                                 lobnet::MarketMsg{next_id++, lob::Qty(1 + rng() % 20),
                                                   (rng() & 1) ? lob::Side::Ask
                                                               : lob::Side::Bid});
        } else if (r < 95) {
            len = lobnet::encode(buf, sizeof(buf),
                                 lobnet::CancelMsg{1 + rng() % (next_id ? next_id : 1)});
        } else {
            len = lobnet::encode(buf, sizeof(buf),
                                 lobnet::ModifyMsg{1 + rng() % (next_id ? next_id : 1),
                                                   10'000 - lob::Price(1 + rng() % 5),
                                                   lob::Qty(1 + rng() % 30)});
        }
        if (!len || !send_all(fd, buf, len)) return 1;
        if (read_reports(fd, fb, 1, false, counts) < 0) return 1;
    }

    std::printf("auto mode: %d ops acknowledged\n", n_orders);
    for (int s = 0; s < 8; ++s)
        if (counts[s])
            std::printf("  %-13s %d\n",
                        status_name(static_cast<lobnet::ExecStatus>(s)), counts[s]);
    return 0;
}

int run_interactive(int fd) {
    lobnet::FrameBuffer fb;
    int counts[8] = {0};
    std::uint64_t next_id = 1;
    std::uint8_t buf[128];
    char line[256];

    std::printf("commands: buy <px> <qty> | sell <px> <qty> | mkt <b|s> <qty> | "
                "cancel <id> | modify <id> <px> <qty> | quit\n> ");
    while (std::fgets(line, sizeof(line), stdin)) {
        char cmd[16] = {0};
        long long a = 0, b2 = 0, c = 0;
        char sc = 0;
        std::size_t len = 0;
        if (std::sscanf(line, "%15s", cmd) != 1) { std::printf("> "); continue; }
        const std::string s = cmd;
        if (s == "quit") break;
        if ((s == "buy" || s == "sell") &&
            std::sscanf(line, "%*s %lld %lld", &a, &b2) == 2) {
            len = lobnet::encode(buf, sizeof(buf),
                lobnet::NewOrderMsg{next_id, a, lob::Qty(b2),
                                    s == "buy" ? lob::Side::Bid : lob::Side::Ask,
                                    lob::Tif::GTC});
            std::printf("  sent id=%llu\n", (unsigned long long)next_id);
            ++next_id;
        } else if (s == "mkt" && std::sscanf(line, "%*s %c %lld", &sc, &a) == 2) {
            len = lobnet::encode(buf, sizeof(buf),
                lobnet::MarketMsg{next_id++, lob::Qty(a),
                                  sc == 'b' ? lob::Side::Bid : lob::Side::Ask});
        } else if (s == "cancel" && std::sscanf(line, "%*s %lld", &a) == 1) {
            len = lobnet::encode(buf, sizeof(buf), lobnet::CancelMsg{std::uint64_t(a)});
        } else if (s == "modify" &&
                   std::sscanf(line, "%*s %lld %lld %lld", &a, &b2, &c) == 3) {
            len = lobnet::encode(buf, sizeof(buf),
                lobnet::ModifyMsg{std::uint64_t(a), b2, lob::Qty(c)});
        } else {
            std::printf("parse error\n> ");
            continue;
        }
        if (!len || !send_all(fd, buf, len)) return 1;
        if (read_reports(fd, fb, 1, true, counts) < 0) return 1;
        std::printf("> ");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char*   ip    = "127.0.0.1";
    std::uint16_t port  = 9001;
    int           autoN = 0;
    std::uint64_t seed  = 42;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--auto" && i + 1 < argc) autoN = atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--port" && i + 1 < argc) port = std::uint16_t(atoi(argv[++i]));
        else { std::fprintf(stderr, "usage: order_client [--auto N] [--seed S] [--port P]\n"); return 1; }
    }

    const int fd = connect_to(ip, port);
    if (fd < 0) return 1;
    const int rc = autoN > 0 ? run_auto(fd, autoN, seed) : run_interactive(fd);
    ::close(fd);
    return rc;
}
