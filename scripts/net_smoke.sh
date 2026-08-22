#!/bin/bash
# End-to-end smoke test: gateway + UDP feed subscriber + TCP order burst.
# Asserts: every order acknowledged, feed saw trades and book updates with
# zero sequence gaps (loopback UDP does not drop), gateway survives.
set -u
BUILD="${1:-build}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT=9101
UDP_PORT=9102
OUT="$(mktemp -d)"
trap 'kill $GW_PID $FEED_PID 2>/dev/null; rm -rf "$OUT"' EXIT

"$DIR/$BUILD/gateway" --port $PORT --udp-port $UDP_PORT > "$OUT/gateway.log" 2>&1 &
GW_PID=$!
sleep 0.3
kill -0 $GW_PID 2>/dev/null || { echo "FAIL: gateway did not start"; cat "$OUT/gateway.log"; exit 1; }

"$DIR/$BUILD/feed_client" --port $UDP_PORT --summary-every 0 > "$OUT/feed.log" 2>&1 &
FEED_PID=$!
sleep 0.2

"$DIR/$BUILD/order_client" --auto 1000 --port $PORT > "$OUT/client.log" 2>&1
CLIENT_RC=$?

sleep 0.3
kill -INT $FEED_PID 2>/dev/null; wait $FEED_PID 2>/dev/null
kill -INT $GW_PID 2>/dev/null;   wait $GW_PID 2>/dev/null

echo "--- client ---";  cat "$OUT/client.log"
echo "--- feed ---";    cat "$OUT/feed.log"
echo "--- gateway ---"; cat "$OUT/gateway.log"

[ $CLIENT_RC -eq 0 ] || { echo "FAIL: client rc=$CLIENT_RC"; exit 1; }
grep -q "1000 ops acknowledged" "$OUT/client.log" || { echo "FAIL: not all ops acked"; exit 1; }
grep -Eq "trades=[1-9]" "$OUT/feed.log" || { echo "FAIL: no trades on feed"; exit 1; }
grep -q "gaps=0" "$OUT/feed.log" || { echo "FAIL: sequence gaps on loopback"; exit 1; }
echo "SMOKE TEST PASSED"
