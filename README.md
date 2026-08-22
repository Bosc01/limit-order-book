# Limit Order Book Matching Engine

A C++20 limit order book built for one purpose: to be defensible under
interview cross-examination at a trading firm. Every optimization is a frozen,
benchmarked step with before and after numbers. Every semantic claim is backed
by a test. The hot path performs zero heap allocations, and the benchmark
proves it on every run.

Headline numbers on Apple M5, single thread, 2M-operation replay (seed 42,
45% passive limit, 10% aggressive limit, 5% market, 30% cancel, 10% modify):

| metric | value |
|---|---|
| throughput | ~26M ops/s |
| mean latency | 29 ns |
| p50 / p90 / p99 / p99.9 | 41 / 42 / 84 / 125 ns |
| heap allocations per op | 0.000 |
| cancel, naive baseline vs final | 17.6 us vs 25 ns (~700x) |

## Architecture

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
 |   every Order is one 64-byte cache line in a pre-sized slab pool              |
 +-------------------------------------------------------------------------------+
```

Three questions, each answered in O(1):

1. **What is the best price?** Cached cursor per side. When the best level
   empties, the next occupied level is found by scanning the occupancy bitmap
   with `clz`/`ctz` instructions, one 64-tick word at a time.
2. **Who is first at this price?** The head of that level's intrusive FIFO.
   Price-time priority falls out of the structure: better price level first,
   then oldest order first.
3. **Where is order N?** One hash probe (expected ~1.5 slots at load 0.5),
   landing on a flat array entry that points directly at the Order.

Order types: limit, market, cancel, modify. Time in force: GTC, IOC, FOK.
Self-trade prevention: none, cancel-resting, or reject-incoming, selected at
construction and compiled out of the match loop when off. Modify follows
exchange convention: a quantity decrease at the same price keeps queue
position; a price change or size increase re-enters as a fresh arrival.

## The measured optimization story

Seven engines share one replayable workload (seed 42). `naive` is the
deliberately slow baseline and the semantic reference; each `v_n` changes ONE
thing. Medians over 5 runs, 200k measured ops each:

| engine | change | mean ns | p50 | p90 | p99 | p99.9 | Mops/s | allocs/op |
|---|---|---|---|---|---|---|---|---|
| naive | std::map + std::list, O(n) cancel | 7431 | 83 | 24517 | 53160 | 61743 | 0.13 | 0.44 |
| v1 | map ladder + intrusive list + hash index | 50.0 | 42 | 84 | 125 | 250 | 16.96 | 0.88 |
| v2 | + slab pool for orders | 40.9 | 42 | 83 | 125 | 209 | 20.05 | 0.44 |
| v3 | + cache-line Order layout | 41.2 | 42 | 83 | 125 | 167 | 19.96 | 0.44 |
| v4 | + branch discipline in match loop | 42.4 | 42 | 83 | 125 | 167 | 19.50 | 0.44 |
| v5 | + flat banded ladder with bitmap | 38.2 | 42 | 42 | 125 | 500 | 21.08 | 0.44 |
| final | + open-addressing id map | 27.1 | 41 | 42 | 83 | 125 | 27.52 | 0.000 |

Honest readings, including the failures:

* **v2, the pool, is a real win**: +18% throughput, tighter p99.9. Malloc was
  paying size-class bookkeeping and scattering orders across the heap.
* **v3, alignment, measured neutral here.** The five-run spreads overlap
  completely. The working set fits in cache and the pool already delivers
  locality, so straddle-avoidance had nothing left to save. Kept because it
  costs 14% memory for insurance the profile cannot show on this machine.
* **v4, branch elimination, also neutral to slightly negative.** The lesson is
  worth more than a win: on a wide out-of-order core, well-predicted branches
  are nearly free, so removing them buys nothing measurable. The STP
  specialization (checks compiled out when the policy is off) is kept for the
  configurability, on the evidence that it costs nothing.
* **v5, the flat ladder, is a modest real win** (p90 halves) and it moves the
  tail: p99.9 rises to 500 ns while everything below improves. That tail is
  the bitmap rescan after the best level empties, plus the surviving
  unordered_map churn.
* **The final id map is the biggest post-v1 win**: +31% throughput, p99.9 back
  down to 125 ns, max latency collapses from ~57 us to ~8 us, and allocations
  hit exactly zero. The general-purpose allocator was the dominant remaining
  noise source, reached through std::unordered_map's per-node behavior.

## Benchmark methodology

* Workload is pre-generated from raw `mt19937_64` draws (seed 42), so every
  engine replays a byte-identical stream. `std::uniform_int_distribution` is
  avoided deliberately: the standard does not pin its algorithm, so seeds
  would not reproduce across standard libraries.
* Timestamps come from the hardware counter (`cntvct_el0` behind an `isb`, or
  `rdtsc` on x86), calibrated against `steady_clock` at startup. Apple
  Silicon's counter steps at ~41.7 ns; single-op figures quantize to that
  granule, which is why means accompany percentiles. Deltas between engines
  are unaffected (both sides carry the same quantization).
* Percentiles are exact: every sample is kept and sorted, no histogram
  approximation. Cross-run figures are medians of 5 runs.
* Global `operator new` (aligned overloads included) is interposed and counted
  over the measured region only. The "0.000 allocs/op" cell is measured on
  every run, not asserted once.
* A compiler barrier consumes every operation's result so the optimizer
  cannot delete work.
* This is a closed-loop benchmark measuring service time, not response time
  under load. There is no coordinated-omission correction because there is no
  arrival schedule; the networked path is where queueing enters.
* Hardware-counter profiling instructions (Linux perf, macOS Instruments) are
  in `docs/perf.md`.

## Correctness

386 GoogleTest cases across 21 suites:

* **A typed semantics suite runs identically against all seven engines.** The
  naive book is the executable specification; optimized engines must agree
  with it exactly, including the trade tape (maker, taker, price, quantity,
  order).
* **Differential replay**: the full benchmark workload runs through the naive
  reference and each engine side by side, comparing every result, every trade,
  best bid/ask, and periodic full structural audits (`check_invariants()`
  walks every list link, level total, bitmap bit, and id-map entry). The
  deepest pair does 1M operations.
* **Feature suites**: TIF x STP interactions (including FOK pre-check
  atomicity under both STP modes), duplicate id rejection, id reuse after
  cancel and after fill, band edges, bitmap scans across word boundaries,
  pool reuse and growth, id-map backward-shift deletion torture (200k random
  ops mirrored against `std::unordered_map`).
* **Sanitizers**: the whole suite passes under UBSan and under ASan+UBSan
  (verified armed with a deliberate use-after-free control).
* **Network parser fuzzing**: every frame decoder rejects truncated bodies,
  trailing bytes, and bad enums; the TCP reassembler is fed valid streams in
  every chunk size from 1 byte up, plus random garbage, and must never crash
  or emit an oversized frame.
* **End-to-end smoke test** (`scripts/net_smoke.sh`): gateway + UDP subscriber
  + 1000-order TCP burst; asserts every order acknowledged, trades observed,
  and zero feed sequence gaps on loopback.

## The networking layer

`gateway` runs the engine on a single poll() loop: TCP order entry on
127.0.0.1:9001, market data out over UDP (unicast or multicast group) with
sequence numbers. `order_client` and `feed_client` complete the demo.

Why TCP for orders and UDP for market data, the way real venues do it:

* An order must arrive exactly once, in order, or the sender must find out.
  That is TCP's contract. Loss on an order path means a compliance incident;
  retransmission latency is acceptable because correctness dominates.
* Market data is one-to-many fan-out where only the FRESHEST data matters. A
  retransmitted two-second-old tick is worse than useless, and TCP would make
  every subscriber's loss everyone's stall (head-of-line blocking). UDP
  multicast delivers one packet to N subscribers at wire speed; sequence
  numbers let each subscriber detect loss and recover via snapshot channels.
  Exchanges (CME, Nasdaq ITCH) publish exactly this shape.

Security posture at the edge: the gateway treats every inbound byte as
hostile. Frames are length-capped and field-decoded with bounds checks, bad
input poisons and drops the connection, participant ids for self-trade
prevention are assigned by the gateway (a client cannot spoof another's), slow
consumers are disconnected before they can stall the market, and order ids
never appear on the public feed.

## Build and run

Requires CMake 3.20+ and a C++20 compiler. GoogleTest is vendored.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/lob_tests                      # 386 tests
./build/bench final                    # benchmark the final engine
./build/bench naive                    # the baseline, for contrast
./scripts/net_smoke.sh build           # end-to-end network test

# sanitizer build (tests only; sanitizers distort benchmark numbers)
cmake -B build-san -DLOB_SANITIZE=address,undefined
cmake --build build-san --target lob_tests && ./build-san/lob_tests
```

