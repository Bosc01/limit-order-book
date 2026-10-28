# Limit Order Book Matching Engine

[![ci](https://github.com/Bosc01/limit-order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/Bosc01/limit-order-book/actions/workflows/ci.yml)

A price-time priority matching engine for one instrument, in C++20, with a
TCP order gateway and a UDP market data feed. I wrote it to learn how
exchange matching engines are built. Each optimization is kept as its own
engine in `engines/`, and all seven replay the same workload, so every step
is measured against the one before it.

Final engine (`include/lob/`) on an Apple M5, one thread, median of five runs
of 2M operations (seed 42: 45% passive limit, 10% crossing limit, 5% market,
30% cancel, 10% modify):

<!-- BEGIN generated: headline final-2026-10-28 final -->
| metric | value |
|---|---|
| throughput | 67.6M ops/s |
| throughput with a timestamp around every op | 29.4M ops/s |
| mean latency | 24.9 ns |
| p50 / p99 / p99.9 | 41 / 83 / 125 ns |
| heap allocations in the measured region | zero in 5 of 5 runs |
<!-- END generated -->

Throughput comes from a pass with no timestamps. Latency comes from a second
pass over the same operations that reads the system counter around each one.
Each read sits behind an `isb`, which serializes the pipeline, so that pass
runs at less than half the rate. The p50 is one step of the counter, which
advances every 41.7 ns. Method, machine conditions, and every run:
[docs/benchmarks.md](docs/benchmarks.md).

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
rehash are O(n) and fire only when the engine is undersized; a 100M-operation
run sized for its session recorded neither.

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
Owner 0 means no STP group and is exempt, as on venues that scope
prevention to explicit groups. A cancel-resting kill emits an unsolicited
cancel so the resting order's owner hears about it.

Market orders can carry a protection limit: the number of ticks past the
touch a sweep may go. The remainder is discarded.

## Optimization steps

`naive` (`std::map` of `std::list`, cancel by linear search) is the
reference the other engines are tested against. Each `v_n` changes one
thing. Five interleaved runs per engine of 200k operations after a 10k
warmup, fewer than the headline because `naive` slows as its book grows
(`results/progression-2026-10-28/`):

<!-- BEGIN generated: progression progression-2026-10-28 -->
| engine | change | untimed Mops/s | against previous | timed Mops/s | mean ns | p50 | p90 | p99 | p99.9 | allocs per op |
|---|---|---|---|---|---|---|---|---|---|---|
| naive | std::map + std::list, O(n) cancel | 0.15 (0.15 to 0.15) |  | 0.15 | 6571.7 | 42 | 22643 | 48577 | 60201 | 0.401 |
| v1 | map ladder, intrusive list, hash index | 25.6 (25.4 to 25.7) | +16888% | 17.2 | 49.0 | 42 | 84 | 125 | 208 | 0.796 |
| v2 | + slab pool for orders | 32.2 (31.8 to 32.6) | +26% | 20.1 | 40.7 | 42 | 83 | 125 | 167 | 0.401 |
| v3 | + Order aligned to 64 bytes | 32.2 (31.7 to 32.4) | -0%, ranges overlap | 20.2 | 40.5 | 42 | 83 | 125 | 167 | 0.401 |
| v4 | + branch layout in the match loop | 32.8 (32.2 to 33.1) | +2%, ranges overlap | 20.1 | 40.5 | 42 | 83 | 125 | 167 | 0.401 |
| v5 | + flat banded ladder with bitmap | 37.3 (36.7 to 37.6) | +13% | 22.0 | 36.4 | 42 | 42 | 84 | 167 | 0.395 |
| final | + open-addressing id map, Fibonacci hash | 78.2 (77.2 to 79.0) | +110% | 30.7 | 23.6 | 41 | 42 | 83 | 125 | 0.000 |
<!-- END generated -->

A step is called faster only when every one of its runs beats every run of
the previous engine; "ranges overlap" marks the others.

* v1: in `naive`, cancel and modify average 16 and 18 us because they search
  the book. The id index brings them to 32 and 42 ns.
* v2: the pool beat v1 in every run.
* v3 (64-byte aligned `Order`) and v4 (branch layout in the match loop): no
  measurable change, most likely because the working set fits in cache and
  the branches are well predicted. Both are kept because they cost nothing.
* v5: the flat ladder beat v4 in every run, 13% at the median.
* final: the open-addressing id map more than doubles v5 (+110%) and
  removes the last allocations. v1 to v5 still allocate a node for every
  `std::unordered_map` insert.
* Timed and untimed throughput diverge as the engines get faster. The
  counter reads cost about the same per operation in every engine, so they
  are a larger share of a faster one.

## Verification

425 GoogleTest cases; 98.2% line and 99.2% function coverage over the
engines and the wire protocol (llvm-cov).

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
  model, quantity is conserved (everything that rested is still resting,
  traded, or canceled), and every trade hits a live order at that order's
  price. Each property was confirmed to fail against a deliberately broken
  engine.
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
* Linux VM on the same machine, 1 to 128 closed-loop clients
  (`results/gateway_load.md`): throughput levels off near 50k orders/s from
  8 clients up while round trips grow with the client count, so the gateway
  thread is the limit, not the engine. It publishes a UDP book snapshot
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
