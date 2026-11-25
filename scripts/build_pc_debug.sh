#!/usr/bin/env bash
# Project Dragon — Build PC Debug Binary, Asset Validator, and Automated Test Suite
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$ROOT/logs/build_pc_debug.log"
mkdir -p "$ROOT/logs"

{
    echo "=== Project Dragon PC Debug Build ($(date -u)) ==="
    cmake -S "$ROOT" -B "$ROOT/build-pc" -G Ninja \
        -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$ROOT/build-pc" -j"$(nproc)"
    echo ""
    echo "=== Running Automated Tests ==="
    "$ROOT/build-pc/dragon_tests"
} 2>&1 | tee "$LOG"
