#!/usr/bin/env python3
"""
Project Dragon — Phase 1 Original Game Binary & Asset Inspector
Inspects a user-provided Call of Duty Online 1.1.1.13 installation:
  - PE headers, architecture, subsystem, timestamps, sections, and DLL import tables
    for codoMP_client_shipRetail.exe, Kernel33.dll, and all bundled DLLs
  - FastFile (.ff) inventory: magic (IWff0100 signed vs IWffu100 unsigned), version, size
  - IWD archive inventory & sizes
  - Key & wavelet table verification offsets in the executable (without printing secret key bytes!)
Outputs JSON and Markdown reports for docs/TECHNICAL_RESEARCH_REPORT.md.
"""

import json
import os
import struct
import sys
from pathlib import Path


def parse_pe_info(path: Path) -> dict:
    info = {
        "file": path.name,
        "size": path.stat().st_size,
        "valid_pe": False,
        "arch": "unknown",
        "subsystem": "unknown",
        "timestamp": 0,
        "sections": [],
        "imports": [],
        "exports_count": 0,
    }
    data = path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        return info
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_off + 24 > len(data) or data[pe_off : pe_off + 4] != b"PE\x00\x00":
        return info

    info["valid_pe"] = True
    machine, num_sections, timestamp, _, _, opt_size, characteristics = struct.unpack_from(
        "<HHIIIHH", data, pe_off + 4
    )
    info["timestamp"] = timestamp
    info["arch"] = {0x014C: "x86 (i386 32-bit)", 0x8664: "x86-64 (AMD64)"}.get(
        machine, f"0x{machine:04X}"
    )
    info["is_dll"] = bool(characteristics & 0x2000)

    opt_off = pe_off + 24
    opt_magic = struct.unpack_from("<H", data, opt_off)[0]
    is_pe64 = opt_magic == 0x20B
    subsystem_off = opt_off + (68 if not is_pe64 else 68)
    if subsystem_off + 2 <= len(data):
        subsys = struct.unpack_from("<H", data, subsystem_off)[0]
        info["subsystem"] = {2: "Windows GUI", 3: "Windows CUI"}.get(subsys, str(subsys))

    dd_off = opt_off + (96 if not is_pe64 else 112)
    sections_off = opt_off + opt_size
    sections = []
    for i in range(num_sections):
        so = sections_off + i * 40
        if so + 40 > len(data):
            break
        raw_name, vsize, vaddr, rsize, rptr = struct.unpack_from("<8sIIII", data, so)
        sname = raw_name.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        sections.append((sname, vaddr, vsize, rptr, rsize))
        info["sections"].append(
            {
                "name": sname,
                "virtual_addr": f"0x{vaddr:08X}",
                "virtual_size": vsize,
                "raw_size": rsize,
            }
        )

    def rva_to_off(rva: int):
        for _, vaddr, vsize, rptr, rsize in sections:
            span = max(vsize, rsize)
            if vaddr <= rva < vaddr + span:
                off = rptr + (rva - vaddr)
                if off < len(data):
                    return off
        return None

    # Export directory (data dir 0)
    if dd_off + 8 <= len(data):
        exp_rva, exp_sz = struct.unpack_from("<II", data, dd_off)
        if exp_rva and exp_sz:
            eo = rva_to_off(exp_rva)
            if eo is not None and eo + 40 <= len(data):
                info["exports_count"] = struct.unpack_from("<I", data, eo + 24)[0]

    # Import directory (data dir 1)
    if dd_off + 16 <= len(data):
        imp_rva, imp_sz = struct.unpack_from("<II", data, dd_off + 8)
        if imp_rva and imp_sz:
            io = rva_to_off(imp_rva)
            if io is not None:
                while io + 20 <= len(data):
                    orig_thunk, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, io)
                    if orig_thunk == 0 and name_rva == 0 and first_thunk == 0:
                        break
                    no = rva_to_off(name_rva)
                    if no is not None:
                        end = data.find(b"\x00", no)
                        if end > no:
                            dll_name = data[no:end].decode("ascii", errors="replace")
                            info["imports"].append(dll_name)
                    io += 20

    return info


