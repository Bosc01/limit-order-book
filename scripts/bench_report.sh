#!/bin/bash
# Reproduces the README benchmark table: RUNS back-to-back runs of the final
# engine (2M measured operations each, seed 42), with every run's full output
# and CSV rows saved under results/, then the median of each figure.
# Usage: scripts/bench_report.sh [build-dir]    (RUNS=5 by default)
set -eu
BUILD="${1:-build}"
RUNS="${RUNS:-5}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"
STAMP="$(date +%Y-%m-%d)"
LOG="results/bench_$STAMP.txt"
CSV="results/bench_$STAMP.csv"
rm -f "$LOG" "$CSV"

for i in $(seq 1 "$RUNS"); do
    echo "===== run $i of $RUNS: $BUILD/bench final --ops 2000000 --warmup 100000 =====" >> "$LOG"
    "$BUILD/bench" final --ops 2000000 --warmup 100000 \
        --label "$STAMP-run$i" --csv "$CSV" >> "$LOG"
done

# Median of column $2 over the CSV rows for operation kind $1.
median() {
    awk -F, -v k="$1" -v c="$2" '$5 == k { print $c }' "$CSV" | sort -n |
        awk '{ v[NR] = $1 } END { print v[int((NR + 1) / 2)] }'
}

grep -m1 '^machine:' "$LOG"
grep -m1 '^build:' "$LOG" | sed 's/; load average.*//'
echo "load average at the start of each run:"
grep 'load average' "$LOG" | sed 's/.*load average at start: /  /'
echo "timer resolution: $(grep -m1 -o 'resolution [0-9.]* ns' "$LOG" | cut -d' ' -f2) ns"
echo
echo "median of $RUNS runs, nanoseconds per operation:"
printf "  %-7s %8s %6s %6s %7s\n" kind mean p50 p99 p99.9
for k in ALL limit market cancel modify; do
    printf "  %-7s %8s %6s %6s %7s\n" "$k" "$(median $k 7)" "$(median $k 8)" \
        "$(median $k 10)" "$(median $k 11)"
done
echo
echo "throughput, median: $(median ALL 13) ops/s"
echo "throughput per run: $(awk -F, '$5 == "ALL" { printf "%s ", $13 }' "$CSV")"
echo "heap allocations in measured regions: $(grep -c 'allocations in measured region: 0 ' "$LOG") of $RUNS runs at zero"
echo
echo "saved: $LOG and $CSV"
