#!/usr/bin/env bash
# Project Dragon — Build Native PlayStation 4 (OrbisOS x86-64) ELF -> OELF -> eboot.bin (FSELF)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$ROOT/logs/build_ps4.log"
mkdir -p "$ROOT/logs"

export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$ROOT/toolchains/openorbis/OpenOrbis/PS4Toolchain}"

{
    echo "=== Project Dragon Native PS4 Build ($(date -u)) ==="
    echo "OO_PS4_TOOLCHAIN=$OO_PS4_TOOLCHAIN"
    cmake -S "$ROOT" -B "$ROOT/build-ps4" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/openorbis-ps4-toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$ROOT/build-ps4" -j"$(nproc)"

    echo ""
    echo "=== Binary Format Verification ==="
    file "$ROOT/build-ps4/project_dragon.elf" "$ROOT/build-ps4/project_dragon.oelf" "$ROOT/build-ps4/eboot.bin"
    ls -lh "$ROOT/build-ps4/project_dragon.elf" "$ROOT/build-ps4/project_dragon.oelf" "$ROOT/build-ps4/eboot.bin"
    llvm-readelf-18 -h -l "$ROOT/build-ps4/project_dragon.oelf" | head -n 35 || true
    python3 - "$ROOT/build-ps4/eboot.bin" <<'PY'
import sys, struct
data = open(sys.argv[1], "rb").read(32)
magic = struct.unpack_from("<I", data, 0)[0]
assert magic == 0x1D3D154F, f"Unexpected FSELF magic: 0x{magic:08X} (expected 0x1D3D154F)"
print(f"eboot.bin FSELF magic verified: 0x{magic:08X} (PS4 Signed ELF / FSELF)")
PY
} 2>&1 | tee "$LOG"
