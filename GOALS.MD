# ioQuake3 Xbox Goal Map

## Mission

Produce a native original-Xbox build of ioquake3 using the open nxdk/libxenon
toolchain and native NV2A rendering. The first useful milestone is a bootable
XBE that reaches the ioquake3 console; the first playable milestone is a single
Quake III map with input, sound, and rendering working.

Confidence tags in this file describe the evidence level of technical claims:
`[HIGH]` verified from the checked-out trees or toolchain, `[MEDIUM]` a strong
engineering assumption, and `[LOW]` an item requiring implementation evidence.

## Current baseline

- The ioquake3 source snapshot from `../ioq3` has been copied here, excluding
  only its `.git` metadata. The destination contains 1,048 source/assets files
  from that snapshot. [HIGH]
- The working source remains organized as upstream ioquake3: `code/`, `cmake/`,
  `docs/`, `misc/`, and `ui/`. [HIGH]
- The current source expects SDL and OpenGL/OpenGL ES backends on ordinary
  platforms. [HIGH]
- The OG Xbox target is a 32-bit Pentium III/NV2A system with a 64 MiB unified
  memory pool. [HIGH]
- The pulled build baseline is `xboxdev/nxdk:latest`; its local image digest is
  `sha256:be4dcf3336470d525ca05716c40f85852c6140225f3442baa0117ee4c32a65eb`.
  [HIGH]
- `Makefile.xbox` produces `build_xbox/default.xbe` with the pinned nxdk
  container. The full qcommon/client engine-shell XBE was booted on real OG
  Xbox hardware and reached the running frame loop without the nxdk `strtod()`
  assertion or command-buffer overflow. [HIGH]
- The validated shell loads all clean user-owned pak files, initializes the
  native UI module and sound system, and completes common initialization.
  Renderer and in-game validation have since passed on hardware; sustained-play
  validation remains outstanding. [HIGH]
- No emulator smoke test has been run; hardware remains the authoritative
  validation environment for the current target. [HIGH]

## Non-goals and constraints

- Do not use Microsoft proprietary XDK headers, libraries, tools, or D3D8
  interfaces. The target toolchain is nxdk/libxenon only. [HIGH]
- Do not make OpenGL, OpenGL ES, SDL, or a desktop D3D translation layer a
  runtime dependency of the Xbox build. [HIGH]
- Do not copy Quake III Arena data into the source tree. Stage user-owned game
  data separately when packaging a test image. [HIGH]
- Do not run broad smoke tests by default. Run a validation step only when it
  is the explicit gate for the current goal or is needed to diagnose a blocker.
  [HIGH]
- Keep the upstream source changes isolated and reviewable; do not rewrite the
  portable game, renderer, or VM code before the platform boundaries are known.
  [MEDIUM]

## Workstream map

Sections are ordered by current priority: complete, then in progress, then not
started. Goal IDs are stable.

### G0 — Source and build inventory

Status: **complete**

- Identify the exact upstream files used by the client, renderer, sound, input,
  filesystem, networking, and VM targets. [HIGH]
- Record every SDL/OpenGL/platform include and link dependency that enters the
  Xbox target. [HIGH]
- Keep `renderergl2` out of the first Xbox target; use the simpler renderer
  feature set as the bring-up reference. [MEDIUM]

Exit: the target source list and dependency list are written down before the
first implementation pass.

### G1 — Native nxdk target skeleton

Status: **complete**

- Add an Xbox-specific build entry point, preferably a dedicated
  `Makefile.xbox` or equivalent, without breaking the host CMake build. [MEDIUM]
- Establish nxdk compiler/linker flags, include paths, startup code, XBE
  output, and a deterministic output directory. [HIGH]
- Build the smallest executable containing the platform entry point and a
  debug console logger. **Complete:** the XBE builds and boots on hardware. [HIGH]
- Disable unsupported desktop targets and optional libraries from this target;
  retain the upstream build for host regression use. [MEDIUM]
- Compile the qcommon/client-facing command boundary without SDL, OpenGL, or
  proprietary XDK dependencies. **Complete; V1 passed with the pinned nxdk
  container and emitted `build_xbox/default.xbe`.** [HIGH]
- Add engine-facing logging and a controlled `quit` path. **Complete.** [HIGH]

Exit: the nxdk container produces a valid XBE and links the platform shell.
The build-side and hardware exits are complete. [HIGH]

### G2 — Platform services

Status: **complete**

- `Sys_*`: timing, sleep, exit/error handling, paths, directory enumeration and
  conservative memory reporting live in `code/sys/sys_xbox.c`. **Done.** [HIGH]
