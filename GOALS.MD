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
  Renderer, map, and sustained-play validation remain outstanding. [HIGH]
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

### G0 — Source and build inventory

Status: **complete for planning**

- Identify the exact upstream files used by the client, renderer, sound, input,
  filesystem, networking, and VM targets. [HIGH]
- Record every SDL/OpenGL/platform include and link dependency that enters the
  Xbox target. [HIGH]
- Keep `renderergl2` out of the first Xbox target; use the simpler renderer
  feature set as the bring-up reference. [MEDIUM]

Exit: the target source list and dependency list are written down before the
first implementation pass.

### G1 — Native nxdk target skeleton

Status: **complete for the qcommon/client engine-shell milestone**

- Add an Xbox-specific build entry point, preferably a dedicated
  `Makefile.xbox` or equivalent, without breaking the host CMake build. [MEDIUM]
- Establish nxdk compiler/linker flags, include paths, startup code, XBE
  output, and a deterministic output directory. [HIGH]
- Build the smallest executable containing the platform entry point and a
  debug console logger. **Complete:** the XBE builds and boots on hardware. [HIGH]
- Disable unsupported desktop targets and optional libraries from this target;
  retain the upstream build for host regression use. [MEDIUM]

Completed G1 shell slice:

- Compile the qcommon/client-facing command boundary without SDL, OpenGL, or
  proprietary XDK dependencies. **Complete for the G1 shell slice; V1 passed
  with the pinned nxdk container and emitted `build_xbox/default.xbe`.** [HIGH]
- Add engine-facing logging and a controlled `quit` path. **Complete for the
  G1 shell slice.** [HIGH]

Remaining G1 work:

- None for the validated qcommon/client engine-shell milestone. [HIGH]

Exit: the nxdk container produces a valid XBE and links the platform shell.
The build-side and hardware exits are complete for the qcommon/client
engine-shell milestone. [HIGH]

### G2 — Platform services

Status: **in progress — core services and full qcommon/client shell validated**

Implement the minimum services behind ioquake3's existing interfaces:

- `Sys_*`: timing, console output, thread/critical-section behavior, exit,
  memory allocation, and file paths. [MEDIUM]
- Filesystem: map the game base directory to the Xbox HDD/volume layout and
  preserve pak-file semantics. [MEDIUM]
- Input: Xbox controller enumeration, digital buttons, analog sticks, triggers,
  and menu/navigation mapping. [MEDIUM]
- Sound: start with the smallest nxdk-compatible mixer/output path; defer
  advanced streaming until basic effects work. [MEDIUM]
- Networking: keep local/offline play first; add UDP/System Link only after the
  single-player loop is stable. [MEDIUM]

Completed native service slice:

- Xbox system timing, sleep, exit/error handling, paths, directory enumeration,
  and conservative memory reporting live in `code/sys/sys_xbox.c`. [HIGH]
- Controller input uses nxdk's native XID USB driver with all four controller
  slots, analog face buttons/triggers, sticks, hot-plug callbacks, and
  disconnect clearing. [HIGH]
- Xbox controller state is translated through the ioquake3 `IN_*` boundary into
  Quake `SE_KEY` and `SE_JOYSTICK_AXIS` events; the translation passed on real
  hardware. [HIGH]
- Sound has a native AC97 PCM service with contiguous DMA buffer allocation,
  a refill callback, silence fallback, and pause/shutdown control. [HIGH]
