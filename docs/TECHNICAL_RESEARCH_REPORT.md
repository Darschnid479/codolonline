# Project Dragon — Technical Research Report
**Target Game:** Call of Duty Online (Chinese Retail / Offline Build `1.1.1.13`)
**Target Platform:** Sony PlayStation 4 (Native x86-64 OrbisOS, AMD Jaguar CPU + AMD Liverpool GCN GPU)
**Author:** Project Dragon Native Engine Team

---

## 1. Executive Summary

Project Dragon is a **100% native PlayStation 4 (x86-64 OrbisOS)** port of Call of Duty Online (`1.1.1.13`). No Wine, Proton, Linux guest runtime, Windows API translation layer, or streaming is used. The engine compiles directly to a signed PlayStation 4 `eboot.bin` (FSELF) packaged into a standalone `.pkg` installer, reading the user's own lawfully supplied Call of Duty Online `1.1.1.13` game files at runtime from `/data/codol` or `/usb0/codol` without redistributing any proprietary game content or hardcoded cryptographic keys.

Empirical analysis of the supplied `1.1.1.13` installation confirmed:
1. The original Windows client (`codoMP_client_shipRetail.exe`) is a **32-bit x86 (i386)** Direct3D 9 / Miles Sound System / Bink2 / TenProtect executable built on Treyarch/Infinity Ward's **IW4-derived "C1" engine** using **FastFile version 334** (`IWff0100` / `IWffu100`).
2. **No legitimate proprietary source code** exists for `codoMP_client_shipRetail.exe`. Public community code exists for the offline Windows patcher/server (`Kernel33.dll` + CoD4x-derived server in `nucleur16/cod-offline-1.1.1.13`, AGPL-3.0) and for clean-room C1 asset readers/tools (`nucleur16/sh1xo-radiant`, MIT).
3. Static recompilation of a VMProtect-packed 32-bit x86 D3D9/Win32 binary to 64-bit OrbisOS without a Windows compatibility shim is architecturally brittle and violates native PS4 goals. Therefore, Project Dragon uses a **clean-room native C++20 engine reconstruction** paired with a **zero-Windows-dependency portable FastFile v334 / IWD / IWI / Wavelet asset pipeline** derived from MIT-licensed `sh1xo-radiant` layouts.

---

## 2. Phase 1 — Empirical Original Game Analysis (`1.1.1.13`)

