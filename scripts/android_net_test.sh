#!/usr/bin/env bash
# Run the Android x86_64 live smoke test on a booted emulator (adb must be in
# PATH — invoked from the android-net CI job's emulator-runner script, which only
# runs this once the AVD is booted). Runs the test RUNS times with the net_posix
# OR_NET_DIAG trace on, failing on the FIRST failure so any fault surfaces with
# its exact cause. This is a regression gate for the stale-OpenSSL-error-queue bug
# (a would-block SSL_read misreported as fatal) that made early connects flaky.
set -uo pipefail

BIN="${1:-build/net_live_android}"
RUNS="${RUNS:-5}"

adb wait-for-device
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
