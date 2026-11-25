# Project Dragon — Development Roadmap & Status Matrix

**Target:** Native PlayStation 4 (x86-64 OrbisOS) port of Call of Duty Online `1.1.1.13` (Offline)

## Verification Label Definitions (Strictly Enforced)

| Label | Meaning |
|---|---|
| `IMPLEMENTED` | Source code written and functionally verified on the PC Debug host (`build-pc`) against real Call of Duty Online `1.1.1.13` assets and/or automated tests. |
| `COMPILED` | Cross-compiled and linked into the native PS4 x86-64 OrbisOS executable (`build-ps4/eboot.bin` FSELF, verified 1 RW `PT_LOAD` segment) and packaged into `dist/IV0000-DRGN00113_00-PROJECTDRAGON000.pkg`. Not yet run on physical PS4 hardware or emulator. |
| `EMULATOR_TESTED` | Verified executing on a PS4 emulator (e.g., shadPS4). *(None claimed yet — no PS4 emulator installed in this build environment.)* |
| `HARDWARE_TESTED` | Verified executing on physical PlayStation 4 hardware. *(None claimed yet — requires jailbroken PS4 hardware.)* |
| `BLOCKED` | Blocked on an external runtime dependency, missing hardware, or proprietary module constraint. |
| `NOT_IMPLEMENTED` | Planned for a future milestone; not yet implemented. |

---

## Phase 1 — Original Game Analysis

| Workstream | Status | Evidence / Notes |
|---|---|---|
| PE / DLL Binary Architecture Audit (`codoMP_client_shipRetail.exe`, `Kernel33.dll`, `D3DX9_43.dll`, `mss32.dll`, `bink2w32.dll`, `Ten*`) | `IMPLEMENTED` | Verified via `tools/dragon_pe_inspect/inspect_game.py` → `logs/phase1_game_analysis.json` & `docs/TECHNICAL_RESEARCH_REPORT.md`. All binaries confirmed 32-bit x86 (i386). |
| Cryptographic & Wavelet Table Offset Verification | `IMPLEMENTED` | Empirically verified in user's `codoMP_client_shipRetail.exe`: RSA-2048 DER at `0x83C248`, wrapped IWD AES-192 blob at `0x838B20`, Wavelet Huffman tables at `0x7F5E60`/`0x7F9E60`/`0x7FDE60`. |
| FastFile v334 & IWD Corpus Survey | `IMPLEMENTED` | Cataloged 195 FastFiles (193 v334: 89 signed `IWff0100`, 104 unsigned `IWffu100`; 76 playable maps) and 77 `.iwd` archives (62 image archives, 42,221 `.iwi` images). |
| Source Code & Recompilation Feasibility Assessment | `IMPLEMENTED` | Evaluated `sh1xo-radiant` (MIT), `ps4-kit` (MIT), `OpenOrbis`, `LibOrbisPkg`, `cod-offline-1.1.1.13` (AGPL-3.0, read-only reference). Selected clean-room native C++20 engine + portable MIT asset loaders. |

---

## Phase 2 — Native PS4 Foundation