All data below was collected directly from the supplied offline `1.1.1.13` installation using [inspect_game.py](file:///home/antigravity/project-dragon/tools/dragon_pe_inspect/inspect_game.py) (full machine-readable output archived in `logs/phase1_game_analysis.json`).

### 2.1 Executable & DLL Inventory

Every binary in the original Windows installation is **32-bit x86 (i386)** Windows PE:

| Binary | Size (Bytes) | Arch | Exports | Key Imports / Observed Role |
|---|---:|---|---:|---|
| `codoMP_client_shipRetail.exe` | 20,918,752 | x86 (32-bit) | 21 | Packed with 8 `nsn` sections + `.vmp0` (VMProtect). Offline pack patches its IAT to import **only** `Kernel33.dll`. Embeds RSA-2048 key, wrapped IWD AES-192 key, and Wavelet Huffman tables. |
| `Kernel33.dll` | 19,222,528 | x86 (32-bit) | 67 | Offline bootstrapper (`nucleur16/cod-offline-1.1.1.13`). Embeds `unpacked.z` (~10.18 MiB `.rsrc`), bypasses TenProtect, emulates DemonWare lobby/stats, hosts embedded CoD4x-derived match server. |
| `D3DX9_43.dll` | 1,998,168 | x86 (32-bit) | 329 | Direct3D 9 shader/math helper library (June 2010 DirectX SDK). |
| `mss32.dll` | 450,560 | x86 (32-bit) | 394 | RAD Game Tools Miles Sound System core (`USER32`, `KERNEL32`, `WINMM`). Accompanied by 6 plugins in `miles/`: `milesEq.flt`, `mssds3d.flt`, `mssdsp.flt`, `msseax.flt`, `mssmp3.asi`, `mssvoice.asi`. |
| `bink2w32.dll` | 340,992 | x86 (32-bit) | 81 | RAD Game Tools Bink 2 video decoder (used for 25 `.bk2` movies in `main/video/`, e.g. `CODO_logo.bk2`, `PUB_logo.bk2`, `coop_*_load.bk2`). |
| `TenProxy.dll` | 163,480 | x86 (32-bit) | 1 | Tencent client launcher / anti-cheat proxy (obsolete offline; not needed on PS4). |
| `TenSLX.dll` | 2,547,168 | x86 (32-bit) | 2 | Tencent anti-cheat module containing `.tvm0` virtualized code section (not needed on PS4). |
| `TerSafe.dll` | 3,255,712 | x86 (32-bit) | 8 | Tencent anti-cheat kernel/user-mode protection module (not needed on PS4). |

### 2.2 Runtime Cryptographic & Compression Material in `codoMP_client_shipRetail.exe`

Project Dragon **never** embeds or redistributes proprietary keys. At startup on PS4 or PC, `AssetManager::mount()` reads these structures directly from the user's supplied `codoMP_client_shipRetail.exe`:

| Artifact | Verified File Offset | Size | Format & Usage |
|---|---:|---:|---|
| FastFile RSA-2048 Private Key | `0x83C248` | ~1,192 B (ASN.1 DER) | PKCS#1 `RSAPrivateKey` (`SEQUENCE` of 9 integers: `n, e, d, p, q, dp, dq, qinv`). Used via RSA-OAEP(SHA-256) to unwrap per-FastFile AES-192-CTR session keys. |
| Wrapped IWD AES-192 Key Blob | `0x838B20` | 256 B | RSA-2048-OAEP(SHA-256) encrypted payload yielding the 24-byte AES-192 key + 16-byte nonce prefix for encrypted `.iwd` archives. |
| Wavelet Huffman Tables (3 planes) | `0x7F5E60`, `0x7F9E60`, `0x7FDE60` | 3 × 16,384 B (48 KiB) | Static 16-bit symbol/length lookup tables used to decode Tencent/CODOL proprietary Wavelet-compressed `.iwi` textures (`IWi` format `0x1C` / `0x1D`). |

### 2.3 Asset Corpus Inventory (`1.1.1.13`)

- **FastFiles (`.ff`)**: **195 archives** totaling **2,996,674,823 bytes (~2.79 GiB)**:
  - **193** archives are **FastFile version 334** (`89` signed `IWff0100`, `104` unsigned `IWffu100`), plus `2` legacy version `4` files.
  - **76 playable maps** (`mp_*`, `zm_*`, `coop_*`, excluding `*_load.ff` helper zones), spanning Multiplayer (`mp_killhouse_sh`, `mp_nuked`, `mp_rust`, `mp_crash_tropical`, `mp_highrise`, `mp_terminal_cls`, etc.), Cyborg/Zombies (`zm_factory_sh`, `zm_theater_sh`, `zm_asylum_sh`, `zm_prototype_sh`, `zm_sumpf_sh`, `zm_zombie_training`), and Spec-Ops/Co-op (`coop_*`).
- **IWD Archives (`.iwd`)**: **77 archives** totaling **6,919,453,344 bytes (~6.44 GiB)** under `main/`:
  - **62 encrypted image archives** containing **42,221** streamed `.iwi` texture entries (`iw_image_00.iwd` .. `iw_image_61.iwd` + `iw_image_16.iwd`), plus sound, script (`sh1x_svr_scripts.iwd`), model, and config archives.
- **Videos**: **25** Bink 2 (`.bk2`) cinematics in `main/video/`.

### 2.4 Binary Asset Formats (C1 / IW4-Derived Engine)

1. **FastFile v334 (`IWff0100` / `IWffu100`)**:
   - Header: 8-byte magic + `uint32` version (`334`).
   - Signed (`IWff0100`): Followed by a 256-byte RSA-2048-OAEP(SHA-256) encrypted block yielding a 24-byte AES-192 key + 16-byte IV. Payload is encrypted with **AES-192-CTR using little-endian 128-bit counter increment**, then **zlib** inflated into the 10-block virtual memory zone stream (`XFile`).
   - Unsigned (`IWffu100`): Plaintext zlib-compressed `XFile` stream.
   - Zone Stream Layout: 10 typed memory blocks (`TEMP`, `PHYSICAL`, `RUNTIME`, `VIRTUAL`, `LARGE`, `CALLBACK`, `VERTEX`, `INDEX`, `SCRIPT`, etc.) with 32-bit block-encoded pointer fixups (`0xFFFFFFFF` / `0xFFFFFFFE` inline markers), script-string table, and `XAssetList` spanning all 42+ C1 asset types (`GfxWorld`, `clipMap_t`, `MapEnts`, `Material`, `GfxImage`, `XModel`, `XAnimParts`, `WeaponCompleteDef`, `RawFile`, `ComWorld`, `FxEffectDef`, `SoundAliasList`, `MenuList`, etc.).
2. **Encrypted `.iwd` Archives**:
   - Standard ZIP container whose entry payloads (when flagged encrypted) are encrypted with **AES-192-CTR using big-endian 128-bit counter increment** keyed from the unwrapped `0x838B20` key blob and per-entry salt, followed by standard Deflate / Stored decompression and ZIP CRC-32 verification.
3. **Textures (`.iwi` v8/v9/v10 & Embedded `GfxImage`)**:
   - Supports DXT1, DXT3, DXT5, uncompressed ARGB8/RGB8/A8/L8/A8L8, and CODOL's proprietary **Wavelet-compressed IWI** format decoded via integer CDF 5/3 inverse wavelet lifting + Huffman tables extracted from the executable.
4. **World Geometry & Collision (`GfxWorld` + `clipMap_t` + `MapEnts`)**:
   - `GfxWorld`: Packed 44-byte `GfxWorldVertex` (position, normal, BGRA vertex color, diffuse UV, lightmap UV), 16-bit triangle index buffers (`GfxSurface`), embedded lightmap atlases (`GfxLightmapArray`), AABB visibility tree (`GfxAabbTree`), portal cells (`GfxCell`), and static model placements (`GfxStaticModelDrawInst` referencing `XModel` LODs).
   - `clipMap_t`: Convex `cbrush_t` axial + arbitrary bevel planes, `cLeaf_t`, `cmodel_t` brush submodels, and static model collision bounds.
   - `MapEnts`: Plaintext Quake/IW entity lump (`classname`, `origin`, `angles`) encoding `mp_tdm_spawn*`, `mp_dm_spawn`, `info_player_start`, `mp_global_intermission`, `node_pathnode` AI navigation graphs, triggers, and `worldspawn` lighting parameters.

---

## 3. Source Code Availability & Porting Strategy Evaluation

### 3.1 Investigated Public Repositories & Licenses

| Repository | License | Findings & Reuse Decision |
|---|---|---|
| `OpenOrbis/OpenOrbis-PS4-Toolchain` (v0.5.4) | GPL-3.0 (toolchain scripts) / BSD-like headers & crt | Official open-source PS4 LLVM-18 cross-compiler (`x86_64-pc-freebsd12-elf`), OrbisOS `.so` stubs, `create-fself`. **Used as primary PS4 build toolchain.** |
| `OpenOrbis/LibOrbisPkg` | GPL-3.0 | Native `.NET` PS4 `.pkg` / `.sfo` / `.gp4` builder. Recompiled natively on `.NET 10` (bypassing Ubuntu 24.04 OpenSSL 1.1 breakage in stock .NET 3.1 binary). **Used as PS4 PKG packager.** |
| `nucleur16/sh1xo-radiant` | MIT | Complete, verified FastFile v334 (`codol_zone`), IWD/IWI/Wavelet (`codol_render`), and C1 struct layouts. **Adapted into `third_party/codol_zone_portable/` with all Windows CryptoAPI/Win32 calls replaced by zero-dependency portable C++20 (FIPS 180-4 SHA-256, FIPS 197 AES-192-CTR LE/BE, CRT RSA-2048-OAEP, POSIX I/O).** |
| `iHaiDeeZ/ps4-kit` | MIT | Production-proven PS4 homebrew fixes: patched `link.x` preserving `.init_array.*`/`.tdata.*`/`.tbss.*`/`.data.*`, `check-oelf.cmake` verifying exactly 1 RW `PT_LOAD` segment (preventing OrbisOS `CE-41839-5`), musl `std::filesystem` `*at` pitfalls, and a Mesa 26.0.8 + RADV Vulkan backend (`gpu/build-mesa.sh`, `ps4_vk_wsi.*`). **Linker script fixes and OELF verifier directly adopted; Mesa/RADV Vulkan mapped as the zero-`.sprx` GPU upgrade path.** |
| `nucleur16/cod-offline-1.1.1.13` | AGPL-3.0 | Windows x86 `Kernel33.dll` + CoD4x-derived server (`third_party/sh1x/src`). **Used strictly as read-only architectural reference** for offline match flow, bot waypoints, and DemonWare elimination (no AGPL code copied into Project Dragon). |
| `CodOnline-Disected` | Public research | Read-only reference corroborating C1 asset structures. |

### 3.2 Static Recompilation vs. Clean-Room Native Reconstruction

| Criterion | Static Recompilation of `codoMP_client_shipRetail.exe` | Clean-Room Native C++20 Reconstruction (Project Dragon) |
|---|---|---|
| Architecture Match | 32-bit x86 (4-byte pointers, x87/SSE2, stdcall/thiscall) → 64-bit OrbisOS x86-64 (SysV ABI). Requires full 32-bit virtual address sandbox & thunking. | Native 64-bit x86-64 OrbisOS (`-march=btver2` tuned for PS4 AMD Jaguar). |
| Anti-Cheat / Packing | VMProtect `.vmp0` + custom `nsn` packer + TenProtect hooks obscure CFG and indirect jump tables. | Zero reliance on packed Win32 code; reads clean original assets directly. |
| Graphics / Audio / OS | Hardwired to Direct3D 9 (`IDirect3DDevice9`), Miles (`mss32.dll`), Bink2, Win32 threads/TLS. Recompiling forces a D3D9→PS4 translation layer (bordering on Win32 emulation). | Direct native PS4 APIs: `libSceVideoOut`, `libScePigletv2VSH` (AMD GCN GLES2) / future `ps4-kit` RADV Vulkan, `libScePad`, `libSceAudioOut`, `libkernel` direct memory & pthreads. |
| Verdict | **Rejected** — infeasible without quasi-emulation and unmaintainable. | **Selected** — genuine native PS4 engine reading 100% original 1.1.1.13 assets. |

---

## 4. Native PS4 Architecture & Hardware Lessons Applied

### 4.1 Critical PS4 Toolchain / ABI Fixes Integrated
1. **Patched OpenOrbis Linker Script (`cmake/ps4-link.x`)**: Adopted `ps4-kit` patches adding `KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))`, `.tdata.*`, `.tbss.*`, and `.data.*` so C++20 static constructors and thread-local storage initialize reliably on OrbisOS.
2. **Single RW `PT_LOAD` Verification (`cmake/check-oelf.cmake`)**: Automatically runs after every `create-fself` conversion, confirming exactly 1 RW `PT_LOAD` segment so OrbisOS never fails launch with `CE-41839-5`.
3. **POSIX Filesystem Discipline**: Avoids `std::filesystem::directory_iterator` (which calls unimplemented `*at` syscalls in OpenOrbis musl) across both `engine/` and `third_party/codol_zone_portable/`, using `fopen`/`fseeko`/`ftello`/`opendir`/`readdir`/`stat` everywhere.
4. **OpenOrbis `signal.h` Workaround**: Handles OpenOrbis header macro discrepancy (`__sa_handler.__sa_sigaction`) in `engine/platform/crash_log.cpp` to capture `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGABRT`/`SIGFPE` crash traces to `/data/project_dragon/crash.log`.

### 4.2 Dual-Path Native PS4 Graphics Architecture (`engine/gfx/ps4_renderer.cpp`)
1. **Path A — Hardware AMD Liverpool GCN GPU (Dynamic Piglet GLES2 + Shacc)**: Dynamically loads `libScePigletv2VSH.sprx` and `libSceShaccVSH.sprx` at runtime via `sceKernelLoadStartModule` + `sceKernelDlsym` (searching `/data/project_dragon/modules/`, `/app0/sce_module/`, and system common lib). Uploads decoded `GfxWorld` VBOs, 16-bit re-based surface IBOs, and decoded `.iwi` diffuse + lightmap textures to VRAM and shades them with GLSL ES 2.0 shaders (alpha cutout, sRGB-lifted HDR lightmap modulation, sun diffuse, distance fog).
2. **Path B — Guaranteed Native Direct-Memory VideoOut Fallback**: Because `libScePigletv2VSH.sprx` / `libSceShaccVSH.sprx` are copyrighted Sony system/game modules that cannot be legally redistributed inside the `.pkg`, if the user has not placed them on their PS4, Project Dragon automatically falls back **without crashing** to native double-buffered 1920×1080 `libSceVideoOut` (`ORBIS_VIDEO_OUT_PIXEL_FORMAT_B8_G8_R8_A8_SRGB`) backed by `sceKernelAllocateDirectMemory` + our 4-thread band-parallel perspective-correct 3D textured rasterizer (`SoftwareRasterizer`).
3. **Planned Path C — Zero-`.sprx` Native Vulkan GPU via `ps4-kit` Mesa/RADV**: Long-term upgrade integrating `ps4-kit`'s 32 PS4 patches on Mesa 26.0.8 RADV + `ps4_vk_wsi` directly into `eboot.bin` for full hardware AMD GCN acceleration with zero external `.sprx` dependencies.

---

## 5. Verification Evidence Summary

| Verification Step | Evidence / Result |
|---|---|
| **Portable Crypto & Engine Unit + Integration Tests (`dragon_tests`)** | **26 / 26 PASSED, 0 FAILED** — Verified FIPS 180-4 SHA-256, FIPS 197 C.2 AES-192 ECB, AES-192-CTR LE/BE, ZIP CRC-32, ClipMap `CM_RayTrace`/`CM_BoxTrace`, `PM_Move` wall slide, `MapEnts` parser, clean-room GSC VM (`thread`/`wait`/`waittill`/`notify`), and real `1.1.1.13` FastFile v334 + IWD + GfxWorld + ClipMap + MapEnts + 3D rendering. |
| **Multi-Map Real CODOL `1.1.1.13` Visual Renders (1280×720)** | Visually verified rendered frames of authentic CODOL maps: `mp_killhouse_sh` (spawn + intermission), `mp_nuked` (Nuketown), `mp_crash_tropical` (CODOL Tropical Crash), and `zm_factory_sh` (Cyborg Zombies Der Riese with decoded `thundergun_zm`), confirming 100% textured geometry, lightmaps, static props, and HUD. |
| **Automated Asset Validator (`dragon_validate --all`)** | **64 / 64 (100.0%) standalone playable maps PASSED** (`58 / 58` Multiplayer + `6 / 6` Cyborg/Zombies maps decrypted, inflated, traversed, and decoded `GfxWorld`, `ClipMap`, `MapEnts`, lightmaps, and `64/64` sampled `.iwi` textures). All `9` `coop_*` FastFiles also decrypted cleanly (2,815–5,734 assets each) and were empirically confirmed to be mission overlay zones referencing their base `mp_*` FastFile for `GfxWorld`; `3` non-world helper zones (`mp_playlists`, `mp_weapons`, `mp_weapons_postgfx_mp`) were excluded from the map menu. Full per-map table in [COMPATIBILITY_REPORT.md](file:///home/antigravity/project-dragon/docs/COMPATIBILITY_REPORT.md). |
| **Native PS4 ELF / OELF / FSELF (`build-ps4/eboot.bin`)** | Compiled cleanly with OpenOrbis LLVM-18 (`clang++-18 --target=x86_64-pc-freebsd12-elf -march=btver2`), verified FreeBSD ELF64 `fe10` with **1 RW `PT_LOAD` segment** (`check-oelf.cmake`), and verified FSELF header magic `0x1D3D154F` (`2.7 MiB`). |
| **Standalone PS4 PKG (`dist/IV0000-DRGN00113_00-PROJECTDRAGON000.pkg`)** | Packaged cleanly via `.NET 10` `LibOrbisPkg` (`6.4 MiB`), verified `\x7fCNT` magic, `PARAM_SFO` (`TITLE_ID=DRGN00113`), `ICON0_PNG`, PlayGo digests, and inner/outer PFS layout. |
| **Physical PS4 / Emulator Execution** | **NOT YET EXECUTED** (no physical jailbroken PS4 or shadPS4 runtime exists in this build environment). All PS4-specific runtime modules are strictly labeled `COMPILED` pending hardware/emulator testing. |
