#!/usr/bin/env bash
# Build a static OpenSSL (libssl.a + libcrypto.a + headers) for one Android ABI
# using the NDK toolchain, staged under third_party/openssl-android/<abi>/ exactly
# like the per-ABI raylib install. The Android online client (net_posix.c with
# -DOR_TLS) links these for wss://. Nothing is committed — CI runs this before
# `make android`, and the result is gitignored. Idempotent: skips if already built.
#
# Usage: build_openssl_android.sh [abi]     (abi: arm64-v8a | x86_64, default arm64-v8a)
# Env:   ANDROID_NDK (required), ANDROID_API (default 24), OPENSSL_VERSION (default 3.0.16)
set -euo pipefail

ABI="${1:-arm64-v8a}"
API="${ANDROID_API:-24}"
VER="${OPENSSL_VERSION:-3.0.16}"
: "${ANDROID_NDK:?set ANDROID_NDK to the NDK root}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/third_party/openssl-android/$ABI"
if [ -f "$OUT/lib/libssl.a" ] && [ -f "$OUT/lib/libcrypto.a" ]; then
    echo "[openssl-android] $ABI already built at $OUT"
    exit 0
fi

case "$ABI" in
    arm64-v8a)   TARGET=android-arm64 ;;
    x86_64)      TARGET=android-x86_64 ;;
    armeabi-v7a) TARGET=android-arm ;;
    x86)         TARGET=android-x86 ;;
    *) echo "unknown ABI: $ABI" >&2; exit 1 ;;
esac

TOOLCHAIN="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64"
export ANDROID_NDK_ROOT="$ANDROID_NDK"
export PATH="$TOOLCHAIN/bin:$PATH"

SRC="$ROOT/build/openssl-src"
mkdir -p "$SRC"
cd "$SRC"
TARBALL="openssl-$VER.tar.gz"
if [ ! -d "openssl-$VER" ]; then
    curl -fsSL -o "$TARBALL" \
        "https://github.com/openssl/openssl/releases/download/openssl-$VER/$TARBALL"
    tar xzf "$TARBALL"
fi
cd "openssl-$VER"

# Static libs only; no test suite. -D__ANDROID_API__ pins the platform level so
# the compiled objects match the app's minSdk.
./Configure "$TARGET" -D__ANDROID_API__="$API" no-shared no-tests \
    --prefix="$OUT" --openssldir="$OUT/ssl"
make -j"$(nproc)" build_libs
make install_dev

echo "[openssl-android] built $ABI ($TARGET, API $API, OpenSSL $VER) -> $OUT"