| Subsystem | PS4 Target Status | PC Debug Status | Evidence / Implementation |
|---|---|---|---|
| OpenOrbis LLVM-18 Cross-Toolchain + Patched `ps4-link.x` + `check-oelf.cmake` | `COMPILED` | `IMPLEMENTED` | `cmake/openorbis-ps4-toolchain.cmake`, `cmake/ps4-link.x`, `cmake/check-oelf.cmake`. Verified FreeBSD ELF64 `fe10`, **1 RW `PT_LOAD`**, FSELF magic `0x1D3D154F`. |
| Standalone PS4 `.pkg` Packager (`LibOrbisPkg` on .NET 10) + Local PE Icon Extractor | `COMPILED` | `IMPLEMENTED` | `dist/IV0000-DRGN00113_00-PROJECTDRAGON000.pkg` (`6.9 MiB`), verified `\x7fCNT` magic, `PARAM_SFO` (`Call of Duty Online`), `ICON0_PNG` extracted locally from `codoMP_client_shipRetail.exe` (`RT_GROUP_ICON` ID 1 / `RT_ICON` ID 5, 512×512 RGBA preserving aspect & alpha), 28/28 `pkg_validate` checks OK. |
| Native Graphics Init — Piglet GLES2 (`libScePigletv2VSH` + `libSceShaccVSH`) | `COMPILED` | `NOT_IMPLEMENTED` | `engine/gfx/ps4_renderer.cpp`. Dynamically loads `.sprx` via `sceKernelLoadStartModule`/`sceKernelDlsym`; includes GLES2 2D UI quad pass for startup/menus; awaits real PS4 test. |
| Native Graphics Fallback — `libSceVideoOut` Direct-Memory 1920×1080 Scanout | `COMPILED` | `IMPLEMENTED` | `engine/gfx/ps4_renderer.cpp` + `engine/gfx/software_rasterizer.cpp`. Zero external `.sprx` requirement; double-buffered direct memory flip queue + bilinear 2D UI compositor. |
| Native Vulkan AMD GCN Backend (`ps4-kit` Mesa 26.0.8 + RADV + `ps4_vk_wsi`) | `NOT_IMPLEMENTED` | `NOT_IMPLEMENTED` | Next major GPU milestone: eliminates Sony `.sprx` dependency completely while delivering full native AMD Liverpool GCN hardware rendering. Requires clang 21 sysroot + Mesa 26.0.8 RADV build. |
| Direct Memory Allocator (`sceKernelAllocateDirectMemory` / `MapDirectMemory`) | `COMPILED` | `IMPLEMENTED` | `engine/platform/platform_ps4.cpp`, `engine/platform/crash_log.cpp` (`MemoryStats` tagged tracking). |
| POSIX Filesystem & Game Data Discovery (`/data/codol`, `/usb0/codol`, `/usb1/codol`) | `COMPILED` | `IMPLEMENTED` | Avoids broken OpenOrbis musl `std::filesystem` `*at` stubs; pure POSIX `fopen`/`fseeko`/`opendir`/`readdir`/`stat`. |
| Multithreading (POSIX `pthread` Worker Pool) | `COMPILED` | `IMPLEMENTED` | 4-worker persistent thread pool in `platform_ps4.cpp` / `platform_pc.cpp` driving parallel rasterizer scanline bands. |
| DualShock 4 Controller (`libScePad` + `libSceUserService`) | `COMPILED` | `IMPLEMENTED` | `engine/platform/input_ps4.cpp`: analog sticks with deadzones, L2/R2 triggers, D-Pad, OPTIONS/TOUCHPAD, lightbar & rumble hooks. |
| Native Audio Output (`libSceAudioOut` 48 kHz Stereo S16 Mixer Thread) | `COMPILED` | `IMPLEMENTED` | `engine/platform/audio_ps4.cpp`: dedicated streaming audio thread mixing PCM voices. |
| Crash Logging & Signal Handler (`SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGABRT`/`SIGFPE`) | `COMPILED` | `IMPLEMENTED` | `engine/platform/crash_log.cpp`: signal-safe crash trace + boot ring buffer to `/data/project_dragon/crash.log`. |
| Explicit Developer Tools (Debug Console, Dev HUD, Dev Map Selector) | `COMPILED` | `IMPLEMENTED` | `engine/platform/debug_console.cpp`. Strictly gated behind explicit dev flags (`--dev-map-menu`, `--dev-hud`, `--dev-boxes`, PS4 `L1+R1+TOUCHPAD` / `TOUCHPAD`) — OFF during normal startup and gameplay. |

---

## Phase 3 — Original Call of Duty Online `1.1.1.13` Asset Support

