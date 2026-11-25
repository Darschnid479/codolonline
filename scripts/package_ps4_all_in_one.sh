#!/usr/bin/env bash
# Project Dragon — Build Self-Contained Personal All-in-One PlayStation 4 PKG
# Bundles Project Dragon native PS4 eboot.bin + user's lawfully supplied COD Online 1.1.1.13 assets
# into /app0/codol inside a single installable PS4 .pkg (FOR PERSONAL CONSOLE USE ONLY — DO NOT REDISTRIBUTE).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="${1:-${CODOL_GAME_DIR:-/home/antigravity/codol-game/extracted/codol-classic}}"
INCLUDE_VIDEOS=1
if [ "${2:-}" = "--no-videos" ] || [ "${1:-}" = "--no-videos" ]; then
    INCLUDE_VIDEOS=0
    if [ "${1:-}" = "--no-videos" ]; then
        GAME_DIR="${CODOL_GAME_DIR:-/home/antigravity/codol-game/extracted/codol-classic}"
    fi
fi

LOG="$ROOT/logs/package_ps4_all_in_one.log"
OUT_DIR="$ROOT/dist/all-in-one"
STAGE="$ROOT/build-ps4/pkg_stage_all_in_one"
mkdir -p "$ROOT/logs" "$OUT_DIR"

export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$ROOT/toolchains/openorbis/OpenOrbis/PS4Toolchain}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

TOOLS="$OO_PS4_TOOLCHAIN/bin/linux"
PKGTOOL="$TOOLS/PkgTool.Core"
if [ -x "$ROOT/toolchains/liborbispkg/PkgTool.Core" ]; then
    PKGTOOL="$ROOT/toolchains/liborbispkg/PkgTool.Core"
fi

TITLE_ID="${DRAGON_TITLE_ID:-DRGN00113}"
TITLE="${DRAGON_TITLE:-Call of Duty Online}"
VERSION="${DRAGON_VERSION:-01.00}"
CONTENT_ID="IV0000-${TITLE_ID}_00-PROJECTDRAGON000"
LOCAL_ICON="$ROOT/build-ps4/sce_sys/icon0.png"

{
    echo "=== Project Dragon ALL-IN-ONE PS4 PKG Packaging ($(date -u)) ==="
    echo "NOTE: Bundles user-provided CODOL 1.1.1.13 assets into /app0/codol for personal PS4 install only."
    echo "Using PkgTool : $PKGTOOL"
    echo "Game data dir : $GAME_DIR"
    echo "Include video : $INCLUDE_VIDEOS"

    if [ ! -f "$ROOT/build-ps4/eboot.bin" ]; then
        echo "ERROR: build-ps4/eboot.bin not found. Run scripts/build_ps4.sh first." >&2
        exit 1
    fi
    if [ ! -f "$GAME_DIR/codoMP_client_shipRetail.exe" ] || [ ! -d "$GAME_DIR/client_shipRetail" ] || [ ! -d "$GAME_DIR/main" ]; then
        echo "ERROR: Valid CODOL 1.1.1.13 install not found at: $GAME_DIR" >&2
        echo "Expected: codoMP_client_shipRetail.exe, client_shipRetail/, main/" >&2
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

    # Generate complete GP4 XML referencing native PS4 app files + user's CODOL assets directly (zero 10 GB staging copy!)
    python3 - "$STAGE" "$GAME_DIR" "$CONTENT_ID" "$INCLUDE_VIDEOS" <<'PY'
import datetime, os, sys
from xml.sax.saxutils import escape

stage, game_dir, content_id, include_videos = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3], sys.argv[4] == "1"

files = [
    ("eboot.bin", "eboot.bin"),
    ("sce_sys/param.sfo", "sce_sys/param.sfo"),
    ("sce_sys/icon0.png", "sce_sys/icon0.png"),
    ("sce_sys/about/right.sprx", "sce_sys/about/right.sprx"),
    ("sce_module/libc.prx", "sce_module/libc.prx"),
    ("sce_module/libSceFios2.prx", "sce_module/libSceFios2.prx"),
]

# Required key/wavelet table source (read-only; never executed on PS4) + original root startup/localization files
files.append(("codol/codoMP_client_shipRetail.exe", os.path.join(game_dir, "codoMP_client_shipRetail.exe")))
for root_file in ("localization.txt", "splash.bmp", "logo.bmp"):
    full = os.path.join(game_dir, root_file)
    if os.path.isfile(full):
        files.append(("codol/" + root_file, full))

