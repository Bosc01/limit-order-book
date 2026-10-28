# Limit Order Book Matching Engine

[![ci](https://github.com/Bosc01/limit-order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/Bosc01/limit-order-book/actions/workflows/ci.yml)

A C++20 limit order book I built because I wanted to understand how exchange
matching engines actually work, and then could not stop optimizing. Every
optimization is a frozen, benchmarked step with before and after numbers.
Every semantic claim is backed by a test. The hot path performs zero heap
allocations, and the benchmark proves it on every run.

Headline numbers on Apple M5, single thread: the median of five runs of 2M
operations each (seed 42; 45% passive limit, 10% aggressive limit, 5%
market, 30% cancel, 10% modify). Throughput comes from an untimed pass over
the operations; latency comes from a second pass that timestamps every
operation, and those timestamps cost throughput of their own (second row).
The machine, its load, and every run are in [Benchmark](#benchmark).

<!-- BEGIN generated: headline final-2026-10-27 final -->
| metric | value |
|---|---|
| throughput | 51.0M ops/s |
| throughput with a timestamp around every op | 22.1M ops/s |
| mean latency | 33.4 ns |
| p50 / p99 / p99.9 | 42 / 84 / 208 ns |
| heap allocations in the measured region | zero in 5 of 5 runs |
<!-- END generated -->

Tests: 420, at 98.2% line coverage (llvm-cov). Every benchmark table in
this README is generated from the saved runs in `results/` by
`scripts/results.py`, and CI fails if a table drifts from them.

In August 2026, on a quieter machine, the same engine measured 27M ops/s
with per-op timestamps, a 28 ns mean, and a 166 ns p99.9
(`results/final_2m.csv`); the untimed pass did not exist yet.

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
of either, re-checked with every allocation path counted in
`results/endurance_2026-10-22.txt`.

**Why a flat ladder and not a tree or heap.** Prices are integers on a tick
grid inside a bounded band, so an array index already sorts them. The
`std::map` version (`engines/book_v1.hpp`) pays O(log L) to insert a new
level and to erase a level emptied by a cancel, chasing tree nodes scattered
across the heap. The switch is the v4 to v5 step in the
[progression table](#the-measured-optimization-story). The price is the band: orders outside it are
rejected, and empty levels still cost memory, one 64-byte line per tick per
side, about 2 MB per side at the default band.

**Why no lazy deletion.** Lazy deletion is the workaround for a binary heap
of prices: a heap cannot remove an element from its middle, so a canceled
entry is marked dead and skipped when it reaches the top. Here every
removal is already constant time. The order is unlinked through its own
prev and next pointers, its level is reached through a back-pointer, and an
emptied level is a cleared bit. Lazy deletion would add a dead-entry check
to the matching loop and keep dead orders in memory, for no gain.

## Benchmark

Reproduce with one command after building (see
[Build and run](#build-and-run)), then regenerate every table in this README
from the saved runs:

```bash
./scripts/bench_report.sh mine final=build
python3 scripts/results.py readme
```

The first command runs the final engine five times (2M measured operations
each, after 100k warmup, seed 42) and saves each run as one JSON file under
`results/mine/`, with the full printout and the commit it was built from.
The second rewrites the generated blocks of this README from `results/`;
with `--check` it only reports drift, which is how CI runs it. The figures
below are `results/final-2026-10-27/`:

<!-- BEGIN generated: operations final-2026-10-27 final -->
```
machine: Apple M5, 10 logical cores (4 performance, 6 efficiency), Darwin 25.6.0 arm64
build: Apple LLVM 17.0.0 (clang-1700.6.3.2), Release
flags: -std=c++20 -O3 -DNDEBUG -O3 -fno-exceptions -fno-rtti -g -Wall -Wextra -Wpedantic
timer resolution: 42.7 ns
```

| operation | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| all | 33.4 | 42 | 84 | 208 |
| limit | 32.9 | 41 | 125 | 209 |
| market | 35.1 | 42 | 84 | 167 |
| cancel | 32.0 | 41 | 84 | 125 |
| modify | 39.6 | 42 | 125 | 167 |

Throughput, untimed pass: median 51.0M ops/s, runs from 47.6M to 53.2M. With a timestamp around every op: median 22.1M, runs from 19.9M to 23.6M. Load average at the start of each run: 3.76 to 3.83. Every run is in `results/final-2026-10-27/`, built from commit `eefb3a6`.
<!-- END generated -->

Each run makes two passes over the same operations, each on a fresh book.
The timed pass reads the hardware counter before and after every operation
and gives the latency figures. Those reads sit behind an `isb` instruction
barrier, which drains the pipeline, so the timed pass cannot overlap one
operation's cache misses with the next operation's work; in a profile of the
timed loop, 61.7% of samples land on the two reads (`results/profiles/`).
The untimed pass runs the same operations with no timestamps and gives the
throughput. It must end with the same book as the timed pass, or the run
fails.

Conditions, stated because they move the numbers: every batch dated
2026-10-27 in `results/` ran on battery power, per the system power log, and
batches recorded after commit c662000 also log the battery line before each
run in their `output.txt`. The Bluetooth daemon and an RStudio session each
held a core at about 100% throughout. Batch-to-batch drift under these
conditions is large: the same final engine measured 57.8M ops/s untimed in
one batch (`results/try-fibonacci-hash-run3/`) and 51.0M in this one, minutes
apart. That is why a change is only judged inside one interleaved batch. The
p50 sits at the timer's resolution of about 42 ns; the means and tails carry
the finer information.

## The measured optimization story

Seven engines share one replayable workload (seed 42). `naive` is the
deliberately slow baseline and the semantic reference; each `v_n` changes ONE
thing. Re-measured on 2026-10-27, five interleaved runs per engine of 200k
measured operations each (`results/progression-2026-10-27/`). The count is
200k rather than 2M because `naive` slows as its book grows: one 2M run of it
took over eight minutes.

<!-- BEGIN generated: progression progression-2026-10-27 -->
| engine | change | untimed Mops/s | against previous | timed Mops/s | mean ns | p50 | p90 | p99 | p99.9 | allocs per op |
|---|---|---|---|---|---|---|---|---|---|---|
| naive | std::map + std::list, O(n) cancel | 0.12 (0.11 to 0.13) |  | 0.13 | 7908.5 | 83 | 27517 | 60035 | 83845 | 0.401 |
| v1 | map ladder, intrusive list, hash index | 20.8 (20.5 to 21.9) | +17077% | 14.1 | 59.8 | 42 | 84 | 167 | 292 | 0.796 |
| v2 | + slab pool for orders | 25.9 (23.5 to 27.8) | +25% | 16.3 | 50.3 | 42 | 84 | 125 | 209 | 0.401 |
| v3 | + cache-line Order layout | 26.1 (25.7 to 26.4) | +1%, ranges overlap | 16.3 | 50.3 | 42 | 84 | 125 | 209 | 0.401 |
| v4 | + branch discipline in the match loop | 26.5 (25.5 to 28.3) | +2%, ranges overlap | 16.4 | 49.8 | 42 | 84 | 125 | 209 | 0.401 |
| v5 | + flat banded ladder with bitmap | 29.7 (20.6 to 29.8) | +12%, ranges overlap | 17.5 | 46.1 | 42 | 83 | 125 | 250 | 0.395 |
| final | + open-addressing id map, Fibonacci hash | 63.9 (55.6 to 64.6) | +115% | 21.0 | 36.4 | 41 | 83 | 125 | 209 | 0.000 |
<!-- END generated -->

Honest readings, including the failures:

* **v2, the slab pool, is a real win**: every v2 run beat every v1 run.
  Malloc was paying size-class bookkeeping and scattering orders across the
  heap.
* **v3, alignment, and v4, branch discipline, are no clear change**, as they
  were in August. The working set fits in cache and the pool already
  delivers locality, so straddle-avoidance has nothing left to save; on a
  wide out-of-order core, well-predicted branches are nearly free. Both stay
  in the code, on the evidence that they cost nothing measurable.
* **v5, the flat ladder, raises the median, but one slow v5 run overlaps
  v4's range**, so by this README's rule it is not a clear win in this batch.
  August's batch showed it as a modest win. Its new tail source is the
  bitmap rescan after a best level empties.
* **The final engine is the biggest step after v1**, more than doubling
  untimed throughput over v5, and the only one with zero heap allocations:
  v1 to v5 still reach the general-purpose allocator through
  `std::unordered_map`'s per-node allocation.
* **The timed and untimed columns pull apart as the engines get faster.**
  For naive they match; final runs about three times faster untimed. The
  timestamps cost about the same per operation in every engine, so the
  faster the engine, the larger their share.

### Profile-driven attempts, October 2026

Starting from the final engine, the rule was: profile, change one thing,
and keep it only if it measures faster. `scripts/profile.py` sampled the untimed pass of
the final engine (`results/profiles/`). After the op-kind switch, which
mispredicts because the workload mixes operation types at random, the hot
lines were id-map probes and the order-line loads in `match` and `rest`.
Two changes came from that profile:

* **Single-probe cancel.** Cancel found an order, then `erase()` hashed the
  id and walked the same probe chain again. `take()` does both in one walk.
* **Fibonacci hashing.** Every probe starts with the hash, and
  backward-shift deletion re-hashes each neighbor it inspects. The splitmix64
  finalizer is two multiplies and three shift-xors in a dependent chain;
  Fibonacci hashing is one multiply and a shift.

Each batch runs the unchanged baseline build and the changed build
interleaved, five runs each. A batch is valid only if its unchanged build ran
within 10% of the baseline batch's median (`results/baseline/`, 50.0M ops/s).
That check reads only the unchanged build, so it cannot favor a change.
Invalid batches stay in the table.

<!-- BEGIN generated: attempts baseline noise-check try-cancel-take try-cancel-take-rerun try-cancel-take-run3 try-fibonacci-hash try-fibonacci-hash-rerun try-fibonacci-hash-run3 -->
| batch | change | before, Mops/s | after, Mops/s | median | load | verdict |
|---|---|---|---|---|---|---|
| `noise-check` | The same build as a. | 49.8 (48.6 to 50.2) | 49.6 (46.2 to 49.8) | -0.5% | 4.82 to 4.82 | no clear change, ranges overlap |
| `try-cancel-take` | cancel takes its id-map entry with one probe walk instead of find and then erase. Another job held three cores; the code was reverted after this batch. | 31.6 (29.9 to 32.5) | 32.8 (31.7 to 33.8) | +3.9% | 9.53 to 9.92 | not valid: before build ran -37% from the baseline |
| `try-cancel-take-rerun` | cancel takes its id-map entry with one probe walk, second batch. Both builds ran about a third as fast as a minute later. | 13.6 (11.2 to 17.5) | 13.6 (7.7 to 16.4) | -0.0% | 4.25 to 4.39 | not valid: before build ran -73% from the baseline |
| `try-cancel-take-run3` | cancel takes its id-map entry with one probe walk, third batch. Stays reverted. | 52.3 (47.3 to 53.5) | 52.6 (51.8 to 55.0) | +0.6% | 3.81 to 3.91 | no clear change, ranges overlap |
| `try-fibonacci-hash` | Fibonacci hashing in the id map. Another job held three cores during the batch. | 29.5 (26.5 to 31.4) | 36.4 (30.6 to 39.0) | +23.3% | 8.70 to 9.02 | not valid: before build ran -41% from the baseline |
| `try-fibonacci-hash-rerun` | Fibonacci hashing in the id map, second batch. Both builds ran about a third as fast as a minute later. | 18.7 (10.8 to 30.0) | 19.2 (8.9 to 27.5) | +2.6% | 4.25 to 4.68 | not valid: before build ran -62% from the baseline |
| `try-fibonacci-hash-run3` | Fibonacci hashing in the id map, third batch. Kept. | 49.6 (44.9 to 49.9) | 57.8 (53.7 to 60.8) | +16.5% | 3.91 to 3.91 | faster, ranges do not overlap |
<!-- END generated -->

Readings:

* **The A/A check holds.** One build against itself differs by 0.5% at the
  median, with overlapping ranges.
* **Four of the six attempt batches are not valid.** The first two ran while
  another job held three cores. The next two ran during a twenty-second
  slowdown that cut both builds to about a third of their speed; a minute
  later the machine was back to normal. The plan before those batches was
  "re-run once"; the validity check was added after they came back
  unusable, and it reads only the unchanged build.
* **Fibonacci hashing is kept.** In its first valid batch every changed run
  beat every unchanged run: 16.5% more throughput at the median, and mean
  latency down from 32.5 to 27.7 ns. The measurement does not say which part
  did it: the shorter hash chain in front of each probe's load, or the more
  even spacing Fibonacci hashing gives sequential ids, which leaves fewer
  collisions to walk.
* **Single-probe cancel stays reverted.** In its valid batch it measured
  +0.6% at the median, inside the noise. Likely reason: the second walk it
  removed was already in cache, and the core computes the second hash while
  the first probe's load is still in flight.

## Scale, endurance, and the wire

Measurements beyond the single-engine benchmark. The first three are from
August 2026, before runs recorded load:

* **Sharding** (`--threads N`: one engine per thread, disjoint instruments,
  no shared state, the way real venues scale): aggregate throughput peaks at
  **~85M ops/s at 8 shards**, still zero allocations. The per-shard rate
  falls from 24.6M (2 shards) to ~19M (4 shards) as shards share the memory
  system, then to ~11M (8 shards): the M5 has only 4 performance cores, so
  past 4 shards some threads must run on the slower efficiency cores. This
  is why production engines pin one engine thread per fast core and stop
  there.
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
  kernel-bypass networking in production. Re-measured on 2026-10-22 through
  the current gateway (with per-connection order ownership), under a load
  average of 8.1: p50 31.5 us, p99 58.4 us, p99.9 90.5 us
  (`results/wire_2026-10-22.txt`).
* **Gateway under load** (`scripts/gateway_load.sh`, October 2026, in a Linux
  VM on this machine, median of 5 runs per client count): 1, 8, 32, and 128
  closed-loop clients on persistent connections. Throughput levels off near
  50k orders/s from 8 clients up while round trips grow with the client
  count (p50 2.3 ms at 128 clients), so the single-threaded poll() gateway,
  not the engine, is the limit. After every run the clients disconnect with
  orders resting, and the gateway must cancel all of them: zero were left in
  all 20 runs (`results/gateway_load.md`).

## Benchmark methodology

* Workload is pre-generated from raw `mt19937_64` draws (seed 42), so every
  engine replays a byte-identical stream. `std::uniform_int_distribution` is
  avoided deliberately: the standard does not pin its algorithm, so seeds
  would not reproduce across standard libraries.
* Timestamps come from the hardware counter (`cntvct_el0` behind an `isb`, or
  `rdtsc` on x86), calibrated against `steady_clock` at startup. The harness
  also measures the counter's resolution and prints it: Apple Silicon counts
  in 1 ns units but advances in steps of about 41.7 ns, so single-op figures
  quantize to that step, which is why means accompany percentiles. Deltas
  between engines are unaffected (both sides carry the same quantization).
* Every run first prints the CPU, core layout, OS, compiler, whether the
  build is optimized, and the load average, so a result always carries its
  conditions.
* Percentiles are exact: every sample is kept and sorted, no histogram
  approximation. Cross-run figures are medians of 5 runs.
* Two passes per run, on fresh books. The timed pass gives latency; the
  untimed pass, with no timestamps, gives throughput and must end with the
  same book. The timestamp reads sit behind an `isb`, which drains the
  pipeline: in a profile of the timed loop, 61.7% of samples land on those
  two reads (`results/profiles/`), and the timed pass runs at about half the
  untimed rate.
* Comparisons run in one batch with the builds interleaved, A then B, then
  B then A, so load drift falls on both. A change is called faster or slower
  only when every run of one build beats every run of the other; otherwise
  the verdict is "no clear change", whatever the medians say. The A/A check
  (one build against itself) is the first row of the attempts table.
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

420 GoogleTest cases across 23 suites (98.2% line coverage, 99.2% function
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
* **Property tests** (`tests/property_test.cpp`): seeded random sequences of
  limit, market, cancel, and modify orders, 100,000 operations per
  self-trade policy, checked after every single operation against a model
  the test keeps itself. The book is never crossed or locked, the cached best
  prices match the model, every price level holds exactly the modeled
  quantity, and a ledger proves quantity conservation: everything that came
  to rest is still resting, traded, or canceled. Each trade must hit a live
  order on the other side at that order's price, and the same seed run twice
  must produce identical results and trades. Each property was checked by
  injecting a bug into the engine and confirming the tests fail.
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
never appear on the public feed. Order ownership is enforced per connection:
each connection has its own order id space, which the gateway maps to engine
ids it assigns itself, so a client can cancel or amend only its own orders. An
id that belongs to another client gets the same answer as an id nobody uses,
so a client cannot probe for other clients' live orders. When a connection
drops, for any reason including a slow-consumer disconnect, the gateway
cancels every order it still has resting and publishes one book snapshot, so
no order is left on the book that nobody can cancel.

## Build and run

Requires CMake 3.20+ and a C++20 compiler. GoogleTest is vendored.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/lob_tests                      # 420 tests
./scripts/bench_report.sh mine final=build   # 5 runs saved as results/mine/*.json, medians
python3 scripts/results.py readme      # regenerate this README's tables from results/
./scripts/profile.py                   # where the untimed pass spends its time (macOS)
./build/bench final                    # one benchmark run of the final engine
./build/bench naive                    # the baseline, for contrast
./scripts/net_smoke.sh build           # end-to-end network test
./scripts/gateway_load.sh build        # gateway load test: 1-128 clients, writes results/gateway_load.md

# sanitizer build (tests only; sanitizers distort benchmark numbers)
cmake -B build-san -DLOB_SANITIZE=address,undefined
cmake --build build-san --target lob_tests && ./build-san/lob_tests
```

`bench` options: engine name (`naive v1 v2 v3 v4 v5 final`), `--ops N`
(default 1,000,000), `--warmup N` (default 100,000), `--seed S`,
`--label STR`, `--csv PATH`, `--json PATH` (one file holding the whole run:
machine, compiler, flags, load, timer resolution, throughput, allocations,
and percentiles per operation type, plus the untimed pass's throughput),
`--threads N` (one engine per thread),
`--pool SLOTS` and `--idmap SLOTS` (final engine capacity). Results also
append as CSV for charting.

## Repository layout

```
include/lob/        the final engine: order_book.hpp, pool.hpp, id_map.hpp, types.hpp
engines/            naive_book.hpp (reference) and frozen snapshots book_v1..v5.hpp
bench/              harness: clock, workload, stats, alloc counter, driver
net/                protocol.hpp, framing.hpp, gateway, order_client, feed_client
tests/              GoogleTest suites incl. differential, property, and fuzz tests
scripts/            bench_report.sh and results.py (benchmark runs and README tables),
                    profile.py, net_smoke.sh, gateway_load.sh
docs/               perf.md (hardware counter profiling guide)
results/            one directory of JSON runs per batch, profiles, older CSVs
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
* Apple Silicon's timer resolution (about 41.7 ns, measured on every run)
  limits single-op resolution; the numbers worth quoting are means, tails,
  and deltas, all of which survive it.