| Asset Subsystem | PS4 Target Status | PC Debug Status | Evidence / Notes |
|---|---|---|---|
| Portable Zero-Dependency Crypto (`SHA-256`, `AES-192-CTR` LE/BE, `RSA-2048-OAEP`, PKCS#1 DER) | `COMPILED` | `IMPLEMENTED` | `third_party/codol_zone_portable/codol_zone/src/crypto_portable.cpp`. Verified against FIPS 180-4, FIPS 197 C.2, ZIP CRC-32, and real `1.1.1.13` signed FastFiles & IWDs. |
| Runtime Key & Wavelet Table Extraction from User's `codoMP_client_shipRetail.exe` | `COMPILED` | `IMPLEMENTED` | Zero embedded proprietary keys; verified dynamically against supplied `1.1.1.13` executable. |
| FastFile v334 (`IWff0100` Signed + `IWffu100` Unsigned) Reader & Zone Loader | `COMPILED` | `IMPLEMENTED` | Verified across `dragon_tests` (33/33) and `dragon_validate` (`docs/COMPATIBILITY_REPORT.md`). |
| Encrypted `.iwd` Archive Set & Streamed `.iwi` Decoder (DXT1/3/5, RGBA, Wavelet) | `COMPILED` | `IMPLEMENTED` | Mounted 62 IWD image archives (42,221 images); visually verified real decoded textures across maps & UI. |
| Material & Embedded Lightmap Resolution (`Material` / `GfxImage`) | `COMPILED` | `IMPLEMENTED` | 100% textured surface resolution on tested maps; volumetric/additive beam filtering + sRGB lightmap lift. |
| Automated Asset Validation Tool (`dragon_validate`) | `NOT_IMPLEMENTED` *(PC host tool)* | `IMPLEMENTED` | `tools/dragon_validate/main.cpp` → `docs/COMPATIBILITY_REPORT.md`. |

---

## Phase 4 — Engine Reconstruction

| Engine Feature | PS4 Target Status | PC Debug Status | Current Scope & Next Steps |
|---|---|---|---|
| **First Milestone: Original CODOL Map Geometry & Textures Rendered** | `COMPILED` | `IMPLEMENTED` | **ACHIEVED** (PC visually verified across `mp_killhouse_sh`, `mp_nuked`, `mp_crash_tropical`, `zm_factory_sh`; native PS4 `eboot.bin` + `.pkg` compiled & verified). |
| World Rendering (`GfxWorld` vertices, indices, surfaces, static `XModel` props, lightmaps, frustum culling) | `COMPILED` | `IMPLEMENTED` | Static props baked up to configurable budget; next: PVS/portal cell culling, dynamic lightgrid probes, cubemap sky shader, translucent/additive pass. |
| BSP & Collision (`clipMap_t` `cbrush_t` swept AABB `CM_BoxTrace` & `CM_RayTrace`) | `COMPILED` | `IMPLEMENTED` | Convex brushes verified in `dragon_tests` and real maps; next: terrain displacement triangle-soup (`cLeafBrushNode` / `AabbTree` partitions) & static model collision. |
| Player Movement (IW4 `PM_Move`: walk, sprint, crouch, prone, ADS, jump, gravity, step-slide, noclip) | `COMPILED` | `IMPLEMENTED` | Verified collision stop & floor snap in `dragon_tests`; next: ladder climb, mantle, viewmodel bob & sway. |
| Materials, Lighting & Dynamic Shadows | `COMPILED` | `IMPLEMENTED` | Diffuse, alpha-cutout, lightmap modulation, sun directional diffuse, fog implemented; next: normal/specular maps, real-time shadow maps & `ComWorld` dynamic omni/spot lights via RADV Vulkan. |
| Skeletal Animation (`XModel` bones & `XAnimParts` registry) | `COMPILED` | `IMPLEMENTED` | `SkeletalRegistry` indexes real `XModel` bones and `XAnimParts` from zones; next: GPU skinning, frame interpolation, blend trees, first-person viewmodel & third-person character rendering. |
| Particle Effects (`FxEffectDef`) | `NOT_IMPLEMENTED` | `NOT_IMPLEMENTED` | Planned: decode `ASSET_TYPE_FX` emitters (billboards, muzzle flashes, tracers, smoke, blood/sparks). |
| Weapon Systems (`WeaponCompleteDef` metadata, firing, reload, raycast hit detection) | `COMPILED` | `IMPLEMENTED` | Reads real `ASSET_TYPE_WEAPON` headers (`thundergun_zm`, etc.) + fires raycast against ClipMap & bots; next: load `common_mp.ff` weapon catalog, viewmodels, recoil/spread, ADS sights, grenades, killstreaks. |
| GSC Script Runtime | `COMPILED` | `IMPLEMENTED` | Clean-room cooperative thread VM (`thread`/`wait`/`waittill`/`notify`/`setDvar`) passing unit tests + parsing map `RawFile` scripts; next: full IW4 GSC bytecode/AST coverage & built-in entity method bindings. |
| Offline Server, Bots & Game Modes (MP / Zombies / Cyborg / Co-op) | `COMPILED` | `IMPLEMENTED` | 20 Hz `OfflineServer` spawning bots along real `MapEnts` `node_pathnode` graphs; next: A* waypoint navigation, combat AI, Zombies wave/mystery-box/perk/round logic, TDM/FFA/S&D/Domination scoring. |
| Authentic Startup & IW Menus (`splash.bmp`, `ASSET_TYPE_FONT`, `ASSET_TYPE_MENU` `ui_mp/load.txt` & `connect`, `localized_code_post_gfx_mp.ff`) | `COMPILED` | `IMPLEMENTED` | `engine/ui/authentic_ui.cpp`: Loads root `splash.bmp` (800×510), 9 IW fonts (`fonts/gamefonts_pc` / `devfonts_pc`), IW menus (`load`, `connect`, `error_*`) with `ScrPlace_ApplyRect` coordinates, and 9,385 GBK localized entries. Visually verified side-by-side on PC Debug. |
| Scaleform GFx Display-List Logo/Bitmap Renderer (`codologin.gfx`, `codo.gfx`) | `COMPILED` | `IMPLEMENTED` *(static bitmaps only)* | Parses GFX v10 stage (`1280×960`), `DefineExternalImage2`, `ExportAssets`/`SymbolClass`, `DefineSprite`, `PlaceObject2/3` (`Matrix`, `CXFormWithAlpha`, SWF `BlendMode 8` Additive) for `loginView`, `logoView`, `logoCyborgsView`. Visually verified side-by-side on PC Debug. |
| Scaleform GFx Interactive AS3 Runtime (`DoABC` tag 82), Vector `DefineShape`, CJK `fonts_ingame.gfx`, In-Game `codohud.gfx` | `NOT_IMPLEMENTED` | `NOT_IMPLEMENTED` | Explicitly logged as `[UI][UNFINISHED]` at runtime. Required for full interactive Chinese menus, buttons, and in-game HUD. |
| Audio & Video Decoding (`SoundAliasList` / Miles MP3/WAV / Bink 2 `KB2i` `PUB_logo.bk2` & `CODO_logo.bk2`) | `NOT_IMPLEMENTED` | `NOT_IMPLEMENTED` | PS4 `libSceAudioOut` hardware output thread is compiled; Bink 2 headers are inspected and logged as `[UI][UNFINISHED]` (`KB2i` decoder `NOT_IMPLEMENTED`). |

---

## Phase 5 — Automated Build System

| Deliverable | Status | Path |
|---|---|---|
| Dependency & Toolchain Setup | `IMPLEMENTED` | `SETUP.bat`, `scripts/setup.sh` |
| Native PS4 Build + ELF/OELF/FSELF Verification | `IMPLEMENTED` | `BUILD_PS4.bat`, `scripts/build_ps4.sh` → `logs/build_ps4.log` |
| Standalone PS4 PKG Packaging + Verification | `IMPLEMENTED` | `PACKAGE_PS4.bat`, `scripts/package_ps4.sh` → `logs/package_ps4.log` |
| PC Debug Build + Automated Unit & Real-Game Tests | `IMPLEMENTED` | `BUILD_PC_DEBUG.bat`, `scripts/build_pc_debug.sh` → `logs/build_pc_debug.log` |
| Automated Asset Validator + Compatibility Report | `IMPLEMENTED` | `VALIDATE_ASSETS.bat`, `scripts/validate_assets.sh` → `docs/COMPATIBILITY_REPORT.md` |

---

## Immediate Next Milestones

1. **Hardware / Emulator Bring-Up (`HARDWARE_TESTED` / `EMULATOR_TESTED`)**: Install `dist/IV0000-DRGN00113_00-PROJECTDRAGON000.pkg` on a jailbroken PS4 (FW 5.05–11.00+) with user's CODOL `1.1.1.13` files in `/data/codol` (or `/usb0/codol`), verify boot log, DualShock 4 input, map streaming, and capture real hardware screenshots.
2. **Zero-`.sprx` Native Vulkan GPU Backend (`ps4-kit` Mesa/RADV)**: Build `ps4-kit` clang 21 sysroot + Mesa 26.0.8 RADV and add `engine/gfx/ps4_vulkan_renderer.cpp` for 60 FPS hardware-accelerated AMD GCN rendering (normal maps, cubemap skyboxes, dynamic shadows, GPU skeletal skinning) with zero external Sony `.sprx` files.
3. **Full Gameplay Parity**: Load shared `common_mp.ff` / `patch_mp.ff` zones alongside map zones for complete weapon catalogs, first-person viewmodels, character `XModel` rendering with `XAnimParts`, original `SoundAlias` audio playback, `MenuDef` UI, and full MP + Cyborg Zombies gameplay rules.
