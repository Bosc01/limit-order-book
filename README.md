# Limit Order Book Matching Engine

[![ci](https://github.com/Bosc01/limit-order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/Bosc01/limit-order-book/actions/workflows/ci.yml)

A price-time priority matching engine for one instrument, in C++20, with a
TCP order gateway and a UDP market data feed. I wrote it to learn how
exchange matching engines are built. Each optimization is kept as its own
engine in `engines/`: seven engines, so six steps, each measured against the
one before it on the same workload. Two of the six steps, v3 and v4,
measured no change. Correctness is checked against `naive`: every engine
replays the benchmark workload in lockstep with it, and randomized property
tests audit the final engine after every operation.

Final engine (`include/lob/`) on an Apple M5, one thread, median of five runs
of 2M operations (seed 42: 45% passive limit, 10% crossing limit, 5% market,
30% cancel, 10% modify). One operation is one call into the engine: a limit
order, a market order, a cancel, or a modify. A crossing order counts as one
operation however many fills it makes.

<!-- BEGIN generated: headline final-2026-10-28 final -->
| metric | value |
|---|---|
| throughput, no per-operation timestamps | 67.6M ops/s |
| throughput, counter read around every operation | 29.3M ops/s |
| mean cost of one engine call, closed loop | 24.9 ns |
| p50 / p99 / p99.9, in 41.7 ns counter steps | 41 / 83 / 125 ns |
| heap allocations in the measured region | zero in 5 of 5 runs |
| 1-minute load average when the runs started | 4.81 |
<!-- END generated -->

Throughput comes from a pass with no timestamps. Latency comes from a second
pass over the same operations that reads the system counter around each one.
Each read sits behind an `isb`, which serializes the pipeline, so that pass
runs at less than half the rate. The p50 is one step of the counter, which
advances every 41.7 ns. The August 2026 figure of 26.9M ops/s, quoted
as 27M (`results/final_2m.csv`), had only the timed pass, so it compares
with the timed row, not the first. Comparisons interleave their builds run
by run, an A/A batch measures the noise, and batches that failed the
validity rule stay in the tables. Method, machine conditions, and every run:
[docs/benchmarks.md](docs/benchmarks.md#method).

## Design

```
   order entry (TCP, gateway assigns owner id)     market data out (UDP, seq-numbered)
                    |                                          ^
                    v                                          |
 +------------------------- matching engine, ONE thread -------------------------+
 |                                                                               |
 |   price ladder (per side)         orders within a level        id index       |
 |   ---------------------------     ----------------------      -------------   |
 |   flat Level array indexed        intrusive doubly linked     open-addressing |
 |   by price tick                   FIFO list: head = oldest    hash map        |
 |   + occupancy bitmap              = fills first; the links    id -> Order*    |
 |   (1 bit per tick, scan by        live INSIDE the Order,      linear probing, |
 |   count-zeros instruction)        so no list nodes exist      backward-shift  |
 |   + cached best-price cursor                                  deletion        |
 |                                                                               |
 |   every Order is a 64-byte, 64-aligned slot in a pre-sized slab pool          |
 +-------------------------------------------------------------------------------+
```

* Ladder: each side is a flat array of `Level`s, one per tick across a fixed
  band (32,767 ticks by default), with an occupancy bitmap and a cached best
  price. Finding a level is an index. When the best level empties, the next
  occupied tick is found with `clz` or `ctz` over the bitmap, 64 ticks per
  word.
* Queue: each level is a doubly linked FIFO threaded through the orders, so
  an `Order*` is enough to unlink it. The head is the oldest order.
* Id index: open addressing from order id to `Order*`, linear probing, load
  factor at most 0.5, backward-shift deletion so cancels leave no
  tombstones. The hash is Fibonacci (multiply by 2^64/φ, keep the top bits),
  which spreads the sequential ids a gateway issues.
* Memory: an `Order` is a 64-byte slot on a 64-byte boundary, so it never
  straddles a cache line (128 bytes on the M5), taken from a pre-faulted slab
  pool with an intrusive free list. The ladder, bitmap, pool, and id map are
  sized at construction. The pool and id map can grow if undersized; both
  count growth events, and the benchmark counts every heap allocation in the
  measured region.

### Operation costs

F is the number of resting orders an incoming order fills or removes, L the
number of price levels it crosses, and d the number of ticks the best-price
scan walks: the gap to the next occupied level after a cancel, or the span a
crossing order sweeps.

| Operation | Cost | Where the time goes |
|---|---|---|
| Add, does not cross | O(1) expected | duplicate-id probe, append at the level tail, id-map insert, set one bitmap bit |
| Add that crosses, or market order | O(F + L + d/64) expected | constant work per fill; one bitmap scan each time the best level empties |
| Cancel | O(1) expected, plus O(d/64) if it empties the best level | id-map lookup and erase, unlink through the order's own pointers, return the slot to the pool |
| Modify, same or smaller size at the same price | O(1) expected | amended in place, keeps queue position |
| Modify, any other change | cancel plus add | loses queue position |
| Best bid or ask | O(1) | cached per side |
| FOK pre-check | O(L + d/64) | sums level totals; walks individual orders only when self-trade prevention is on |

"Expected" is the id map: linear probing at load 0.5 averages 1.5 probes for
a hit and 2.5 for a miss if the hash spreads keys evenly. The worst-case
bitmap scan is the whole band, 512 words per side. Pool growth and id-map
rehash are O(n) and fire only when the engine is undersized. A 100M-operation
run sized for its session made zero heap allocations in its measured region,
so neither fired (`results/endurance_2026-10-22.txt`, load average 7.49 at
the start). The first endurance runs, in August 2026, predate load recording
(`results/endurance.csv`).

A flat array instead of a tree, because prices are integer ticks inside a
bounded band and the index already sorts them. The `std::map` ladder in
`engines/book_v1.hpp` pays O(log L) to create or erase a level and chases
nodes across the heap. The cost is the band: prices outside it are rejected,
and each tick holds a 64-byte `Level` per side whether or not anything rests
there, 2 MB per side at the default band.

No lazy deletion. It exists for heap-based ladders, which cannot remove from
the middle. Here a cancel unlinks through the order's own pointers, reaches
its level through a back-pointer, and clears one bit if the level empties.

## Semantics

Limit and market orders, cancel, and modify, with time in force GTC, IOC,
and FOK. FOK checks that the full size is reachable before it touches the
book, so a failed FOK changes nothing.

Modify keeps queue position only for a size decrease at the same price. A
price change or a size increase is a cancel plus a new arrival, which may
cross. The result tells "unknown id" apart from "rejected amend on a live
order".

Self-trade prevention is off, cancel-resting, or reject-incoming, chosen at
construction. When it is off, the check is compiled out of the match loop.
Owner 0 means no STP group: an order with owner 0 never triggers
self-trade prevention and is never canceled by it. A cancel-resting kill
emits an unsolicited cancel so the resting order's owner hears about it.

Market orders can carry a protection limit: the number of ticks past the
touch a sweep may go. The remainder is discarded.

## Optimization steps

`naive` (`std::map` of `std::list`, cancel by linear search) is the
reference the other engines are tested against. v2 to v5 each change one
thing. v1 changes two at once: the per-level `std::list` becomes an
intrusive list, and a hash index replaces the linear search, both on the
`std::map` ladder `naive` already uses. The final step also combines two:
open addressing and Fibonacci hashing. The hash was measured on its own only
in [docs/benchmarks.md](docs/benchmarks.md#profile-driven-attempts-october-2026).
Five interleaved runs per engine of 200k operations after a 10k
warmup, fewer than the headline because `naive` slows as its book grows
(`results/progression-2026-10-28/`). The final engine reads higher here than
in the headline. These runs end with 19,040 resting orders against 60,225
after 2M operations, warm up for 10k operations instead of 100k, and come
from a different batch at a different load. Which of these accounts for the
gap was not isolated.

<!-- BEGIN generated: progression progression-2026-10-28 -->
| engine | change | untimed Mops/s | against previous | timed Mops/s | mean ns | p50 | p90 | p99 | p99.9 | allocs per op | load |
|---|---|---|---|---|---|---|---|---|---|---|---|
| naive | std::map + std::list, O(n) cancel | 0.15 (0.14 to 0.15) |  | 0.15 | 6571.7 | 42 | 22643 | 48577 | 60201 | 0.402 | 3.42 to 3.60 |
| v1 | map ladder, intrusive list, hash index | 25.5 (25.4 to 25.7) | +16888.2% | 17.2 | 49.0 | 42 | 84 | 125 | 208 | 0.797 | 3.31 to 3.60 |
| v2 | + slab pool for orders | 32.2 (31.8 to 32.6) | +25.9% | 20.1 | 40.7 | 42 | 83 | 125 | 167 | 0.402 | 3.31 to 3.60 |
| v3 | + Order aligned to 64 bytes | 32.1 (31.6 to 32.4) | 0.0%, ranges overlap | 20.1 | 40.5 | 42 | 83 | 125 | 167 | 0.402 | 3.31 to 3.60 |
| v4 | + branch layout in the match loop | 32.8 (32.2 to 33.1) | +2.0%, ranges overlap | 20.1 | 40.5 | 42 | 83 | 125 | 167 | 0.402 | 3.31 to 3.60 |
| v5 | + flat banded ladder with bitmap | 37.2 (36.7 to 37.6) | +13.4% | 22.0 | 36.4 | 42 | 42 | 84 | 167 | 0.396 | 3.31 to 3.56 |
| final | + open-addressing id map, Fibonacci hash | 78.2 (77.1 to 78.9) | +110.0% | 30.6 | 23.6 | 41 | 42 | 83 | 125 | 0.000 | 3.31 to 3.56 |
<!-- END generated -->

A step is called faster only when every one of its runs beats every run of
the previous engine; "ranges overlap" marks the others.

* v1: in `naive`, cancel and modify average 15.5 and 18.4 us because they
  search the book, which ends each run with 19,040 resting orders. The id
  index brings them to 32.2 and 42.1 ns.
* v2: the pool beat v1 in every run.
* v3 (64-byte aligned `Order`) and v4 (branch layout in the match loop): no
  measured throughput change. Both are kept. The alignment is not free in
  memory: it pads each order from 56 to 64 bytes, 8 bytes or 14.2% more per
  order. One hypothesis for the null result is that the working set fits in
  cache and the branches are well predicted; no cache-miss or branch-miss
  counts were taken, so it is untested.
* v5: the flat ladder beat v4 in every run, 13% at the median.
* final: the open-addressing id map more than doubles v5 (+110%) and
  removes the last allocations. v1 to v5 still allocate a node for every
  `std::unordered_map` insert.
* Timed and untimed throughput diverge as the engines get faster. The
  counter reads cost about the same per operation in every engine, so they
  are a larger share of a faster one.

## Verification

425 GoogleTest cases, the suite CI runs on Linux, on macOS, and under UBSan.
llvm-cov measures 97.4% line and 98.9% function coverage over the engines,
the wire protocol, and the gateway's order handling
(`results/coverage-2026-10-28.txt`). The socket loop in `net/gateway.cpp` is
not in the test binary; `scripts/net_smoke.sh` exercises it, unmeasured.

* One typed suite runs against all seven engines. `naive` is the
  specification: every engine must return the same results and print the
  same trade tape (taker, maker, price, quantity, in order).
* Differential replay: the benchmark workload runs through `naive` and each
  engine in lockstep, comparing every result, trade, and best price, with
  periodic full audits (`check_invariants()` walks every link, level total,
  bitmap bit, and id-map entry). The longest pair runs 1M operations.
* Property tests: 100,000 seeded random operations per STP policy, checked
  after every operation against a model the test keeps. The book is never
  crossed or locked, cached best prices and per-level quantities match the
  model, and every trade takes the earliest live order at its price, at that
  order's price. Quantity is checked against the input: each order splits
  into its fill, what appeared on the book, and the remainder its type
  discards, and a cancel takes exactly the order's quantity off the book.
* `scripts/mutation_check.sh`, run in CI, rebuilds the property and
  differential suites against six deliberately broken copies of the final
  engine, and each is caught. Both suites catch a fill missing from the
  book, a cancel that leaves the order resting, and two time-priority
  breaks (matching newest first, resting new orders at the head of their
  level). Only the property suite catches the two RejectIncoming faults
  (canceling the resting order instead, resting the remainder after a
  stop). The differential test does not cover self-trade prevention,
  because `naive` ignores it, so the property suite is the only check there.
* Edge cases: TIF and STP combinations, FOK atomicity under both STP modes,
  duplicate and reused ids, band edges, bitmap scans across word boundaries,
  pool growth, and 200k random id-map operations mirrored against
  `std::unordered_map`.
* The suite passes under UBSan in CI and under ASan with UBSan locally.
* The frame decoders reject truncated bodies, trailing bytes, and bad enums.
  The stream reassembler is fed valid streams in every chunk size from one
  byte up, 2000 random-length frames in random chunk sizes, and random
  garbage.
* `scripts/net_smoke.sh` runs the gateway, a UDP subscriber, and a
  1000-order burst, and checks that every order is acknowledged and the feed
  has no sequence gaps.
* Two semantic bugs were found after the first version. Both have regression
  tests in all six indexed engines. STP fired between unrelated orders that
  both left the owner id at its default (owner 0 is now exempt), and a
  cancel-resting kill was invisible to the resting order's owner (the engine
  now emits `on_cancel`, and the gateway forwards it).
* A review in October 2026 found five gateway bugs, fixed in `69e502c`. The
  poll loop read one entry past the end of its descriptor array after an
  accept. Frames after a malformed frame in the same read still reached the
  book. A market order that partly filled was reported Filled, and one with
  a rejected id Canceled. A reprice that traded away or was stopped by STP
  was reported Replaced. A crossing reprice published its trades with the
  previous message's taker side. The middle three have regression tests in
  `tests/gateway_test.cpp`. The poll-loop fix is in the socket loop, outside
  the test binary, and the taker-side fix has no test yet.

## Gateway and feed

`gateway` runs the engine on one thread with a poll() loop: binary order
entry over TCP, book updates over UDP (unicast or multicast) with sequence
numbers. `order_client` and `feed_client` are the other ends.

* Frames are length-capped and decoded with bounds checks. After a
  malformed frame nothing more from that connection is executed; reports
  already owed are flushed and the connection is closed.
* The gateway assigns each connection its STP owner id, so a client cannot
  claim another's.
* Each connection has its own order id space, mapped to engine ids that the
  gateway assigns. A client can cancel or amend only its own orders, and an
  id that belongs to another client gets the same reply as an unknown one.
* Order ids never appear on the public feed.
* A client that stops reading is disconnected instead of buffered without
  bound. When a connection closes for any reason, the gateway cancels its
  resting orders.

Round trips, measured at the client from send() to the last byte of the
exec report:

* macOS loopback, one client, 20k synchronous orders, load average 8.1:
  p50 31.5 us, p99 58.4 us, p99.9 90.5 us (`results/wire_2026-10-22.txt`).
  The engine's mean is 25 ns, so nearly all of that is outside the engine.
* Linux VM on the same machine (Lima, 8 vCPUs, 4 GiB), 1 to 128 closed-loop
  clients (`results/gateway_load.md`): throughput levels off near 50k orders/s from
  8 clients up while round trips grow with the client count. That plateau
  points to the gateway thread rather than the engine; the gateway was not
  profiled. It publishes a UDP book snapshot
  after every order. Every run ends with the clients disconnecting while
  orders rest; none were left after any of the 20 runs.

## Build and run

CMake 3.20+ and a C++20 compiler. GoogleTest is fetched at configure time
unless a copy is in `third_party/`.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/lob_tests                            # 425 tests
./build/bench final                          # one run of the final engine
./build/bench naive                          # the reference engine
./scripts/bench_report.sh mine final=build   # five runs saved under results/mine/
python3 scripts/results.py readme            # regenerate the tables from results/
./scripts/profile.py                         # sample the untimed pass (macOS)
./scripts/net_smoke.sh build                 # gateway, feed, and client end to end
./scripts/gateway_load.sh build              # 1 to 128 clients, writes results/gateway_load.md

# sanitizers apply to the test binary only
cmake -B build-san -DLOB_SANITIZE=address,undefined
cmake --build build-san --target lob_tests && ./build-san/lob_tests
```

`bench` takes an engine name (`naive v1 v2 v3 v4 v5 final`) and `--ops N`
(default 1,000,000), `--warmup N` (100,000), `--seed S`, `--threads N` (one
engine per thread), `--pool SLOTS` and `--idmap SLOTS` (final engine
capacity), `--label STR`, `--csv PATH`, and `--json PATH`. The JSON holds
the whole run: machine, compiler, flags, load, timer resolution, both
throughputs, allocations, and percentiles per operation type.

## Layout

```
include/lob/   final engine: order_book.hpp, pool.hpp, id_map.hpp, types.hpp
engines/       naive_book.hpp (reference) and book_v1.hpp .. book_v5.hpp
bench/         harness: clock, workload, stats, allocation counter, driver
net/           protocol.hpp, framing.hpp, gateway, order_client, feed_client
tests/         typed, differential, property, and fuzz suites
scripts/       bench_report.sh, results.py, profile.py, net_smoke.sh, gateway_load.sh
docs/          benchmarks.md
results/       one directory of JSON runs per batch, profiles, earlier CSVs
```

## Limitations

* One instrument per engine, and the ladder costs 4 MB per instrument at the
  default band. Thousands of instruments on one host would need a band sized
  per instrument, or a sparse ladder for illiquid names.
* Prices must lie inside the band. There is no recentering after a halt.
* No journal. A production engine writes inbound messages to a sequenced log
  before matching, so it can replay after a crash.
* The feed detects gaps by sequence number but has no retransmit or snapshot
  channel.
* The gateway uses kernel TCP and a single poll() thread.
* Apple Silicon's counter advances in 41.7 ns steps, so single-operation
  latencies are quantized. Means, tails, and deltas are the figures to read.