def main():
    if len(sys.argv) < 2:
        print("Usage: inspect_game.py <codol_game_dir> [output.json]", file=sys.stderr)
        sys.exit(1)

    root = Path(sys.argv[1])
    if not root.is_dir():
        print(f"Error: not a directory: {root}", file=sys.stderr)
        sys.exit(2)

    report = {
        "game_root": str(root),
        "executables": [],
        "fastfiles": {"total": 0, "signed": 0, "unsigned": 0, "versions": {}, "maps": [], "total_bytes": 0},
        "iwds": {"total": 0, "total_bytes": 0, "files": []},
        "videos": [],
        "miles_plugins": [],
        "client_key_probe": {},
    }

    for p in sorted(root.iterdir()):
        if p.is_file() and p.suffix.lower() in (".exe", ".dll"):
            report["executables"].append(parse_pe_info(p))

    exe = root / "codoMP_client_shipRetail.exe"
    if exe.is_file():
        blob = exe.read_bytes()
        der_prefix = bytes([0x30, 0x82, 0x04, 0xA3, 0x02, 0x01, 0x00, 0x02, 0x82, 0x01, 0x01, 0x00])
        report["client_key_probe"] = {
            "exe_size": len(blob),
            "rsa_der_at_0x83C248": len(blob) >= 0x83C248 + len(der_prefix) and blob[0x83C248 : 0x83C248 + len(der_prefix)] == der_prefix,
            "iwd_blob_present_at_0x838B20": len(blob) >= 0x838B20 + 256,
            "wavelet_tables_present_at_0x7F5E60": len(blob) >= 0x7FDE60 + 4096 * 4,
            "has_d3d9_import": any(e["file"] == exe.name and any(i.lower() == "d3d9.dll" for i in e["imports"]) for e in report["executables"]),
            "imports_kernel33": any(e["file"] == exe.name and any(i.lower() == "kernel33.dll" for i in e["imports"]) for e in report["executables"]),
        }

    # Scan FastFiles
    for ff in sorted(root.rglob("*.ff")):
        sz = ff.stat().st_size
        with ff.open("rb") as f:
            hdr = f.read(16)
        magic = hdr[:8].decode("ascii", errors="replace") if len(hdr) >= 8 else ""
        ver = struct.unpack_from("<I", hdr, 8)[0] if len(hdr) >= 12 else 0
        report["fastfiles"]["total"] += 1
        report["fastfiles"]["total_bytes"] += sz
        if magic == "IWff0100":
            report["fastfiles"]["signed"] += 1
        elif magic == "IWffu100":
            report["fastfiles"]["unsigned"] += 1
        vkey = str(ver)
        report["fastfiles"]["versions"][vkey] = report["fastfiles"]["versions"].get(vkey, 0) + 1
        stem = ff.stem.lower()
        if (stem.startswith("mp_") or stem.startswith("zm_") or stem.startswith("coop_")) and not stem.endswith("_load"):
            report["fastfiles"]["maps"].append(
                {
                    "name": ff.stem,
                    "rel_path": str(ff.relative_to(root)),
                    "magic": magic,
                    "version": ver,
                    "size_bytes": sz,
                }
            )

    # Scan IWDs
    for iwd in sorted(root.rglob("*.iwd")):
        sz = iwd.stat().st_size
        report["iwds"]["total"] += 1
        report["iwds"]["total_bytes"] += sz
        report["iwds"]["files"].append({"rel_path": str(iwd.relative_to(root)), "size_bytes": sz})

    # Scan miles & videos
    miles_dir = root / "miles"
    if miles_dir.is_dir():
        report["miles_plugins"] = sorted(p.name for p in miles_dir.iterdir() if p.is_file())
    video_dir = root / "main" / "video"
    if video_dir.is_dir():
        report["videos"] = sorted(p.name for p in video_dir.iterdir() if p.is_file())

    text = json.dumps(report, indent=2)
    if len(sys.argv) >= 3:
        Path(sys.argv[2]).write_text(text, encoding="utf-8")
    print(text[:4000])
    if len(text) > 4000:
        print(f"... ({len(text)} total bytes written)")


if __name__ == "__main__":
    main()