- Filesystem: the clean user-owned pak set loads from `D:\baseq3\` with
  preserved pak semantics, including zlib decompression under nxdk's `Z_SOLO`
  build. Home path uses E: when available and falls back to `D:\baseq3`. **Done.**
  [HIGH]
- Input: nxdk's native XID USB driver, all four controller slots, analog face
  buttons/triggers, sticks, hot-plug callbacks and disconnect clearing. State
  goes through the `IN_*` boundary into `SE_KEY` and `SE_JOYSTICK_AXIS` events.
  **Done on hardware.** [HIGH]
- Keyboard and mouse: nxdk HID support is enabled (`NXDK_USB_ENABLE_HID`,
  libusbohci `src_hid`). Boot keyboards map key and modifier transitions to
  `SE_KEY` and printable input to `SE_CHAR`. Mice read their HID report
  descriptor and decode buttons 1-5, X, Y and wheel from it in report protocol,
  which covers report IDs and 12/16-bit axes. Without a usable descriptor, a
  mouse falls back to boot protocol. Hot-plug disconnects release held keys and
  buttons. Backspace in text fields sends ctrl-h `SE_CHAR`, as in
  `sdl_input.c`. **Done on hardware** (XBE md5 `16031d7a...`). [HIGH]
- Sound: native AC97 PCM service with contiguous DMA buffer allocation, a
  refill callback, silence fallback and pause/shutdown control. **Done.** [HIGH]
- Networking: UDP/IPv4 over nxdk lwIP (`code/sys/xbox_net.c`), started with the
  dashboard settings before `Com_Init`, as on the PS3/PS4/Wii U ports. When DHCP
  times out, it falls back to link-local 169.254/16 for System Link. LAN play
  **done on hardware** (XBE md5 `5c5041e3...`). [HIGH]

Exit: the executable reaches the running qcommon/client engine shell without
SDL, desktop OpenGL, or proprietary XDK dependencies, with the native services
connected to the full qcommon/client boundary. Met. [HIGH]

### G3 — Native NV2A renderer bring-up

Status: **in progress (priority 1) — hardware-confirmed for menu, MD3 models,
BSP world, lightmaps, sky and RoQ cinematics; combat effects await a hardware
test**

Create a new native renderer backend, provisionally `code/renderernv2a/`, using
nxdk's native Xbox graphics facilities such as pbkit/xgu. [HIGH]

Use the UT99 Xbox replacement platform as the architectural reference and
selectively as the low-level backend base. Reuse its proven separation of
Xbox startup/platform services, viewport/input handling, render-state caching,
texture caching, dynamic vertex buffering, and frame presentation. Adapt those
pieces to ioquake3's renderer interfaces; do not import Unreal's
`URenderDevice`/scene lifecycle or its incompatible build/runtime dependencies.
The Quake III renderer remains the owner of shader stages, tessellation, world
surfaces, lightmaps, models, and draw ordering. [HIGH]

Bring-up order and state:

1. Video mode, framebuffer, clear, present, and a solid-color triangle.
   **Done on hardware.** [HIGH]
2. 2D stretch blits, console text, screenshots/diagnostics, and scissor state.
   **Done on hardware.** [HIGH]
3. Indexed textured triangles with nearest/linear filtering and texture upload.
   [HIGH]
4. Quake 3 world surfaces, lightmaps, sky and batching. **Done on hardware.**
   Fog is not done. [HIGH]
5. Alias/MD3 models, sprites, particles, marks, dynamic lights, and UI.
   MD3 models, skins, entity lighting and UI are done on hardware. Sprites,
   beams, rail, lightning and polys are implemented and await hardware. Marks
   are not done (MarkFragments stub). [MEDIUM]
6. Shader-script fallbacks and the visual features required by the base game.
   Multi-stage shaders and RoQ cinematics are done on hardware. [MEDIUM]

Remaining work:

- Hardware test of the combat effects build.
- Fog, deforms, polygonOffset, world dlights, RF_DEPTHHACK.
- Wall marks (MarkFragments), videoMap stages.
- Texture-cache staleness after a pool reuse on map load.

The renderer targets the GL1-era feature contract and avoids GL2 requirements
such as GLSL, FBOs, VAOs, and desktop extension discovery. [HIGH]

Exit: a supplied test map renders world geometry, lightmaps, a player/model,
HUD text, and a stable present path without a host GL context.

### G4 — Game-code and VM integration

Status: **in progress (priority 2) — native qagame, cgame and q3_ui
hardware-confirmed in a bot game; map transition not confirmed**

- qagame, cgame and q3_ui are native code linked into the XBE. **Done on
  hardware** (2026-09-24, XBE md5 `eba99788...`). [HIGH]
- Verify syscall ABI, alignment, endian assumptions, packed structures, and
  32-bit pointer-sized fields. [HIGH]
- Keep gameplay changes separate from platform and renderer changes. [HIGH]

Remaining work:

- Q3DM11 map load still fails, now later: `Hunk_AllocateTempMemory` fails on
  262200 bytes during the world texture registration. See G6, "Q3DM11 memory
  budget".
- Confirm a map-to-map transition. The q3dm0 -> menu -> q3dm11 sequence
  reloaded all three modules correctly.
- Hardware test of the botlib entity-vector fix (below).

Native modules (the VM mode decision; JIT and interpreter were rejected: the
JIT costs about 3-6x the bytecode size in RAM, the interpreter is too slow):

- Build (`Makefile.xbox`): each module compiles to LLVM bitcode, `llvm-link`
  merges it, and `opt -passes=internalize,globaldce` keeps only
  `vmMain<QAG|CG|UI>` and `dllEntry<QAG|CG|UI>` global. So the three copies of
  `q_shared`, `bg_misc` and `Com_Printf` never clash in the XBE link. [HIGH]
- `code/sys/xbox_module.h` is force-included into every module file: entry
  renames, one data/bss section pair per module (`#pragma clang section`), and
  float-capable `vsnprintf`/`sscanf` from `code/sys/xbox_module_libc.c`
  (pdclib skips `%f` in both; `g_spawn.c` origins use `sscanf("%f")`). The
  formatter passed 158,815 host checks against musl. [HIGH]