`bench` options: engine name (`naive v1 v2 v3 v4 v5 final`), `--ops N`,
`--warmup N`, `--seed S`, `--label STR`, `--csv PATH`. Results append as CSV
for charting.

## Repository layout

```
include/lob/        the final engine: order_book.hpp, pool.hpp, id_map.hpp, types.hpp
engines/            naive_book.hpp (reference) and frozen snapshots book_v1..v5.hpp
bench/              harness: clock, workload, stats, alloc counter, driver
net/                protocol.hpp, framing.hpp, gateway, order_client, feed_client
tests/              GoogleTest suites incl. differential and fuzz tests
scripts/            net_smoke.sh
docs/               perf.md (hardware counters), INTERVIEW_PREP.md
results/            benchmark CSVs
```

## Known limitations, stated on purpose

* One instrument per engine instance; a venue would shard instruments across
  engines.
* Prices must lie inside the configured band (default [1, 32768)). Real
  venues bound prices too; recentering on a halt is not implemented.
* Single-threaded core with in-process callbacks; a production system would
  pin the engine thread and hand fills across cores via queues.
* No persistence or crash recovery; a real engine journals inbound messages
  before matching (sequenced-log architecture).
* The UDP feed has no retransmission or snapshot-recovery channel; gap
  detection is implemented, recovery is not.
* Apple Silicon's 41.7 ns timer granule limits single-op resolution; the
  numbers worth quoting are means, tails, and deltas, all of which survive it.
