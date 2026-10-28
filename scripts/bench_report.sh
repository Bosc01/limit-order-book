#!/bin/bash
# Runs one batch of benchmark runs and keeps every run:
#   results/BATCH/NAME-I.json   one file per run (bench --json)
#   results/BATCH/output.txt    every run's full printout
#   results/BATCH/builds.txt    name, engine, and source commit of each entry
# Each entry is NAME=BUILD_DIR[:ENGINE]; the engine defaults to final. With
# several entries the runs interleave (A B, then B A, ...), so drift in
# machine load lands on every entry alike. Ends by printing the summary.
#   scripts/bench_report.sh baseline final=build
#   scripts/bench_report.sh try-x before=../old/build after=build
# RUNS (default 5), OPS (2000000) and WARMUP (100000) override the defaults.
set -eu
[ $# -ge 2 ] || { sed -n 2,12p "$0"; exit 1; }
BATCH="$1"; shift
RUNS="${RUNS:-5}"; OPS="${OPS:-2000000}"; WARMUP="${WARMUP:-100000}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"
OUT="results/$BATCH"
[ ! -e "$OUT" ] || { echo "$OUT already exists; choose a new batch name"; exit 1; }

names=(); builds=(); engines=()
for e in "$@"; do
    name="${e%%=*}"; rest="${e#*=}"; build="${rest%%:*}"; engine=final
    [ "$rest" = "$build" ] || engine="${rest#*:}"
    cmake --build "$build" --target bench > /dev/null
    names+=("$name"); builds+=("$build"); engines+=("$engine")
done

mkdir -p "$OUT"
for i in "${!names[@]}"; do
    src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${builds[$i]}/CMakeCache.txt")"
    commit="$(git -C "$src" rev-parse --short HEAD)"
    git -C "$src" diff --quiet HEAD -- . ':!results' || commit="$commit-dirty"
    echo "${names[$i]} ${engines[$i]} $commit" >> "$OUT/builds.txt"
done

order=("${!names[@]}"); reversed=()
for ((i = ${#names[@]} - 1; i >= 0; i--)); do reversed+=("$i"); done
for r in $(seq 1 "$RUNS"); do
    # Odd rounds run the entries in order, even rounds in reverse, so no
    # entry always goes first; position effects showed up as a fake +12%
    # when one build ran against itself in a fixed order.
    [ $((r % 2)) -eq 1 ] && idx=("${order[@]}") || idx=("${reversed[@]}")
    for i in "${idx[@]}"; do
        echo "run $r of $RUNS: ${names[$i]}"
        echo "===== ${names[$i]} run $r: bench ${engines[$i]} --ops $OPS --warmup $WARMUP =====" >> "$OUT/output.txt"
        # Power moves the numbers too, and the benchmark cannot see it.
        pmset -g batt 2>/dev/null | sed -n 2p >> "$OUT/output.txt" || true
        "${builds[$i]}/bench" "${engines[$i]}" --ops "$OPS" --warmup "$WARMUP" \
            --label "$BATCH/${names[$i]}-$r" --csv /dev/null \
            --json "$OUT/${names[$i]}-$r.json" >> "$OUT/output.txt"
    done
done
echo
python3 scripts/results.py summary "$OUT"
