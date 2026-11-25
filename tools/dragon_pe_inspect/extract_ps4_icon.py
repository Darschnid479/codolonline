#!/usr/bin/env python3
"""
Extract the authentic Windows application icon from Call of Duty Online 1.1.1.13
(codoMP_client_shipRetail.exe, PE resource RT_GROUP_ICON ID 1 -> highest-resolution
32-bit RT_ICON, ID 5: 64x64 BGRA with full alpha transparency) and generate a
proportion-preserving, transparency-preserving 512x512 RGBA PNG for the local PS4
package tile (sce_sys/icon0.png).

Zero external dependencies (Python 3 standard library only).
Never redistribute extracted proprietary assets; generate icon0.png locally during packaging.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import struct
import sys
import zlib
from pathlib import Path
from typing import Dict, List, Optional, Tuple


RT_ICON = 3
RT_GROUP_ICON = 14


def parse_pe_icon_resources(pe_bytes: bytes) -> Tuple[List[dict], Dict[int, Tuple[int, bytes]]]:
    """Parse PE32/PE32+ resource directory for RT_GROUP_ICON (14) and RT_ICON (3)."""
    if len(pe_bytes) < 0x40 or pe_bytes[:2] != b"MZ":
        raise ValueError("Input file is not a valid PE executable (missing MZ header)")
    e_lfanew = struct.unpack_from("<I", pe_bytes, 0x3C)[0]
    if pe_bytes[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("Input file is not a valid PE executable (missing PE signature)")

    num_sections = struct.unpack_from("<H", pe_bytes, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", pe_bytes, e_lfanew + 20)[0]
    opt_off = e_lfanew + 24
    opt_magic = struct.unpack_from("<H", pe_bytes, opt_off)[0]
    if opt_magic == 0x10B:  # PE32
        dd_off = opt_off + 96
    elif opt_magic == 0x20B:  # PE32+
        dd_off = opt_off + 112
    else:
        raise ValueError(f"Unsupported PE optional header magic: 0x{opt_magic:x}")

    rsrc_rva, rsrc_size = struct.unpack_from("<II", pe_bytes, dd_off + 2 * 8)
    if rsrc_rva == 0 or rsrc_size == 0:
        raise ValueError("PE executable has no resource directory (.rsrc)")

    sec_off = opt_off + opt_size
    sections: List[Tuple[int, int, int, int]] = []
    for i in range(num_sections):
        s = sec_off + i * 40
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", pe_bytes, s + 8)
        sections.append((va, vsize, rptr, rsize))

    def rva_to_off(rva: int) -> int:
        for va, vsize, rptr, rsize in sections:
            span = max(vsize, rsize)
            if va <= rva < va + span:
                return rptr + (rva - va)
        raise ValueError(f"RVA 0x{rva:x} not mapped in any PE section")

    rsrc_base = rva_to_off(rsrc_rva)
    entries: List[Tuple[List[int], int, int]] = []

    def walk(dir_rel: int, path: List[int]) -> None:
        pos = rsrc_base + dir_rel
        _, _, _, _, n_named, n_id = struct.unpack_from("<IIHHHH", pe_bytes, pos)
        for i in range(n_named + n_id):
            ent_pos = pos + 16 + i * 8
            name_or_id, off_to_data = struct.unpack_from("<II", pe_bytes, ent_pos)
            key = -(name_or_id & 0x7FFFFFFF) if (name_or_id & 0x80000000) else name_or_id
            if off_to_data & 0x80000000:
                walk(off_to_data & 0x7FFFFFFF, path + [key])
            else:
                dpos = rsrc_base + off_to_data
                data_rva, size, _, _ = struct.unpack_from("<IIII", pe_bytes, dpos)
                doff = rva_to_off(data_rva)
                entries.append((path + [key], doff, size))

    walk(0, [])

    groups: List[dict] = []
    icons: Dict[int, Tuple[int, bytes]] = {}
    for path, doff, size in entries:
        if len(path) < 3:
            continue
        rtype, rid, rlang = path[0], path[1], path[2]
        blob = pe_bytes[doff : doff + size]
        if rtype == RT_ICON:
            icons[rid] = (doff, blob)
        elif rtype == RT_GROUP_ICON:
            _, itype, count = struct.unpack_from("<HHH", blob, 0)
            members = []
            for j in range(count):
                bw, bh, bcc, _, planes, bpp, nbytes, nid = struct.unpack_from(
                    "<BBBBHHIH", blob, 6 + j * 14
                )
                members.append(
                    {
                        "id": nid,
                        "width": bw or 256,
                        "height": bh or 256,
                        "bpp": bpp,
                        "bytes": nbytes,
                    }
                )
            groups.append(
                {
                    "group_id": rid,
                    "lang": rlang,
                    "offset": doff,
                    "size": size,
                    "type": itype,
                    "members": members,
                }
            )
    return groups, icons


def decode_rt_icon_to_rgba(blob: bytes) -> Tuple[int, int, int, bytes]:
    """Decode an RT_ICON resource blob (PNG or 32-bit/24-bit DIB) into RGBA8."""
    if blob[:8] == b"\x89PNG\r\n\x1a\n":
        raise ValueError("Embedded PNG icon encountered; expected 32-bit DIB in CODOL 1.1.1.13")

    if len(blob) < 40:
        raise ValueError("RT_ICON blob too small for BITMAPINFOHEADER")

    hdr_sz, w, h2, planes, bpp, comp = struct.unpack_from("<IiiHHI", blob, 0)
    if planes != 1 or comp != 0:
        raise ValueError(f"Unsupported DIB icon format (planes={planes}, comp={comp})")
    h = abs(h2) // 2
    if w <= 0 or h <= 0:
        raise ValueError(f"Invalid DIB icon dimensions: {w}x{h}")

    if bpp != 32:
        raise ValueError(f"Expected 32-bit BGRA icon, got {bpp} bpp")

    px_off = hdr_sz
    xor_size = w * h * 4
    xor_data = blob[px_off : px_off + xor_size]
    and_off = px_off + xor_size
    and_stride = ((w + 31) // 32) * 4
    and_data = blob[and_off : and_off + and_stride * h]

    has_alpha_channel = any(xor_data[i] != 0 for i in range(3, len(xor_data), 4))
    rgba = bytearray(w * h * 4)
    for y in range(h):
        src_y = (h - 1 - y) if h2 > 0 else y
        for x in range(w):
            si = (src_y * w + x) * 4
            di = (y * w + x) * 4
            b, g, r, a = xor_data[si : si + 4]
            if not has_alpha_channel and len(and_data) >= (src_y + 1) * and_stride:
                mask_byte = and_data[src_y * and_stride + (x >> 3)]
                mask_bit = (mask_byte >> (7 - (x & 7))) & 1
                a = 0 if mask_bit else 255
            rgba[di] = r
            rgba[di + 1] = g
            rgba[di + 2] = b
            rgba[di + 3] = a

    return w, h, bpp, bytes(rgba)


def _lanczos3(x: float) -> float:
    ax = abs(x)
    if ax < 1e-8:
        return 1.0
    if ax >= 3.0:
        return 0.0
    pix = math.pi * ax
    return (math.sin(pix) / pix) * (math.sin(pix / 3.0) / (pix / 3.0))


def resize_rgba_premul_lanczos(src_w: int, src_h: int, src_rgba: bytes, dst_w: int, dst_h: int) -> bytes:
    """
    Separable Lanczos-3 resampling in premultiplied RGBA space so transparent edges
    do not bleed dark fringes and exact proportions are preserved.
    """
    # Convert to premultiplied float buffers
    src_f = [0.0] * (src_w * src_h * 4)
    for i in range(0, src_w * src_h * 4, 4):
        a = src_rgba[i + 3] / 255.0
        src_f[i] = (src_rgba[i] / 255.0) * a
        src_f[i + 1] = (src_rgba[i + 1] / 255.0) * a
        src_f[i + 2] = (src_rgba[i + 2] / 255.0) * a
        src_f[i + 3] = a

    # Horizontal pass -> dst_w x src_h
    scale_x = src_w / float(dst_w)
    radius_x = 3.0 * max(1.0, scale_x)
    tmp = [0.0] * (dst_w * src_h * 4)
    x_taps = []
    for dx in range(dst_w):
        center = (dx + 0.5) * scale_x - 0.5
        lo = max(0, int(math.floor(center - radius_x)))
        hi = min(src_w - 1, int(math.ceil(center + radius_x)))
        taps = []
        wsum = 0.0
        inv_s = 1.0 / max(1.0, scale_x)
        for sx in range(lo, hi + 1):
            w = _lanczos3((sx - center) * inv_s)
            if w != 0.0:
                taps.append((sx, w))
                wsum += w
        if wsum != 0.0:
            taps = [(sx, w / wsum) for sx, w in taps]
        x_taps.append(taps)

    for y in range(src_h):
        src_row = y * src_w * 4
        dst_row = y * dst_w * 4
        for dx, taps in enumerate(x_taps):
            r = g = b = a = 0.0
            for sx, w in taps:
                si = src_row + sx * 4
                r += src_f[si] * w
                g += src_f[si + 1] * w
                b += src_f[si + 2] * w
                a += src_f[si + 3] * w
            di = dst_row + dx * 4
            tmp[di] = r
            tmp[di + 1] = g
            tmp[di + 2] = b
            tmp[di + 3] = a

    # Vertical pass -> dst_w x dst_h
    scale_y = src_h / float(dst_h)
    radius_y = 3.0 * max(1.0, scale_y)
    y_taps = []
    for dy in range(dst_h):
        center = (dy + 0.5) * scale_y - 0.5
        lo = max(0, int(math.floor(center - radius_y)))
        hi = min(src_h - 1, int(math.ceil(center + radius_y)))
        taps = []
        wsum = 0.0
        inv_s = 1.0 / max(1.0, scale_y)
        for sy in range(lo, hi + 1):
            w = _lanczos3((sy - center) * inv_s)
            if w != 0.0:
                taps.append((sy, w))
                wsum += w
        if wsum != 0.0:
            taps = [(sy, w / wsum) for sy, w in taps]
        y_taps.append(taps)

    out = bytearray(dst_w * dst_h * 4)
    for dy, taps in enumerate(y_taps):
        out_row = dy * dst_w * 4
        for dx in range(dst_w):
            r = g = b = a = 0.0
            for sy, w in taps:
                ti = (sy * dst_w + dx) * 4
                r += tmp[ti] * w
                g += tmp[ti + 1] * w
                b += tmp[ti + 2] * w
                a += tmp[ti + 3] * w
            a_clamped = max(0.0, min(1.0, a))
            di = out_row + dx * 4
            if a_clamped <= 1e-4:
                out[di] = out[di + 1] = out[di + 2] = out[di + 3] = 0
            else:
                # Un-premultiply cleanly
                ur = max(0.0, min(1.0, r / a_clamped))
                ug = max(0.0, min(1.0, g / a_clamped))
                ub = max(0.0, min(1.0, b / a_clamped))
                out[di] = int(round(ur * 255.0))
                out[di + 1] = int(round(ug * 255.0))
                out[di + 2] = int(round(ub * 255.0))
                out[di + 3] = int(round(a_clamped * 255.0))
    return bytes(out)


def place_in_square_canvas(
    src_w: int, src_h: int, src_rgba: bytes, canvas_size: int
) -> bytes:
    """Scale preserving exact aspect ratio and center on a transparent canvas_size x canvas_size canvas."""
    scale = min(canvas_size / float(src_w), canvas_size / float(src_h))
    fit_w = max(1, int(round(src_w * scale)))
    fit_h = max(1, int(round(src_h * scale)))
    scaled = (
        src_rgba
        if (fit_w == src_w and fit_h == src_h)
        else resize_rgba_premul_lanczos(src_w, src_h, src_rgba, fit_w, fit_h)
    )
    if fit_w == canvas_size and fit_h == canvas_size:
        return scaled
    canvas = bytearray(canvas_size * canvas_size * 4)  # fully transparent
    ox = (canvas_size - fit_w) // 2
    oy = (canvas_size - fit_h) // 2
    for y in range(fit_h):
        s0 = y * fit_w * 4
        d0 = ((oy + y) * canvas_size + ox) * 4
        canvas[d0 : d0 + fit_w * 4] = scaled[s0 : s0 + fit_w * 4]
    return bytes(canvas)


def write_png_rgba(path: Path, width: int, height: int, rgba: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # Filter type 0 (None)
        raw.extend(rgba[y * stride : (y + 1) * stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Extract original CODOL 1.1.1.13 application icon to PS4 icon0.png (512x512 RGBA)"
    )
    ap.add_argument("exe_path", help="Path to user's codoMP_client_shipRetail.exe")
    ap.add_argument("out_png", help="Output path for PS4 512x512 RGBA icon0.png")
    ap.add_argument("--size", type=int, default=512, help="Output square size in pixels (default: 512)")
    ap.add_argument("--raw-out", help="Optional path to also write the unmodified raw extracted icon PNG")
    args = ap.parse_args()

    exe_path = Path(args.exe_path)
    if not exe_path.is_file():
        print(f"ERROR: Original client executable not found: {exe_path}", file=sys.stderr)
        print("Cannot build authentic PS4 icon0.png without user-supplied codoMP_client_shipRetail.exe.", file=sys.stderr)
        return 1

    pe_bytes = exe_path.read_bytes()
    groups, icons = parse_pe_icon_resources(pe_bytes)
    if not groups:
        print(f"ERROR: No RT_GROUP_ICON resource found in {exe_path}", file=sys.stderr)
        return 1

    # Select RT_GROUP_ICON ID 1 (used by LoadIconA(hInstance, MAKEINTRESOURCE(1)))
    group = next((g for g in groups if g["group_id"] == 1), groups[0])
    # Choose highest resolution, highest bit-depth member
    best = max(group["members"], key=lambda m: (m["width"] * m["height"], m["bpp"]))
    rid = best["id"]
    if rid not in icons:
        print(f"ERROR: RT_ICON ID {rid} referenced by RT_GROUP_ICON ID {group['group_id']} is missing", file=sys.stderr)
        return 1

    icon_offset, icon_blob = icons[rid]
    w, h, bpp, rgba = decode_rt_icon_to_rgba(icon_blob)

    transparent_px = sum(1 for a in rgba[3::4] if a < 255)
    fully_trans_px = sum(1 for a in rgba[3::4] if a == 0)

    if args.raw_out:
        write_png_rgba(Path(args.raw_out), w, h, rgba)

    canvas = place_in_square_canvas(w, h, rgba, args.size)
    out_path = Path(args.out_png)
    write_png_rgba(out_path, args.size, args.size, canvas)

    exe_sha = hashlib.sha256(pe_bytes).hexdigest()[:16]
    out_trans = sum(1 for a in canvas[3::4] if a < 255)
    print(f"[ICON] Source file   : {exe_path.name} (sha256:{exe_sha}...)")
    print(f"[ICON] PE resource   : RT_GROUP_ICON ID {group['group_id']} (lang={group['lang']}, off=0x{group['offset']:x}) -> RT_ICON ID {rid} (off=0x{icon_offset:x}, {len(icon_blob)} B)")
    print(f"[ICON] Original icon : {w}x{h} @ {bpp}bpp RGBA (transparent px={transparent_px}/{w*h}, alpha==0 px={fully_trans_px})")
    print(f"[ICON] PS4 tile PNG  : {out_path} ({args.size}x{args.size} 32-bit RGBA, aspect preserved 1:1, transparent px={out_trans}/{args.size*args.size})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
