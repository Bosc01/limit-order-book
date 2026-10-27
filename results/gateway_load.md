# Gateway load test

Closed-loop clients against the TCP order gateway over loopback. Each client
is a thread with one persistent connection and one order in flight: it sends
the next order when the exec report for the previous one arrives. Every client
runs the seeded `order_client --auto` mix (60% passive limits, 15% aggressive
limits, 10% market orders, 10% cancels, 5% modifies) with seed 42 plus its
thread index. An order here is any of those five messages; each gets exactly
one exec report. Each run starts a fresh gateway, warms up for 1 s, then
measures for 5 s. Round trips are measured at the client, from send()
to the last byte of the exec report, and pooled across clients. Every figure
in the first table is the median of 5 runs.

* command: `scripts/gateway_load.sh build` (RUNS=5 WARMUP=1 DURATION=5)
* started: 2026-10-27 23:10 UTC
* commit: 9fbd47e
* machine: Apple, 8 logical CPUs, virtualization: apple
* kernel: Linux 6.8.0-142-generic aarch64
* compiler: c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
* load average at start: 0.87, 2.26, 1.66
* host: Apple M5 (4 performance + 6 efficiency cores), macOS 26.6.2 (Darwin 25.6.0), Lima 2.2.1 vz VM with 8 vCPUs and 4 GiB; host load average at start: 4.06 5.68 5.39

| clients | orders/s | p50 round trip (us) | p99 round trip (us) |
|--:|--:|--:|--:|
| 1 | 19292 | 51.1 | 72.8 |
| 8 | 49981 | 135.6 | 306.6 |
| 32 | 51289 | 566.2 | 1320.2 |
| 128 | 52528 | 2326.6 | 3336.1 |

## Cancel on disconnect

At the end of every run each client closes its connection with orders still
resting. The gateway cancels a connection's resting orders when it sees the
close. Once it has logged all of the disconnects, the gateway is stopped and
reports how many orders remain on its book. The script fails if that count is
not zero, or if no orders were resting to cancel.

Resting orders after disconnect: 0 in all 20 runs.

## Every run

| clients | run | orders/s | p50 (us) | p99 (us) | canceled on disconnect | resting after |
|--:|--:|--:|--:|--:|--:|--:|
| 1 | 1 | 18820 | 51.5 | 76.9 | 52593 | 0 |
| 1 | 2 | 18696 | 52.0 | 72.8 | 53341 | 0 |
| 1 | 3 | 19292 | 51.1 | 67.8 | 54847 | 0 |
| 1 | 4 | 20196 | 50.2 | 66.6 | 57375 | 0 |
| 1 | 5 | 23564 | 34.0 | 114.1 | 66206 | 0 |
| 8 | 1 | 54410 | 124.2 | 285.8 | 152180 | 0 |
| 8 | 2 | 46962 | 145.0 | 324.5 | 137398 | 0 |
| 8 | 3 | 49981 | 135.6 | 306.6 | 139088 | 0 |
| 8 | 4 | 48255 | 140.7 | 320.3 | 138951 | 0 |
| 8 | 5 | 50256 | 134.3 | 304.9 | 141376 | 0 |
| 32 | 1 | 51598 | 561.9 | 1316.3 | 149123 | 0 |
| 32 | 2 | 65904 | 431.5 | 1068.5 | 182742 | 0 |
| 32 | 3 | 47573 | 657.9 | 1441.5 | 146925 | 0 |
| 32 | 4 | 51289 | 566.2 | 1320.2 | 148188 | 0 |
| 32 | 5 | 42826 | 584.0 | 2440.6 | 128332 | 0 |
| 128 | 1 | 50842 | 2352.2 | 3665.1 | 148785 | 0 |
| 128 | 2 | 51897 | 2332.3 | 3045.3 | 149500 | 0 |
| 128 | 3 | 52893 | 2314.7 | 3207.9 | 152526 | 0 |
| 128 | 4 | 52528 | 2326.6 | 3336.1 | 151834 | 0 |
| 128 | 5 | 89121 | 1127.8 | 5799.5 | 249082 | 0 |

## Reading these numbers

* Clients and gateway share one machine. With more client threads than
  cores, clients compete with the gateway thread for CPU, so round trips at
  high client counts include client-side scheduling delay.
* The gateway is one thread on one poll() loop, and it sends a UDP book
  snapshot after every order. No feed subscriber runs during the test; the
  datagrams are sent all the same.
* Closed loop: no client has more than one order outstanding, so the offered
  load falls whenever the gateway slows down. The percentiles describe
  service time at each concurrency, not latency at a fixed arrival rate.
