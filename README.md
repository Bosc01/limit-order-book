# Limit Order Book Matching Engine

[![ci](https://github.com/Bosc01/limit-order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/Bosc01/limit-order-book/actions/workflows/ci.yml)

A C++20 limit order book I built because I wanted to understand how exchange
matching engines actually work, and then could not stop optimizing. Every
optimization is a frozen, benchmarked step with before and after numbers.
Every semantic claim is backed by a test. The hot path performs zero heap
allocations, and the benchmark proves it on every run.

Headline numbers on Apple M5, single thread, 2M-operation replay (seed 42,
45% passive limit, 10% aggressive limit, 5% market, 30% cancel, 10% modify):

| metric | value |
|---|---|
| throughput | ~27M ops/s |
| mean latency | 28 ns |
| p50 / p90 / p99 / p99.9 | 41 / 42 / 84 / 166 ns |
| heap allocations per op | 0.000 (measured every run) |
| cancel, naive baseline vs final | 17.6 us vs 24 ns (~740x) |
| line coverage (llvm-cov) | 97.7% |

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
construction and compiled out of the match loop when off; owner 0 is
reserved as "ungrouped, STP-exempt" the way venues scope prevention to
explicit groups, and a CancelResting kill emits an unsolicited cancel event
so the owner learns the order died. Modify follows exchange convention: a
quantity decrease at the same price keeps queue position; a price change or
size increase re-enters as a fresh arrival, and the result distinguishes
"unknown id" from "bad amend on a live order". Market orders support a
CME-style protection band (cap on how far past the touch a sweep may run).

### Operation costs

Notation: F is the number of resting orders an incoming order touches
(fills, or removes for self-trade prevention), L the number of price levels
it crosses, d the number of ticks the best-price scan walks (the gap to the
next occupied level after a cancel, or the span a crossing order sweeps),
and B the width of the price band in ticks (32,767 by default, so the
bitmap is 512 words per side).

| Operation | Cost | Where the time goes |
|---|---|---|
| Add, does not cross | O(1) expected | duplicate-id probe, append at the level tail, id-map insert, set one bitmap bit |
| Add that crosses, or market order | O(F + L + d/64) expected | constant work per fill; one bitmap scan each time the best level empties |
| Cancel | O(1) expected, plus O(d/64) if it empties the best level | id-map lookup and erase, unlink through the order's own pointers, return the slot to the pool |
| Modify, same or smaller size at the same price | O(1) expected | amended in place, keeps queue position |
| Modify, any other change | cancel plus add | loses queue position, as on real venues |
| Best bid or ask | O(1) | cached per side |
| FOK pre-check | O(L + d/64) | sums level totals; walks individual orders only when self-trade prevention is on |

"Expected" comes from the id map: linear probing with the load factor held
at or below 0.5 averages at most 1.5 probes for a hit and 2.5 for a miss,
assuming the hash spreads ids evenly. The d/64 term is the bitmap scan, which tests
64 ticks per instruction; d is small when resting orders sit near the
touch, and the worst case is B/64 = 512 word reads. Pool growth and id-map
rehash are O(n) events that only fire when the engine is undersized; the
100M-operation endurance run below, sized for its session, recorded zero
of either.

**Why a flat ladder and not a tree or heap.** Prices are integers on a tick
grid inside a bounded band, so an array index already sorts them. The
`std::map` version (`engines/book_v1.hpp`) pays O(log L) to insert a new
level and to erase a level emptied by a cancel, chasing tree nodes scattered
across the heap. Switching to the flat ladder took the mean from 39.9 to 36.0 ns
and throughput from 20.5M to 22.3M ops/s (v4 to v5 in the next table,
medians of five runs). The price is the band: orders outside it are
rejected, and empty levels still cost memory, one 64-byte line per tick per
side, about 2 MB per side at the default band.

**Why no lazy deletion.** Lazy deletion is the workaround for a binary heap
of prices: a heap cannot remove an element from its middle, so a canceled
entry is marked dead and skipped when it reaches the top. Here every
removal is already constant time. The order is unlinked through its own
prev and next pointers, its level is reached through a back-pointer, and an
emptied level is a cleared bit. Lazy deletion would add a dead-entry check
to the matching loop and keep dead orders in memory, for no gain.

## The measured optimization story

Seven engines share one replayable workload (seed 42). `naive` is the
deliberately slow baseline and the semantic reference; each `v_n` changes ONE
thing. Medians over 5 runs, 200k measured ops each:

| engine | change | mean ns | p50 | p90 | p99 | p99.9 | Mops/s | allocs/op |
|---|---|---|---|---|---|---|---|---|
| naive | std::map + std::list, O(n) cancel | 7459 | 42 | 24393 | 53035 | 70594 | 0.13 | 0.44 |
| v1 | map ladder + intrusive list + hash index | 48.6 | 42 | 84 | 125 | 250 | 17.39 | 0.88 |
| v2 | + slab pool for orders | 40.0 | 42 | 83 | 125 | 167 | 20.47 | 0.44 |
| v3 | + cache-line Order layout | 40.1 | 42 | 83 | 125 | 167 | 20.46 | 0.44 |
| v4 | + branch discipline in match loop | 39.9 | 42 | 83 | 125 | 167 | 20.53 | 0.44 |
| v5 | + flat banded ladder with bitmap | 36.0 | 42 | 42 | 84 | 167 | 22.27 | 0.44 |
| final | + open-addressing id map | 25.5 | 41 | 42 | 83 | 125 | 29.13 | 0.000 |

Honest readings, including the failures:

* **v2, the pool, is a real win**: +18% throughput, tighter p99.9. Malloc was
  paying size-class bookkeeping and scattering orders across the heap.
* **v3, alignment, measured neutral.** The five-run spreads overlap
  completely. The working set fits in cache and the pool already delivers
  locality, so straddle-avoidance had nothing left to save. Kept because it
  costs 14% memory for insurance the profile cannot show on this machine.
* **v4, branch elimination, also neutral** (within run-to-run spread across
  repeated five-run batches). The lesson is worth more than a win: on a wide
  out-of-order core, well-predicted branches are nearly free, so removing
  them buys nothing measurable. The STP specialization (checks compiled out
  when the policy is off) is kept for the configurability, on the evidence
  that it costs nothing.
* **v5, the flat ladder, is a modest real win**: p90 halves and p99 drops a
  granule; the new tail source (the bitmap rescan after a best level
  empties) shows up in occasional runs as a fatter p99.9.
* **The final id map is the biggest post-v1 win**: +31% throughput, p99.9
  down to 125 ns, max latency collapses from ~57 us to ~8 us, and
  allocations hit exactly zero. The general-purpose allocator was the
  dominant remaining noise source, reached through std::unordered_map's
  per-node behavior.

## Scale, endurance, and the wire

Three more measurements, run after the single-thread story was settled:

* **Sharding** (`--threads N`: one engine per thread, disjoint instruments,
  no shared state, the way real venues scale): aggregate throughput peaks at
  **~85M ops/s at 8 shards**, still zero allocations. The per-shard rate
  falls from 24.6M (2 shards) to ~11M (8 shards): the shards saturate the
  shared memory fabric, and the efficiency cores are slower. Matching is
  memory-latency bound, which is exactly why production engines pin one
  engine thread per core and stop there.
* **Endurance** (100M operations, single thread, book growing to 328k live
  orders): **18.6M ops/s sustained, p99.9 333 ns, max 52 us, zero
  allocations**, with the pool and id map sized for the session
  (`--pool 524288 --idmap 1048576`). Two instructive failure modes were
  measured on the way: default sizing let the safety valve open (4 growth
  allocations, and a 23 ms worst op from an id-map rehash), and oversizing
  the id map made every probe a DRAM miss (13.3M ops/s). Capacity planning
  is a latency decision.
* **Wire-to-wire** (order client, 20k synchronous round trips over loopback
  TCP): **p50 20.9 us, p99 53.8 us, p99.9 85.4 us**, measured at the client
  from send() to the last byte of the exec report. The engine core is 28 ns;
  the kernel round trip is ~750x that, which is the whole argument for
  kernel-bypass networking in production.

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

408 GoogleTest cases across 21 suites (97.7% line coverage, 99.2% function
coverage by llvm-cov over the engines and wire protocol):

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
* **Adversarial review found real bugs, and the fixes are tested.** A
  hostile review pass over the finished engine produced two confirmed
  defects that the 386-test suite of the time could not see: STP with a
  defaulted owner id let two unrelated flows self-match-prevent each other
  (owner 0 is now reserved as exempt), and a CancelResting kill was
  invisible to the killed order's owner (there is now an unsolicited cancel
  event, wired through the gateway). Both carry regression tests across all
  six indexed engines.

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

./build/lob_tests                      # 408 tests
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
docs/               perf.md (hardware counter profiling guide)
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
