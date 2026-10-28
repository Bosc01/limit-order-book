# Benchmarks

## Method

* The workload is generated before timing from raw `mt19937_64` output
  (seed 42), so every engine replays the same stream byte for byte.
  `std::uniform_int_distribution` is not used: the standard does not fix
  its algorithm, so one seed would give different streams on libc++ and
  libstdc++.
* Each run makes two passes over the same operations, each on a fresh book.
  The timed pass reads the counter around every operation (`isb; mrs
  cntvct_el0` on arm64, `lfence; rdtsc` on x86), calibrated against
  `steady_clock`. The untimed pass has no timestamps and gives throughput.
  It must end in the same book state as the timed pass, or the run fails.
* Percentiles are exact: every sample is kept and sorted.
* Global `operator new`, aligned overloads included, is replaced by a
  counting version over the measured region, so the allocation figure is
  measured on every run.
* Each operation's result feeds a compiler barrier, so the optimizer cannot
  drop work.
* Every run records CPU, core layout, OS, compiler, flags, build type, load
  average, and the measured timer resolution in its JSON.
* Comparisons run the builds interleaved in one batch (A then B, then B then
  A), five runs each. A change is called faster or slower only when every
  run of one build beats every run of the other.
* The benchmark is closed loop, so it measures service time. There is no
  arrival schedule and so no coordinated omission to correct; queueing is
  measured at the gateway instead.

Reproduce, then regenerate every table in the README and in this file:

```bash
./scripts/bench_report.sh mine final=build
python3 scripts/results.py summary mine
python3 scripts/results.py readme
```

`results.py readme --check` reports drift without writing; CI runs it.

## Conditions

The README's figures come from batches dated 2026-10-28, run on AC power
with an RStudio session, which otherwise held one core at 100%, paused
(SIGSTOP) for their duration. The Bluetooth daemon still held one core near
100%. Under those conditions one build run against itself
(`results/noise-check-2026-10-28/`) differed by 0.1% at the median, and each
build's five runs spanned about 1.6%.

Batches dated 2026-10-27, which include every attempt batch below, ran on
battery with both processes busy, per the power line each run logs in its
`output.txt`. There the same final build measured 57.8M ops/s in one batch
(`results/try-fibonacci-hash-run3/`) and 51.0M in another, minutes apart.
Absolute figures move that much between batches, which is why a change is
only judged inside one interleaved batch.

## Final engine by operation

<!-- BEGIN generated: operations final-2026-10-28 final -->
```
machine: Apple M5, 10 logical cores (4 performance, 6 efficiency), Darwin 25.6.0 arm64
build: Apple LLVM 17.0.0 (clang-1700.6.3.2), Release
flags: -std=c++20 -O3 -DNDEBUG -fno-exceptions -fno-rtti -g -Wall -Wextra -Wpedantic
timer resolution: 41.7 ns
```

| operation | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| all | 24.9 | 41 | 83 | 125 |
| limit | 24.3 | 41 | 83 | 125 |
| market | 26.4 | 41 | 83 | 125 |
| cancel | 24.1 | 41 | 42 | 84 |
| modify | 30.1 | 41 | 84 | 125 |

Throughput, untimed pass: median 67.6M ops/s, runs from 66.3M to 68.0M. With a timestamp around every op: median 29.4M, runs from 28.6M to 29.6M. Load average at the start of each run: 4.81 to 4.81. Every run is in `results/final-2026-10-28/`, built from commit `78d4f36`.
<!-- END generated -->

In a profile of the timed pass, 61.7% of samples land on the two counter
reads (`results/profiles/`). The `isb` before each read waits for every
earlier instruction to complete, so one operation's cache misses cannot
overlap the next operation's work. That is why the timed pass runs at less
than half the untimed rate, and why its latencies overstate the cost of an
operation inside a pipelined stream. The p50 sits at the counter's step of
about 42 ns; the means and tails carry the finer information.

## Profile-driven attempts, October 2026

