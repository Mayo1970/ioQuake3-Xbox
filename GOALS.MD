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
  interpreted UI VM and sound system, and completes common initialization.
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

Status: **in progress (priority 2) — hardware-confirmed in-game with bots;
map transition not confirmed**

- Build qagame, cgame, and UI using the least fragile VM mode first. An
  interpreted VM is the initial fallback if executable-memory or architecture
  assumptions make native VM code unsafe. [MEDIUM]
- Verify syscall ABI, alignment, endian assumptions, packed structures, and
  32-bit pointer-sized fields. [HIGH]
- Keep gameplay changes separate from platform and renderer changes. [HIGH]

Remaining work:

- Q3DM11 map load fails: `Hunk_AllocateTempMemory` fails on 8490248 bytes
  because the 8.1 MiB BSP is read after qagame, cgame and UI fill the hunk.
- Confirm a map transition.

VM mode decision (interpreter now, JIT or native later):

- Current state: `-DC_ONLY` disables `id386`, so `HAVE_VM_COMPILED` is off and
  only `vm_interpreted.c` is linked. [HIGH]
- nxdk `malloc` (dlmalloc) gets pages with `VirtualAlloc(PAGE_READWRITE)`;
  `PAGE_EXECUTE_READWRITE` exists in `xboxkrnl.h`. The Pentium III has no NX
  bit, but execute-from-heap still needs a hardware test. [MEDIUM]
- JIT (`vm_x86.c`): compiled code is about 3-6x the bytecode size, because each
  bytecode becomes its own inline x86 sequence with `dataMask` checks. [MEDIUM]
- JIT budget: 8 MiB resident for game, cgame and ui, plus 4 MiB transient during
  compile. The temp buffer (`codeLength * 8`) comes from `Z_Malloc`, and the
  zone is only 8 MiB, so raise the zone or allocate it outside. Sizes are
  estimates; log `codeLength` to measure. [LOW]
- JIT steps: drop `C_ONLY` for the VM, add `vm_x86.c`, fix the clang calling
  path (`vm.c:840`), use `VirtualAlloc(PAGE_EXECUTE_READWRITE)` for `XBOX`, keep
  `vm_game`/`vm_cgame`/`vm_ui` switchable. [MEDIUM]
- Native (game, cgame, ui built with nxdk clang and linked into the XBE): about
  1-2x bytecode size, no compile buffer, no `instructionPointers`, no execute
  risk. `vm->entryPoint` in `vm.c:836` already supports it. Needs module source;
  mods that ship their own QVMs (Team Arena, OSP, CPMA) stop working. Asset pk3
  files (maps, textures, models, sounds) still load. [MEDIUM]
- Ranking on 64 MiB: speed native > JIT > interpreter; RAM native > interpreter
  > JIT; risk interpreter < native < JIT. [MEDIUM]

Exit: a local match can load a map, spawn a player, move, fire, and complete a
map transition.

### G2 — Platform services

Status: **in progress (priority 3) — all services validated except networking**

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
- Keyboard and mouse: **not started.** Enable the nxdk HID class driver
  (`NXDK_USB_ENABLE_HID`, libusbohci `src_hid`) and map its reports to `SE_KEY`
  and `SE_MOUSE` events. Needs a USB-to-controller-port adapter. [MEDIUM]
- Sound: native AC97 PCM service with contiguous DMA buffer allocation, a
  refill callback, silence fallback and pause/shutdown control. **Done.** [HIGH]
- Networking: **not started.** Add UDP/System Link only after the single-player
  loop is stable. [MEDIUM]

Exit: the executable reaches the running qcommon/client engine shell without
SDL, desktop OpenGL, or proprietary XDK dependencies, with the native services
connected to the full qcommon/client boundary. Met, except networking. [HIGH]

### G6 — Performance and robustness

Status: **partly started (priority 4) — static BSS cut from 25.2 to 4.7 MiB;
no benchmark map yet**

- Establish memory budgets for code, maps, textures, sound, VM heaps, and
  transient renderer buffers. [MEDIUM]
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
cinematics and bot games. Outstanding: combat effects (V3), G5 packaging, G6
robustness and V4 acceptance. [HIGH]

## Definition of done

The project is complete when a clean nxdk build produces a packaged XBE that
boots on real OG Xbox hardware, loads user-owned Quake III Arena data, renders
the base game through the native NV2A backend, accepts controller input, plays
sound, and survives a sustained map session without known correctness or
memory failures. [LOW]