- `llvm-objcopy --rename-section` is not supported for COFF, and
  `#pragma bss_seg` turns bss into file data. `#pragma clang section` keeps
  real bss sections. [HIGH]
- `code/sys/xbox_modules.c`: asm markers (`.data$qag0`/`2` etc.) bracket each
  module's state. The first load keeps a `.data` snapshot; later loads restore
  it and zero `.bss`, like a fresh QVM or DLL. The PSP port showed that q3_ui
  breaks without this. [HIGH]
- `vm.c` loads the built-in module when `vm_* = 0` and the game is baseq3. Mods
  and remote pure servers still load QVMs through the interpreter. [HIGH]
- `+set sv_pure 0` at boot: a pure server drops a client whose cp message lacks
  the cgame/ui QVM pak references, the local listen server included. [HIGH]
- Memory: the modules add 3.9 MiB to the XBE image (code 0.6 MiB; bss cgame
  1446 KiB, qagame 1308 KiB, ui 543 KiB) and free the 12.5 MiB the QVMs used in
  the hunk. The hunk went from 24 to 20 MiB, so free RAM after `Com_Init` stays
  at 6 MiB. q3dm0 now has 12021 KiB of hunk free after the world load (3858
  before). [HIGH]

Botlib entity vectors (implemented, awaits hardware):

- `be_aas_bspq3.c` `AAS_VectorForBSPEpairKey` read vectors with
  `sscanf("%lf")`, which pdclib skips, so every BSP entity vector was 0. On
  hardware every q3dm0 item logged "in solid ... not reachable for bots" and
  q3dm11 camp spots logged "in solid", so bots never went for items. The Xbox
  path now parses with `COM_Parse` and the `xbox_qcommon.c` `atof`. This bug
  existed before the native modules. [HIGH]
- Other engine float formatting is still broken by pdclib (for example
  "CL_InitCGame:  seconds"); it is cosmetic in the known cases. [HIGH]

Exit: a local match can load a map, spawn a player, move, fire, and complete a
map transition.

### G6 — Performance and robustness

Status: **partly started (priority 4) — static BSS cut from 25.2 to 4.7 MiB;
hunk 20 MiB / zone 8 MiB with native modules; Q3DM11 budget measured; no
benchmark map yet**

- Establish memory budgets for code, maps, textures, sound, VM heaps, and
  transient renderer buffers. [MEDIUM]

Q3DM11 memory budget (measured 2026-09-24; Matt will choose the approach, none
of the options below is implemented):

- Method: a 32-bit host run of the real `cm_load.c` and `xbox_nv2a_world.c` on
  the pak0 maps, calibrated on the q3dm0 hardware line `hunk_free=12021 KiB`.
  [HIGH for CM and world, MEDIUM for the calibrated rest]

