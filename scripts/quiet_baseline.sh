#!/bin/bash
# A quiet-machine baseline of the final engine, for the README headline.
# Refuses to start unless CPU idle is at least 90% and the 1-minute load
# average is at most 3 (macOS reports about 2 to 2.5 on an idle machine, so
# idle is the stricter test). Then runs bench final 5 times (2M measured
# ops, 100k warmup, seed 42) with 60 s between runs. Before each run it
# records CPU idle, the load average, power source, and UTC time, and adds
# them to that run's JSON as "pre_run". Writes
# results/final-quiet-DATE/ in the layout bench_report.sh uses, so
# results.py reads it, then prints the median, min, max, and spread.
#   scripts/quiet_baseline.sh [BUILD_DIR]      (default: build)
# GAP, MAX_LOAD, MIN_IDLE and OUT override the gap, the limits and the
# directory.
set -eu
DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"
BUILD="${1:-build}"
RUNS=5; OPS=2000000; WARMUP=100000; SEED=42
GAP="${GAP:-60}"; MAX_LOAD="${MAX_LOAD:-3}"; MIN_IDLE="${MIN_IDLE:-90}"
OUT="${OUT:-results/final-quiet-$(date +%Y-%m-%d)}"

load1() {
    if [ -r /proc/loadavg ]; then cut -d' ' -f1 /proc/loadavg
    else sysctl -n vm.loadavg | awk '{print $2}'; fi
}

# CPU idle percent from the second sample of top, since the first covers
# no interval. null where top -l is not available (Linux).
idle() {
    local i
    i="$(top -l 2 -n 0 2>/dev/null | grep 'CPU usage' | tail -1 | sed -n 's/.* \([0-9.]*\)% idle.*/\1/p')"
    if [ -n "$i" ]; then echo "$i"; else echo null; fi
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
l="$(load1)"; i="$(idle)"
if [ "$i" != null ] && awk -v i="$i" -v m="$MIN_IDLE" 'BEGIN { exit !(i < m) }'; then
    echo "CPU idle is $i%, below $MIN_IDLE%; not starting"
    exit 1
fi
if awk -v l="$l" -v m="$MAX_LOAD" 'BEGIN { exit !(l > m) }'; then
    echo "1-minute load average is $l, above $MAX_LOAD; not starting"
    exit 1
fi

src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")"
commit="$(git -C "$src" rev-parse --short HEAD)"
git -C "$src" diff --quiet HEAD -- . ':!results' || commit="$commit-dirty"
mkdir -p "$OUT"
echo "final final $commit" > "$OUT/builds.txt"
echo "final Quiet baseline: CPU idle $i% and 1-minute load $l at start (limits $MIN_IDLE% and $MAX_LOAD), $GAP s between runs." \
    > "$OUT/notes.txt"

for r in $(seq 1 "$RUNS"); do
    [ "$r" -eq 1 ] || sleep "$GAP"
    i="$(idle)"; when="$(date -u +%Y-%m-%dT%H:%M:%SZ)"; l="$(load1)"; p="$(power)"
    echo "run $r of $RUNS: $when, CPU idle $i%, 1-minute load $l, power $p"
    echo "===== final run $r: bench final --ops $OPS --warmup $WARMUP --seed $SEED =====" \
        >> "$OUT/output.txt"
    "$BUILD/bench" final --ops "$OPS" --warmup "$WARMUP" --seed "$SEED" \
        --label "$(basename "$OUT")/final-$r" --json "$OUT/final-$r.json" >> "$OUT/output.txt"
    python3 - "$OUT/final-$r.json" "$when" "$l" "$p" "$i" <<'EOF'
import json, sys
path, when, load, power, idle = sys.argv[1:]
text = open(path).read()
assert text.startswith("{\n"), path
pre = (f'  "pre_run": {{"recorded_utc": "{when}", "load_average_1m": {load}, '
       f'"power_source": {power}, "cpu_idle_pct": {idle}}},\n')
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
print("CPU idle % before each run:", ", ".join(str(r["pre_run"]["cpu_idle_pct"]) for r in runs))
print("power before each run:", ", ".join(str(r["pre_run"]["power_source"]) for r in runs))
EOF
