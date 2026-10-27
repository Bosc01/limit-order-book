#!/bin/bash
# Gateway load test. For each client count, RUNS runs, each against a fresh
# gateway: order_client --clients N drives the seeded --auto mix closed loop
# over N persistent connections (WARMUP seconds, then DURATION measured).
# After the clients disconnect, the gateway must report zero resting orders,
# having canceled each connection's orders as it dropped; any other count
# fails the script. Writes medians, every run, and the machine to
# results/gateway_load.md.
# Usage: scripts/gateway_load.sh [build-dir]   (RUNS=5 WARMUP=1 DURATION=5)
set -eu
BUILD="${1:-build}"
RUNS="${RUNS:-5}"
WARMUP="${WARMUP:-1}"
DURATION="${DURATION:-5}"
CLIENTS="1 8 32 128"
SEED=42
PORT=9131
UDP_PORT=9132
DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"
OUT="results/gateway_load.md"
TMP="$(mktemp -d)"
GW_PID=""
trap 'if [ -n "$GW_PID" ]; then kill $GW_PID 2>/dev/null; fi; rm -rf "$TMP"' EXIT

cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$BUILD" --target gateway order_client -j > /dev/null

# Waits up to 10 s for at least $3 lines matching $2 in gateway log $1.
wait_for() {
    for _ in $(seq 100); do
        [ "$(grep -c -- "$2" "$1")" -ge "$3" ] && return 0
        sleep 0.1
    done
    echo "FAIL: gateway log never showed $3 line(s) matching '$2'"
    cat "$1"
    exit 1
}

STARTED="$(date -u '+%Y-%m-%d %H:%M UTC')"
LOAD="$(uptime | sed 's/.*load average[s]*: //')"
: > "$TMP/runs.tsv"
for n in $CLIENTS; do
    for r in $(seq 1 "$RUNS"); do
        log="$TMP/gateway_${n}_$r.log"
        "$BUILD/gateway" --port $PORT --udp-port $UDP_PORT > "$log" 2>&1 &
        GW_PID=$!
        wait_for "$log" "orders on" 1
        line="$("$BUILD/order_client" --clients "$n" --warmup "$WARMUP" \
                --seconds "$DURATION" --seed $SEED --port $PORT)"
        wait_for "$log" "disconnected," "$n"
        kill -INT $GW_PID
        wait $GW_PID
        GW_PID=""

        get() { echo "$line" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
        canceled="$(awk '/disconnected,/ { s += $5 } END { print s + 0 }' "$log")"
        resting="$(sed -n 's/.*resting orders: \([0-9]*\)).*/\1/p' "$log")"
        printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$r" "$(get ops_per_s)" \
            "$(get p50_us)" "$(get p99_us)" "$canceled" "$resting" >> "$TMP/runs.tsv"
        echo "clients=$n run=$r $(get ops_per_s) orders/s p50=$(get p50_us)us" \
             "p99=$(get p99_us)us canceled_on_disconnect=$canceled resting_after=$resting"

        [ "$resting" = 0 ] || { echo "FAIL: $resting orders left resting after disconnect"; cat "$log"; exit 1; }
        [ "$canceled" -gt 0 ] || { echo "FAIL: nothing rested at disconnect, so the check proved nothing"; exit 1; }
    done
done

# Median of column $2 over the runs with $1 clients.
median() {
    awk -F'\t' -v n="$1" -v c="$2" '$1 == n { print $c }' "$TMP/runs.tsv" | sort -n |
        awk '{ v[NR] = $1 } END { print v[int((NR + 1) / 2)] }'
}

CPU="$( (lscpu 2>/dev/null | sed -n 's/^Model name: *//p'
         sysctl -n machdep.cpu.brand_string 2>/dev/null) | head -1)"
VIRT="$(systemd-detect-virt 2>/dev/null; true)"
CXX="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD/CMakeCache.txt")"
COMMIT="$(git rev-parse --short HEAD)$(git diff --quiet HEAD -- . ':!results' || echo ' with uncommitted changes')"
TOTAL=$(( $(echo $CLIENTS | wc -w) * RUNS ))

{
cat <<EOF
# Gateway load test

Closed-loop clients against the TCP order gateway over loopback. Each client
is a thread with one persistent connection and one order in flight: it sends
the next order when the exec report for the previous one arrives. Every client
runs the seeded \`order_client --auto\` mix (60% passive limits, 15% aggressive
limits, 10% market orders, 10% cancels, 5% modifies) with seed $SEED plus its
thread index. An order here is any of those five messages; each gets exactly
one exec report. Each run starts a fresh gateway, warms up for $WARMUP s, then
measures for $DURATION s. Round trips are measured at the client, from send()
to the last byte of the exec report, and pooled across clients. Every figure
in the first table is the median of $RUNS runs.

* command: \`scripts/gateway_load.sh $BUILD\` (RUNS=$RUNS WARMUP=$WARMUP DURATION=$DURATION)
* started: $STARTED
* commit: $COMMIT
* machine: ${CPU:-unknown CPU}, $(getconf _NPROCESSORS_ONLN) logical CPUs, virtualization: ${VIRT:-unknown}
* kernel: $(uname -srm)
* compiler: $("$CXX" --version | head -1)
* load average at start: $LOAD

| clients | orders/s | p50 round trip (us) | p99 round trip (us) |
|--:|--:|--:|--:|
EOF
for n in $CLIENTS; do
    echo "| $n | $(median "$n" 3) | $(median "$n" 4) | $(median "$n" 5) |"
done
cat <<EOF

## Cancel on disconnect

At the end of every run each client closes its connection with orders still
resting. The gateway cancels a connection's resting orders when it sees the
close. Once it has logged all of the disconnects, the gateway is stopped and
reports how many orders remain on its book. The script fails if that count is
not zero, or if no orders were resting to cancel.

Resting orders after disconnect: 0 in all $TOTAL runs.

## Every run

| clients | run | orders/s | p50 (us) | p99 (us) | canceled on disconnect | resting after |
|--:|--:|--:|--:|--:|--:|--:|
EOF
awk -F'\t' '{ printf "| %s | %s | %s | %s | %s | %s | %s |\n", $1, $2, $3, $4, $5, $6, $7 }' "$TMP/runs.tsv"
cat <<EOF

## Reading these numbers

* Clients and gateway share one machine. With more client threads than
  cores, clients compete with the gateway thread for CPU, so round trips at
  high client counts include client-side scheduling delay.
* The gateway is one thread on one poll() loop, and it sends a UDP book
  snapshot after every order. No feed subscriber runs during the test; the
  datagrams are sent all the same.
* Closed loop: no client has more than one order outstanding, so the offered
  load falls whenever the gateway slows down. The percentiles describe
  service time at each concurrency, not latency at a fixed arrival rate.
EOF
} > "$OUT"

echo
echo "wrote $OUT"