| Hunk use (20480 KiB total) | q3dm0 KiB | q3dm11 KiB |
|---|---|---|
| Collision map (`CM_LoadMap`) | 1035 | 2921 |
| AAS file (bot navigation) | ~1276 | ~3704 |
| Snapshot entities + other botlib data (calibrated) | ~3581 | ~3581 |
| Free when the renderer starts the world | ~14588 | ~10274 |
| World data built by `xbox_nv2a_world.c` | 2567 | 5819 |
| BSP file held during the whole world load | 3243 | 8291 |
| Peak need of the world load now (file + world) | 5810 | 14111 |

- The world loader keeps the whole BSP in hunk temp memory until the end, so
  q3dm11 is about 3.8 MiB short. This matches the hardware failure in the
  middle of the world shader registration. [HIGH]
- Option 1, lump streaming: read each lump when a step needs it and free it
  after. The peak for q3dm11 drops to 8362 KiB (surfaces step: shaders,
  drawverts, surfaces, drawindexes), about 1.6 MiB under the free space; about
  4.4 MiB of hunk stays free after the load (cgame defers player models below
  about 3.9 MiB). Cost: `FS_Seek` in a pk3 re-inflates from the file start on a
  backward seek, and `drawindexes` is the last lump. [MEDIUM]
- Next limit, texture pool (6 MiB fixed, 16-bit textures, 32-bit lightmaps):
  5042 KiB were used at the q3dm11 failure with about half the world shaders
  registered. q3dm11 has 25 lightmaps (1600 KiB) against 9 on q3dm0. A full
  pool skips textures ("texture pool full"), it does not crash. Options:
  16-bit lightmaps (saves about 800 KiB, some banding) or DXT1 world textures
  (4x smaller than 16-bit, needs a load-time encoder). [MEDIUM]
- Next limit, MD3 models (malloc, not hunk): about 0.8-1.0 MiB per player at
  LOD0 (sarge 1041 KiB, crash 875, lucy 825, biker 823). Five players need about
  4.5 MiB; free RAM after the q3dm0 world load was 5 MiB. Option: LOD1 player
  models (about half the size). [MEDIUM]
- Zone: 8 MiB, peak use unknown. The PSP port runs bots with a 5 MiB zone. One
  `meminfo` during a bot match shows whether 2-3 MiB can move to the hunk, the
  texture pool or the heap. [LOW]
- Prefer batching, persistent allocations, texture reuse, and bounded scratch
  buffers; avoid per-draw heap traffic. [MEDIUM]
- Add optional counters for frame time, draw calls, texture uploads, memory
  high-water marks, and dropped/failed submissions. [MEDIUM]
- Tune only after a representative map renders correctly. [HIGH]

Exit: a repeatable benchmark map runs without allocation failures, visible
corruption, or progressive leaks.

### G5 — Packaging and content staging

Status: **not started (priority 5)**

- Define the XBE name, default arguments, save/config location, and game-data
  directory layout. [MEDIUM]
- Create a repeatable package step that accepts user-owned Quake III Arena data
  and emits an HDD/USB or emulator-ready tree. [MEDIUM]
- Keep source, generated binaries, logs, and staged game data in separate
  directories. [HIGH]

Exit: a clean package can be assembled from a fresh build and separately staged
game data without manual source-tree edits.

## Validation gates

Future work uses these gates instead of an unconditional smoke-test loop:

- **V0 — static gate:** source inventory, diff review, and dependency checks.
- **V1 — build gate:** nxdk container produces the expected XBE and reports no
  unresolved desktop/XDK dependencies.
- **V2 — emulator gate:** use xemu/CXbox only when needed to diagnose startup,
  display, or packaging failures. Emulator success is not hardware proof.
- **V3 — hardware gate:** after V1 and a meaningful bootable artifact, validate
  on an actual modded Xbox with serial/debug logging and a known-good test map.
- **V4 — acceptance gate:** verify boot, map load, input, sound, renderer
  correctness, and a sustained play session; record exact build and content
  hashes.

Passed on hardware (V3): the engine shell, menu, MD3 models, BSP world, sky,
cinematics, bot games, LAN networking and native game modules. Outstanding: combat effects (V3), G5 packaging, G6
robustness and V4 acceptance. [HIGH]

## Definition of done

The project is complete when a clean nxdk build produces a packaged XBE that
boots on real OG Xbox hardware, loads user-owned Quake III Arena data, renders
the base game through the native NV2A backend, accepts controller input, plays
sound, and survives a sustained map session without known correctness or
memory failures. [LOW]
