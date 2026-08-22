# Hardware counter profiling

The benchmark reports wall-clock latency. To explain WHY a number moved, you
need hardware counters: cache misses, branch mispredicts, cycles and
instructions per operation.

## Linux (perf), the real tool

`perf` is Linux-only, and hypervisors generally do not virtualize the PMU, so
use a bare-metal Linux box:

```bash
# counters over a whole run
perf stat -e cycles,instructions,branches,branch-misses,\
cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,\
LLC-loads,LLC-load-misses \
  ./build/bench final --ops 1000000 --label perf-run

# where the cycles go, annotated per function and instruction
perf record -g ./build/bench final --ops 1000000
perf report
```

Convert totals to per-op numbers by dividing by the measured op count. What
to watch per optimization:

| counter | what a drop means |
|---|---|
| L1-dcache-load-misses | better locality (pool, flat ladder) |
| LLC-load-misses | working set fits cache, less pointer chasing |
| branch-misses | branch discipline, side specialization |
| instructions per cycle | less stalling overall (higher is better) |

For stable numbers: pin the process (`taskset -c 3`), set the governor to
`performance`, and ideally isolate the core (`isolcpus`) so the scheduler
keeps other work off it.

## macOS (this machine)

No `perf` here, and VMs on Apple Silicon cannot see the PMU either. In order
of usefulness:

```bash
# Instruments counter template (requires full Xcode, not just CLT):
xcrun xctrace record --template 'CPU Counters' \
  --launch -- ./build/bench final --ops 1000000
# then open the .trace bundle in Instruments
```

The Time Profiler template gives cycle attribution without raw counters. For
serious counter work, run the same binary on a Linux x86 box: the workload is
identical for a given seed, so each machine's before/after pairs are valid on
that machine.

Rule for this project: latency and throughput numbers quoted in the README
come from one machine, one governor state, one seed. Counter analysis is a
supplement, never a substitute for the latency percentiles.
