#!/usr/bin/env bash
# Project Dragon — Setup OpenOrbis PS4 Toolchain v0.5.4 + LLVM 18 + Build Dependencies
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN_ROOT="$ROOT/toolchains/openorbis/OpenOrbis/PS4Toolchain"
TARBALL="$ROOT/toolchains/toolchain-llvm-18.tar.gz"
EXPECTED_SHA="3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526"

mkdir -p "$ROOT/toolchains" "$ROOT/logs"

echo "[SETUP] Checking host build tools..."
for tool in cmake ninja clang++ ld.lld curl python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[SETUP] Missing host tool: $tool (install clang-18 lld-18 llvm-18 cmake ninja-build curl python3)" >&2
        exit 1
    fi
done

if [ ! -f "$TOOLCHAIN_ROOT/lib/crt1.o" ]; then
    echo "[SETUP] Downloading OpenOrbis PS4 Toolchain v0.5.4 (LLVM 18)..."
    curl -fL --progress-bar \
        "https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz" \
        -o "$TARBALL"
    echo "$EXPECTED_SHA  $TARBALL" | sha256sum -c -
    mkdir -p "$ROOT/toolchains/openorbis"
    tar -xzf "$TARBALL" -C "$ROOT/toolchains/openorbis"
fi

echo "[SETUP] OpenOrbis PS4 Toolchain verified at: $TOOLCHAIN_ROOT"
ls -la "$TOOLCHAIN_ROOT/bin/linux" | head -n 10