# Include all FastFiles under client_shipRetail/
for root_sub in ("client_shipRetail",):
    base = os.path.join(game_dir, root_sub)
    for dp, dns, fns in os.walk(base):
        dns.sort()
        for fn in sorted(fns):
            if fn.lower().endswith(".ff"):
                full = os.path.join(dp, fn)
                rel = os.path.relpath(full, game_dir).replace("\\", "/")
                files.append(("codol/" + rel, full))

# Include all .iwd archives under main/ (and optionally main/video/*.bk2)
main_dir = os.path.join(game_dir, "main")
for dp, dns, fns in os.walk(main_dir):
    dns.sort()
    rel_dir = os.path.relpath(dp, game_dir).replace("\\", "/")
    if rel_dir.startswith("main/demo"):
        continue
    if rel_dir.startswith("main/video") and not include_videos:
        continue
    for fn in sorted(fns):
        low = fn.lower()
        if low.endswith(".iwd") or (include_videos and low.endswith(".bk2")):
            full = os.path.join(dp, fn)
            rel = os.path.relpath(full, game_dir).replace("\\", "/")
            files.append(("codol/" + rel, full))

# Build nested directory tree for <rootdir>
dir_tree = {}
total_bytes = 0
for targ, orig in files:
    orig_abs = orig if os.path.isabs(orig) else os.path.join(stage, orig)
    total_bytes += os.path.getsize(orig_abs)
    parts = targ.split("/")[:-1]
    cur = dir_tree
    for p in parts:
        cur = cur.setdefault(p, {})

def emit_dirs(node, indent="\t\t"):
    lines = []
    for name in sorted(node.keys()):
        children = node[name]
        if children:
            lines.append(f'{indent}<dir targ_name="{escape(name)}">')
            lines.extend(emit_dirs(children, indent + "\t"))
            lines.append(f'{indent}</dir>')
        else:
            lines.append(f'{indent}<dir targ_name="{escape(name)}" />')
    return lines

ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
lines = [
    '<?xml version="1.0"?>',
    '<psproject xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" fmt="gp4" version="1000">',
    '\t<volume>',
    '\t\t<volume_type>pkg_ps4_app</volume_type>',
    '\t\t<volume_id>PS4VOLUME</volume_id>',
    f'\t\t<volume_ts>{ts}</volume_ts>',
    f'\t\t<package content_id="{escape(content_id)}" passcode="00000000000000000000000000000000" storage_type="digital50" app_type="full" />',
    '\t\t<chunk_info chunk_count="1" scenario_count="1">',
    '\t\t\t<chunks>',
    '\t\t\t\t<chunk id="0" layer_no="0" label="Chunk #0" />',
    '\t\t\t</chunks>',
    '\t\t\t<scenarios default_id="0">',
    '\t\t\t\t<scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>',
    '\t\t\t</scenarios>',
    '\t\t</chunk_info>',
    '\t</volume>',
    '\t<files img_no="0">',
]
for targ, orig in files:
    lines.append(f'\t\t<file targ_path="{escape(targ)}" orig_path="{escape(orig)}" />')
lines.append('\t</files>')
lines.append('\t<rootdir>')
lines.extend(emit_dirs(dir_tree))
lines.append('\t</rootdir>')
lines.append('</psproject>')

gp4_path = os.path.join(stage, "pkg.gp4")
with open(gp4_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")

print(f"Generated {gp4_path}: {len(files)} files, {total_bytes / (1024**3):.2f} GiB payload")
PY

    echo "Building All-in-One PS4 PKG (this streams ~9-10 GiB directly to disk via LibOrbisPkg)..."
    ( cd "$STAGE" && "$PKGTOOL" pkg_build pkg.gp4 "$OUT_DIR" )

    PKG="$OUT_DIR/${CONTENT_ID}.pkg"
    echo ""
    echo "=== All-in-One PS4 PKG Verification ==="
    ls -lh "$PKG"
    "$PKGTOOL" pkg_listentries "$PKG"
    echo "=== Verifying Inner PFS Contents (/app0/codol) ==="
    "$PKGTOOL" pkg_listfiles "$PKG" | head -n 30
    FILE_COUNT="$("$PKGTOOL" pkg_listfiles "$PKG" | wc -l)"
    echo "Total entries listed inside PKG inner PFS: $FILE_COUNT"
    python3 - "$PKG" <<'PY'
import sys
hdr = open(sys.argv[1], "rb").read(16)
assert hdr[:4] == b"\x7fCNT", f"Invalid PS4 PKG magic: {hdr[:4]!r}"
print("All-in-One PS4 PKG magic verified: 0x7F 'C' 'N' 'T' (PlayStation 4 Content Package)")
PY
    echo "=== Running Full PKG Hash & Signature Validation ==="
    "$PKGTOOL" pkg_validate "$PKG"
} 2>&1 | tee "$LOG"
