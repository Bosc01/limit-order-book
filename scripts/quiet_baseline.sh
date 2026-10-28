#!/bin/bash
# A quiet-machine baseline of the final engine, for the README headline.
# Refuses to start unless the 1-minute load average is at most 1.5, then
# runs bench final 5 times (2M measured ops, 100k warmup, seed 42) with 60 s
# between runs. Before each run it records the load average, power source,
# and UTC time, and adds them to that run's JSON as "pre_run". Writes
# results/final-quiet-DATE/ in the layout bench_report.sh uses, so
# results.py reads it, then prints the median, min, max, and spread.
#   scripts/quiet_baseline.sh [BUILD_DIR]      (default: build)
# GAP, MAX_LOAD and OUT override the gap, the load limit and the directory.
set -eu
DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"
BUILD="${1:-build}"
RUNS=5; OPS=2000000; WARMUP=100000; SEED=42
GAP="${GAP:-60}"; MAX_LOAD="${MAX_LOAD:-1.5}"
OUT="${OUT:-results/final-quiet-$(date +%Y-%m-%d)}"

load1() {
    if [ -r /proc/loadavg ]; then cut -d' ' -f1 /proc/loadavg
    else sysctl -n vm.loadavg | awk '{print $2}'; fi
}

# "AC Power" or "Battery Power" as a JSON string; null where it cannot be read.
power() {
    local p
    p="$(pmset -g batt 2>/dev/null | sed -n "1s/.*'\(.*\)'.*/\1/p")"
    if [ -n "$p" ]; then echo "\"$p\""; else echo null; fi
}

[ ! -e "$OUT" ] || { echo "$OUT already exists; move it or set OUT"; exit 1; }
# Build before the load check, so a compile cannot inflate the first run.
cmake --build "$BUILD" --target bench > /dev/null
l="$(load1)"
if awk -v l="$l" -v m="$MAX_LOAD" 'BEGIN { exit !(l > m) }'; then
    echo "1-minute load average is $l, above $MAX_LOAD; not starting"
    exit 1
fi

src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")"
commit="$(git -C "$src" rev-parse --short HEAD)"
git -C "$src" diff --quiet HEAD -- . ':!results' || commit="$commit-dirty"
mkdir -p "$OUT"
echo "final final $commit" > "$OUT/builds.txt"
echo "final Quiet baseline: 1-minute load $l at start (limit $MAX_LOAD), $GAP s between runs." \
    > "$OUT/notes.txt"

for r in $(seq 1 "$RUNS"); do
    [ "$r" -eq 1 ] || sleep "$GAP"
    when="$(date -u +%Y-%m-%dT%H:%M:%SZ)"; l="$(load1)"; p="$(power)"
    echo "run $r of $RUNS: $when, 1-minute load $l, power $p"
    echo "===== final run $r: bench final --ops $OPS --warmup $WARMUP --seed $SEED =====" \
        >> "$OUT/output.txt"
    "$BUILD/bench" final --ops "$OPS" --warmup "$WARMUP" --seed "$SEED" \
        --label "$(basename "$OUT")/final-$r" --json "$OUT/final-$r.json" >> "$OUT/output.txt"
    python3 - "$OUT/final-$r.json" "$when" "$l" "$p" <<'EOF'
import json, sys
path, when, load, power = sys.argv[1:]
text = open(path).read()
assert text.startswith("{\n"), path
pre = (f'  "pre_run": {{"recorded_utc": "{when}", "load_average_1m": {load}, '
       f'"power_source": {power}}},\n')
text = "{\n" + pre + text[2:]
json.loads(text)
open(path, "w").write(text)
EOF
done

echo
python3 - "$OUT" <<'EOF'
import json, pathlib, statistics, sys
d = pathlib.Path(sys.argv[1])
runs = [json.loads(p.read_text()) for p in sorted(d.glob("final-*.json"))]
print(f"{d}: {len(runs)} runs")
for key, name in [("untimed_throughput_ops_per_s", "untimed"),
                  ("throughput_ops_per_s", "timed  ")]:
    xs = [r[key] for r in runs]
    med = statistics.median(xs)
    print(f"{name} ops/s: median {med:,.0f}, min {min(xs):,.0f}, max {max(xs):,.0f}, "
          f"spread (max - min) / median {100 * (max(xs) - min(xs)) / med:.2f}%")
print("1-minute load before each run:", ", ".join(str(r["pre_run"]["load_average_1m"]) for r in runs))
print("power before each run:", ", ".join(str(r["pre_run"]["power_source"]) for r in runs))
EOF