- The filesystem loads the clean user-owned pak set from `D:\baseq3\` with
  preserved pak semantics, including zlib decompression under nxdk's `Z_SOLO`
  build. [HIGH]
- Xbox home-path creation now uses E: when available and falls back to the
  existing `D:\baseq3` tree when the launch environment lacks a writable E:
  layout. [HIGH]
- The full qcommon/client engine shell reaches common initialization, loads
  the UI VM, initializes sound, and enters the frame loop on real hardware.
  [HIGH]

Next session: begin native NV2A renderer bring-up using the validated native
services and input-event boundary. [HIGH]

Exit: the executable reaches the running qcommon/client engine shell without
SDL, desktop OpenGL, or proprietary XDK dependencies, with the native services
connected to the full qcommon/client boundary. [HIGH]

### G3 — Native NV2A renderer bring-up

Status: **not started — primary risk**

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

Bring it up in this order:

1. Video mode, framebuffer, clear, present, and a solid-color triangle. [MEDIUM]
2. 2D stretch blits, console text, screenshots/diagnostics, and scissor state.
   [MEDIUM]
3. Indexed textured triangles with nearest/linear filtering and texture upload.
   Use RGBA4444 as the default Xbox texture representation; RGBA8888 is not the
   default path. [HIGH]
4. Quake 3 world surfaces, lightmaps, fog, sky, and batching. [LOW]
5. Alias/MD3 models, sprites, particles, marks, dynamic lights, and UI.
   [LOW]
6. Shader-script fallbacks and the visual features required by the base game.
   [LOW]

The renderer should initially target the GL1-era feature contract and avoid GL2
requirements such as GLSL, FBOs, VAOs, and desktop extension discovery. [HIGH]

Exit: a supplied test map renders world geometry, lightmaps, a player/model,
HUD text, and a stable present path without a host GL context.

### G4 — Game-code and VM integration

Status: **not started**

- Build qagame, cgame, and UI using the least fragile VM mode first. An
  interpreted VM is the initial fallback if executable-memory or architecture
  assumptions make native VM code unsafe. [MEDIUM]
- Verify syscall ABI, alignment, endian assumptions, packed structures, and
  32-bit pointer-sized fields. [HIGH]
- Keep gameplay changes separate from platform and renderer changes. [HIGH]

Exit: a local match can load a map, spawn a player, move, fire, and complete a
map transition.

### G5 — Packaging and content staging

Status: **not started**

- Define the XBE name, default arguments, save/config location, and game-data
  directory layout. [MEDIUM]
- Create a repeatable package step that accepts user-owned Quake III Arena data
  and emits an HDD/USB or emulator-ready tree. [MEDIUM]
- Keep source, generated binaries, logs, and staged game data in separate
  directories. [HIGH]

Exit: a clean package can be assembled from a fresh build and separately staged
  game data without manual source-tree edits.

### G6 — Performance and robustness

Status: **not started**

- Establish memory budgets for code, maps, textures, sound, VM heaps, and
  transient renderer buffers. [MEDIUM]
- Prefer batching, persistent allocations, texture reuse, and bounded scratch
  buffers; avoid per-draw heap traffic. [MEDIUM]
- Add optional counters for frame time, draw calls, texture uploads, memory
  high-water marks, and dropped/failed submissions. [MEDIUM]
- Tune only after a representative map renders correctly. [HIGH]

Exit: a repeatable benchmark map runs without allocation failures, visible
corruption, or progressive leaks.

## Validation gates

The planning-only pass is complete. Future work uses these gates instead of an
unconditional smoke-test loop:

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

The full qcommon/client engine-shell artifact passed V3 hardware validation.
G3 renderer, G4 gameplay/map, G5 packaging, and G6 robustness work remain.
[HIGH]

## First implementation slice

The next coding slice should be deliberately small:

1. Add the nxdk build/startup skeleton. **Complete; V1 and the first hardware
   boot gate passed.**
2. Compile the qcommon/client-facing shell without SDL/OpenGL. **Complete; V1
   passed and `build_xbox/default.xbe` was emitted.**
3. Add logging and a controlled exit path. **Complete for the shell slice.**
4. Hardware-validate the current shell. **Complete; real hardware loaded the
   pak set, UI VM, sound system, and entered the frame loop.**
5. Begin native NV2A renderer bring-up as G3.

This slice does not require a smoke test unless the build itself fails or the
resulting artifact must be inspected to resolve a toolchain question. [HIGH]

## Definition of done

The project is complete when a clean nxdk build produces a packaged XBE that
boots on real OG Xbox hardware, loads user-owned Quake III Arena data, renders
the base game through the native NV2A backend, accepts controller input, plays
sound, and survives a sustained map session without known correctness or
memory failures. [LOW]
