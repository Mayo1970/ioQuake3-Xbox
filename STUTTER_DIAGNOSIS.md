# Stutter diagnosis (open)

Opened 2026-09-25. To do in a later session.

## Where we are

- Last commit: `bceb1d9` "Better optimization, debug build toggle".
- Hardware results on Q3DM11 with 4 bots:
  - Perf build 2 (fast world path, `-O3 -ffast-math`, 24 kHz sound mix): much smoother, but some stutter remains.
  - Non-debug build (no log file writes): SUCCESS.
- Last measured values (debug build, per-frame averages over 600 frames):
  - total 20.3-22.3 ms
  - scene 11.7-14.3 ms
  - vblank wait 2.7-3.7 ms
  - GPU wait less than 0.3 ms
- These are averages. They do not show single slow frames.

## Suspected cause: bot frames

The server runs its frame and the bot AI every 50 ms (`sv_fps` 20).
That makes one frame in every 2 or 3 slower than the others.

## Plan

1. Add three values to the `Xbox perf:` line:
   - the longest frame
   - the number of frames over 33 ms
   - the server frame time per frame
2. Do one hardware run on Q3DM11 with 4 bots.
3. Choose the fix from those numbers.

## Notes for the work

- Only a debug build writes the `Xbox perf:` line: `make -f Makefile.xbox DEBUG=y`.
  Delete `code/sys/sys_xbox.obj` when you switch between debug and non-debug builds.
- Perf counters: `XboxNV2APerfCounters`, `XboxNV2ALogPerf` and `XboxNV2A_BeginFrame`
  in `code/renderernv2a/xbox_nv2a.c`. The line prints every 600 frames.
- Server frame: `Com_Frame` calls `SV_Frame( msec )` in `code/qcommon/common.c`.
  Time that call to get the server frame time.
- nxdk printf has no `%f`. Print whole numbers (microseconds), as the current line does.
