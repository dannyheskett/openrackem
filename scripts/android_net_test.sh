#!/usr/bin/env bash
# Run the Android x86_64 live smoke test on a booted emulator (adb must be in
# PATH — invoked from the android-net CI job's emulator-runner script). Two
# phases:
#   1. Warmup: a freshly booted AVD reports boot_completed a second or two before
#      its DNS resolver and network actually work, so an immediate getaddrinfo
#      returns EAI_NONAME, or every connect fails with ENETUNREACH. That is
#      emulator readiness, not a product fault, so retry ONLY those two
#      conditions (bounded: 30 tries) until the server answers; any other
#      failure fails hard.
#   2. Strict: run the test RUNS times and fail on the FIRST failure. This is the
#      regression gate for the stale-OpenSSL-error-queue bug (a would-block
#      SSL_read misreported as fatal) — after the ERR_clear_error() fix every run
#      must be clean, with no [net_posix] fail lines.
set -uo pipefail

BIN="${1:-build/net_live_android}"
RUNS="${RUNS:-5}"

adb wait-for-device
adb push "$BIN" /data/local/tmp/net_live
adb shell chmod 755 /data/local/tmp/net_live

run_once() { adb shell "OR_NET_DIAG=1 /data/local/tmp/net_live" 2>&1; }

echo "== phase 1: wait for emulator DNS/network readiness =="
ready=0
for i in $(seq 1 30); do
    out="$(run_once)"; echo "$out"
    if echo "$out" | grep -q "OK: live wss round-trip verified"; then ready=1; break; fi
    if echo "$out" | grep -q "fail@getaddrinfo"; then
        echo "[warmup ${i}] resolver not ready yet; waiting"
        sleep 3
        continue
    fi
    # The same readiness gap, one layer down: the emulator's network
    # interface can come up after boot_completed, and until then every
    # connect fails with ENETUNREACH (errno 101).
    if echo "$out" | grep -q "fail@connect errno=101"; then
        echo "[warmup ${i}] network not up yet; waiting"
        sleep 3
        continue
    fi
    echo "unexpected failure during warmup (not a DNS-readiness issue)"; exit 1
done
[ "$ready" = 1 ] || { echo "server never became resolvable on the emulator"; exit 1; }

echo "== phase 2: strict ${RUNS}x =="
for i in $(seq 1 "$RUNS"); do
    echo "== run ${i} =="
    out="$(run_once)"; echo "$out"
    echo "$out" | grep -q "OK: live wss round-trip verified" || { echo "RUN ${i} FAILED"; exit 1; }
done
echo "all ${RUNS} strict runs passed"
