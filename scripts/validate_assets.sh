#!/usr/bin/env bash
# Project Dragon — Validate User-Provided COD Online 1.1.1.13 Assets & Generate Compatibility Report
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="${1:-${CODOL_GAME_DIR:-/home/antigravity/codol-game/extracted/codol-classic}}"
shift || true

mkdir -p "$ROOT/logs"
"$ROOT/build-pc/dragon_validate" --game "$GAME_DIR" --out "$ROOT/docs/COMPATIBILITY_REPORT.md" "$@" | tee "$ROOT/logs/validate_assets.log"
