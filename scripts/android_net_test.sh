#!/usr/bin/env bash
# Run the Android x86_64 live smoke test on a booted emulator (adb must be in
# PATH — invoked from the android-net CI job's emulator-runner script). Waits for
# the emulator's network/DNS to come up, then runs the test RUNS times with the
# net_posix OR_NET_DIAG trace on, failing on the FIRST failure so any fault
# surfaces with its exact cause rather than being masked by a retry.
set -uo pipefail

BIN="${1:-build/net_live_android}"
RUNS="${RUNS:-5}"

adb wait-for-device

echo "== waiting for emulator network + DNS =="
for i in $(seq 1 40); do
    if adb shell 'ping -c1 -W2 openrackem-server.fly.dev >/dev/null 2>&1 && echo up' | grep -q up; then
        echo "network ready after ${i} tries"
        break
    fi
    sleep 2
done
echo "== ping probe =="
adb shell 'ping -c2 -W3 openrackem-server.fly.dev' || echo "PING STILL FAILING"

adb push "$BIN" /data/local/tmp/net_live
adb shell chmod 755 /data/local/tmp/net_live

for i in $(seq 1 "$RUNS"); do
    echo "== run ${i} =="
    adb shell "OR_NET_DIAG=1 /data/local/tmp/net_live" 2>&1 | tee out.txt
    if ! grep -q "OK: live wss round-trip verified" out.txt; then
        echo "RUN ${i} FAILED"
        exit 1
    fi
done
echo "all ${RUNS} runs passed"
