#!/usr/bin/env bash
# Project Dragon — Package Standalone PlayStation 4 PKG (LibOrbisPkg / OpenOrbis PkgTool.Core)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="${1:-${CODOL_GAME_DIR:-/home/antigravity/codol-game/extracted/codol-classic}}"
LOG="$ROOT/logs/package_ps4.log"
mkdir -p "$ROOT/logs" "$ROOT/dist"

export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$ROOT/toolchains/openorbis/OpenOrbis/PS4Toolchain}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

TOOLS="$OO_PS4_TOOLCHAIN/bin/linux"
PKGTOOL="$TOOLS/PkgTool.Core"
if [ -x "$ROOT/toolchains/liborbispkg/PkgTool.Core" ]; then
    PKGTOOL="$ROOT/toolchains/liborbispkg/PkgTool.Core"
fi
STAGE="$ROOT/build-ps4/pkg_stage"
LOCAL_ICON="$ROOT/build-ps4/sce_sys/icon0.png"
TITLE_ID="${DRAGON_TITLE_ID:-DRGN00113}"
TITLE="${DRAGON_TITLE:-Call of Duty Online}"
VERSION="${DRAGON_VERSION:-01.00}"
CONTENT_ID="IV0000-${TITLE_ID}_00-PROJECTDRAGON000"

{
    echo "=== Project Dragon PS4 PKG Packaging ($(date -u)) ==="
    echo "Using PkgTool : $PKGTOOL"
    echo "Game data dir : $GAME_DIR"
    if [ ! -f "$ROOT/build-ps4/eboot.bin" ]; then
        echo "ERROR: build-ps4/eboot.bin not found. Run scripts/build_ps4.sh first." >&2
        exit 1
    fi
    if [ ! -f "$GAME_DIR/codoMP_client_shipRetail.exe" ]; then
        echo "ERROR: Original client executable not found: $GAME_DIR/codoMP_client_shipRetail.exe" >&2
        echo "Pass your Call of Duty Online 1.1.1.13 directory as the first argument or set CODOL_GAME_DIR" >&2
        echo "so the authentic application icon (RT_GROUP_ICON ID 1 / RT_ICON ID 5) can be extracted locally." >&2
        exit 1
    fi

    rm -rf "$STAGE"
    mkdir -p "$STAGE/sce_sys/about" "$STAGE/sce_module" "$ROOT/build-ps4/sce_sys"

    # Extract authentic 512x512 RGBA icon0.png locally from codoMP_client_shipRetail.exe (preserving proportions & alpha)
    python3 "$ROOT/tools/dragon_pe_inspect/extract_ps4_icon.py" \
        "$GAME_DIR/codoMP_client_shipRetail.exe" "$LOCAL_ICON"

    cp "$ROOT/build-ps4/eboot.bin" "$STAGE/eboot.bin"
    cp "$LOCAL_ICON" "$STAGE/sce_sys/icon0.png"
    cp "$OO_PS4_TOOLCHAIN/samples/piglet/sce_sys/about/right.sprx" "$STAGE/sce_sys/about/right.sprx"
    cp "$OO_PS4_TOOLCHAIN/samples/piglet/sce_module/"*.prx "$STAGE/sce_module/"

    SFO="$STAGE/sce_sys/param.sfo"
    "$PKGTOOL" sfo_new "$SFO"
    "$PKGTOOL" sfo_setentry "$SFO" APP_TYPE --type Integer --maxsize 4 --value 1
    "$PKGTOOL" sfo_setentry "$SFO" APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
    "$PKGTOOL" sfo_setentry "$SFO" ATTRIBUTE --type Integer --maxsize 4 --value 0x2
    "$PKGTOOL" sfo_setentry "$SFO" CATEGORY --type Utf8 --maxsize 4 --value gd
    "$PKGTOOL" sfo_setentry "$SFO" CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
    "$PKGTOOL" sfo_setentry "$SFO" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
    "$PKGTOOL" sfo_setentry "$SFO" SYSTEM_VER --type Integer --maxsize 4 --value 0
    "$PKGTOOL" sfo_setentry "$SFO" TITLE --type Utf8 --maxsize 128 --value "$TITLE"
    "$PKGTOOL" sfo_setentry "$SFO" TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
    "$PKGTOOL" sfo_setentry "$SFO" VERSION --type Utf8 --maxsize 8 --value "$VERSION"

    ( cd "$STAGE" && "$TOOLS/create-gp4" -out pkg.gp4 --content-id="$CONTENT_ID" \
        --files "eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx" )
    ( cd "$STAGE" && "$PKGTOOL" pkg_build pkg.gp4 "$ROOT/dist" )

    PKG="$ROOT/dist/${CONTENT_ID}.pkg"
    echo ""
    echo "=== PS4 PKG Verification ==="
    ls -lh "$PKG"
    "$PKGTOOL" pkg_listentries "$PKG"
    python3 - "$PKG" <<'PY'
import sys
hdr = open(sys.argv[1], "rb").read(16)
assert hdr[:4] == b"\x7fCNT", f"Invalid PS4 PKG magic: {hdr[:4]!r}"
print("PS4 PKG magic verified: 0x7F 'C' 'N' 'T' (PlayStation 4 Content Package)")
PY
    "$PKGTOOL" pkg_validate "$PKG"
} 2>&1 | tee "$LOG"
