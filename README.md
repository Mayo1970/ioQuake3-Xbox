# ioquake3-Xbox

A port of [ioQuake3](https://github.com/ioquake/ioq3) to the original Microsoft
Xbox (2001), built with [nxdk](https://github.com/XboxDev/nxdk) and a native
NV2A renderer (pbkit/xgu, no OpenGL layer).

Three flavors build from the same tree:

- **Quake III Arena** (`default.xbe`)
- **Team Arena** (`ta.xbe`, launched from the Q3 menu)
- **OpenArena** (standalone `default.xbe`)

## Status

- World geometry, textures, sky, models and particles render at 640×480
- Game, cgame and UI modules are compiled natively into the XBE (no QVM on
  local games)
- Sound effects, music and cinematic audio supported
- Networking: System Link / LAN discovery, internet server browser, hosting,
  and play
- Controller, USB keyboard and USB mouse supported (see **Controls** below)
- Player name defaults to the console's Xbox Live gamertag
- Custom dashboard icon (tested on UnleashX)
- Q3DM11 with 4 bots runs at ~45-50 FPS. V-sync is off by default; the frame
  rate caps at 60.

## Building

Just use Docker lol

```
docker pull xboxdev/nxdk:latest

docker run --rm -v "${PWD}:/src" -w /src xboxdev/nxdk:latest sh -c `
  "make -f Makefile.xbox -j8"
```

| Flavor | Command | Output |
|---|---|---|
| Quake III Arena | `make -f Makefile.xbox` | `build_xbox/default.xbe` |
| Team Arena | `make -f Makefile.xbox ta` | `build_xbox_ta/default.xbe` (rename to `ta.xbe`) |
| OpenArena | `make -f Makefile.xbox oa` | `build_xbox_oa/default.xbe` |

Add `DEBUG=y` to write a log file to `D:\ioquake3.log`
(`ioquake3_ta.log` / `ioquake3_oa.log` for the other flavors).

## Installing on Xbox

Requires a softmodded or modchipped Xbox; tested with UnleashX.

Buy the base game legally [Here](https://www.gog.com/en/game/quake_iii_arena), then copy the `.pk3` files from your
install/disc into the paths below

File structure:

```
E:\Games\ioquake3\default.xbe
E:\Games\ioquake3\ta.xbe                        (optional, Team Arena)
E:\Games\ioquake3\baseq3\pak0.pk3 ... pak8.pk3
E:\Games\ioquake3\missionpack\pak0.pk3 ... pak3.pk3  (optional, Team Arena)

E:\Games\openarena\default.xbe
E:\Games\openarena\baseoa\*.pk3
```

Any folder works, since the game data is read next to the XBE. OpenArena is
free and needs its own folder.

## Controls

### In-game

| Input | Action |
|---|---|
| Left stick | Move / strafe |
| Right stick | Aim (yaw/pitch) |
| **Right trigger** | Attack |
| **Left trigger** | Zoom |
| **A** | Jump |
| **B** | Crouch |
| **X** | Previous weapon |
| **Y** | Next weapon |
| **Black** | Use item |
| **White** | Walk / run |
| **D-pad** | Arrow keys |
| **Start** | Menu / pause |
| **Back** | Scoreboard |

### Menus

| Input | Action |
|---|---|
| Left stick | Move cursor |
| D-pad | Arrow keys |
| **A** | Confirm (Enter) |
| **B** | Back (Escape) |
| **Start** | Escape |

### Keyboard and mouse

USB keyboards and mouse work through a controller-port USB adapter.

---

## Memory budget

Built for the stock **64 MB** Xbox. 128 MB units are detected at boot, but the
budgets below are not scaled up yet.

| Region | Size | Notes |
|---|---|---|
| `com_hunkMegs` | 20 MB | Maps, shaders, collision, bots. Cut from 24 MB because native modules no longer need QVM hunk |
| `com_zoneMegs` | 8 MB (6 MB on Team Arena) | Dynamic allocs; the TA XBE image is ~4 MB larger |
| `com_soundMegs` | 1 unit (~3 MB) | Mono sounds are stored as ADPCM, so maps do not reload sounds in play |
| Texture pool | 6 MB | World lightmaps, models and large 2D art are DXT1/DXT5 compressed |
| Everything else | remainder | XBE image, heap, model data, cinematics |

The values are set in `code/sys/xbox_boot.c` (`XBOX_COM_HUNK_MEGS`,
`XBOX_COM_ZONE_MEGS`, `XBOX_COM_SOUND_MEGS`). All were retuned against
measured heap reports on hardware rather than guessed.

---
## Credits

- **[ioQuake3](https://github.com/ioquake/ioq3)** — the upstream engine this port is based on.
- **[nxdk](https://github.com/XboxDev/nxdk)** — the open-source Xbox toolchain, pbkit and xgu, and the Docker build image.
- **[OpenArena](https://openarena.ws/)** — the 0.8.8 gamecode and assets used by the OpenArena flavor.

---

## AI disclosure

Parts of this port were developed with the assistance of **Claude** (Anthropic). AI was used for code generation. All AI-generated code was reviewed and tested on hardware before inclusion.

---

## License

ioQuake3 is GPLv2. This port layer is also GPLv2. See `COPYING.txt`.