`scripts/profile.py` sampled the untimed pass of the final engine
(`results/profiles/`). After the switch on operation type, which mispredicts
because the workload mixes types at random, the hottest lines were id-map
probes and the order loads in `match` and `rest`. Two changes came from that
profile:

* Single-probe cancel. Cancel found the slot, then `erase()` hashed the id
  and walked the same probe chain again. `take()` does both in one walk.
* Fibonacci hashing. Every probe starts with the hash, and backward-shift
  deletion rehashes each neighbor it inspects. The splitmix64 finalizer it
  replaced is two multiplies and three xor-shifts in a dependent chain;
  Fibonacci hashing is one multiply and a shift.

Each batch runs the unchanged build and the changed build interleaved, five
runs each. A batch is valid only if its unchanged build ran within 10% of
the baseline batch's median (`results/baseline/`, 50.0M ops/s). The check
reads only the unchanged build, so it cannot favor a change. Invalid batches
stay in the table.

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

* The A/A batch, one build against itself, differs by 0.5% at the median
  with overlapping ranges.
* Four of the six attempt batches are invalid. Two ran while another job
  held three cores. Two ran during a twenty-second slowdown that cut both
  builds to about a third of their speed. The validity rule was written
  after those came back; before that the plan was to re-run once.
* Fibonacci hashing is kept. In its valid batch every changed run beat every
  unchanged run: +16.5% at the median, and mean latency from 32.5 to 27.7 ns.
  The same pair of commits re-run on 2026-10-28 under the quieter conditions
  (`results/try-fibonacci-hash-2026-10-28/`) gave 55.4M to 67.8M ops/s,
  +22.6%, again with every changed run faster, and mean latency from 27.5 to
  23.9 ns. Neither batch separates the two candidate causes: the shorter
  dependency chain in front of each probe's load, or fewer collisions from
  the more even spacing it gives sequential ids.
* Single-probe cancel stays reverted: +0.6% in its valid batch, inside the
  noise. The second walk it removed was most likely in L1 already, and its
  hash can be computed while the first probe's load is in flight.

## Scale and endurance

August 2026, timed pass, before runs recorded load:

* Sharding (`--threads N`: one engine per thread on disjoint instruments,
  nothing shared): aggregate throughput peaks at 84.7M ops/s with 8 shards
  (`results/sharded.csv`). The per-shard rate falls from 24.5M at 2 shards
  to 18.3M at 4 and 10.6M at 8. Shards share the memory system, and the M5
  has 4 performance cores, so past 4 shards some run on efficiency cores.
* Endurance: 100M operations, the book growing to 328k resting orders, with
  the pool and id map sized for it (`--pool 524288 --idmap 1048576`):
  18.6M ops/s, p99.9 333 ns, max 52 us, zero allocations
  (`results/endurance.csv`). At default sizing the pool grew 4 times and an
  id-map rehash took 23 ms in one operation. With the id map sized far past
  the book, throughput fell to 13.3M ops/s, most likely because the table
  no longer fit in cache.

The same endurance run on 2026-10-22, with every allocation path counted, at
a load average of 7.5: zero allocations, 11.8M ops/s, p99.9 2.0 us, max
4.1 ms (`results/endurance_2026-10-22.txt`). The August latencies did not
reproduce under that load.

## Profiling

macOS has no `perf`, and VMs on Apple Silicon do not expose the PMU.
`scripts/profile.py` runs the README configuration 50 times under the
built-in `sample` profiler and flattens self samples per source line:

```bash
./scripts/profile.py                         # untimed pass
./scripts/profile.py --frame run_ops_gated   # timed pass
```

Inlined engine code is attributed to the harness line that called it, and
the stall time of the instructions before an `isb` lands on the `isb`, which
is why the timed pass's profile is dominated by its counter reads.

On bare-metal Linux, `perf stat` gives counters for the same run; divide by
the measured operation count for per-operation figures:

```bash
taskset -c 3 perf stat -e cycles,instructions,branch-misses,\
L1-dcache-load-misses,LLC-load-misses ./build/bench final --ops 1000000
```

A given seed produces the same workload on any machine, so before and after
pairs taken on one machine are comparable on that machine.
