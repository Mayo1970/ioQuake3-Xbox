# Internet server browser: diagnosis and status

Date: 2026-09-26. Scope: the Multiplayer (server browser) menu in all three XBEs: Q3 (`default.xbe`), Team Arena (`ta.xbe`) and OpenArena (OA `default.xbe`). All work below is uncommitted.

## 1. Current status

| Item | State |
|---|---|
| Menu freeze while master names resolve (all flavors) | Fixed, hardware SUCCESS |
| TA crash on Multiplayer open | Fixed, hardware SUCCESS |
| OA out of memory on Multiplayer open | Fixed, hardware SUCCESS |
| OA "could not resolve dpmaster.deathmask.net" | Fixed, hardware SUCCESS |
| OA master list | Correct: same as stock OA 0.8.8 |
| List empty on first open, needs a refresh (Q3, OA) | Fixed (section 9), hardware SUCCESS |
| TA never gets to the list (out of memory) | Fixed (section 9), hardware SUCCESS |

Last report (with DEBUG logs of all three flavors): "Team Arena won't get to the list anymore, and in both Open Arena and Quake 3 I need to hit Refresh (once) to make it fetch the internet lists."

## 2. Files changed for this work

| File | Change |
|---|---|
| `code/sys/xbox_net.c` | `Sys_XboxResolveAsync`: DNS on a worker thread, 8 cached slots, failures cached 30 s, 32 KiB thread stack, log lines on failure |
| `code/sys/sys_xbox.h` | Declares `Sys_XboxResolveAsync` |
| `code/client/cl_main.c` | `CL_XboxMasterAdr`, `cl_xboxPendingMasters`, `CL_XboxPollMasterLookups` (called at the top of `CL_Frame`), `cl_xboxMasterRerun`, `CL_XboxMastersPending`; changes in `CL_GlobalServers_f`; all over-long comments trimmed |
| `code/qcommon/common.c` | `Com_RealTime`: `gmtime()` fallback under `XBOX` |
| `code/client/cl_cin.c` | `CIN_RunCinematic`: frees the cinematic storage when a video ends on its own; comments trimmed |
| `code/sys/xbox_net.c` (section 9) | Lookup time in the DNS log lines; DNS servers in the `Xbox net:` boot line |
| `code/client/cl_main.c` (section 9) | `CL_XboxPrefetchMasters` at the end of `CL_Init` |
| `code/q3_ui/ui_servers2.c`, `oa/code/q3_ui/ui_servers2.c` (section 9) | `ArenaServers_DoRefresh` waits up to 15 s while the count is -1; comments trimmed |
| `code/ui/ui_shared.c` (section 9) | `Menus_Activate` also closes videos before `onOpen`; comments trimmed |
| `code/ui/ui_main.c` (section 9) | Server selection stops the old map video before the levelshot; no "have cached list" return |

## 3. How the async master lookup works now

1. The menu runs `globalservers <master> <protocol> ...`. Master 0 means "all": `CL_GlobalServers_f` queues `globalservers 1 ...` and `globalservers 2 ...` with `Cbuf_AddText`. They run in the same `Cbuf_Execute` pass.
2. For one master, `CL_XboxMasterAdr(address, &to)`:
   - `localhost` or a numeric address: plain `NET_StringToAdr`, no DNS.
   - A name: `Sys_XboxResolveAsync(host, &ip)`. It returns 1 with the IP, 0 on failure, or -1 while the lookup runs.
3. Result -1 (lookup running): the command text (`Cmd_Cmd()`) is saved in `cl_xboxPendingMasters[masterNum]`. The engine sets `cls.numglobalservers = -1` and `cls.pingUpdateSource = AS_GLOBAL`, then returns. The -1 tells the menus "waiting for the master".
4. On each `CL_Frame`, `CL_XboxPollMasterLookups` checks each pending master. When its lookup ends, the engine runs the saved command again with `Cmd_ExecuteString`, with `cl_xboxMasterRerun = qtrue`.
5. On a rerun, `CL_GlobalServers_f` sends the request but does **not** reset `cls.numglobalservers` to -1. A reset would make the next reply overwrite the servers that the other master already sent (`CL_ServersResponsePacket` restarts the list when the count is -1).
6. If a rerun fails, and the count is still -1 and no other lookup is pending, the count goes to 0. Without this, the TA browser waits forever: it has no timeout while the count is -1.

`Sys_XboxResolveAsync` details:
- It calls lwIP `getaddrinfo` (AF_INET, SOCK_DGRAM) in a `CreateThread` worker. lwIP `getaddrinfo` blocks its caller until DNS answers or times out (4 tries, up to ~10 s).
- The results stay cached for the session. A failed lookup answers "failed" from the cache for 30 s, then retries.
- Stack size: 32 KiB. `CreateThread(..., 0, ...)` would use the XBE stack size, which is 1 MiB (`-stack:1048576` in `Makefile.xbox`).
- Log lines (DEBUG build): `Xbox net: DNS lookup of <host> failed (<err>)` and `Xbox net: no thread for the DNS lookup of <host>`.

## 4. Bugs found and fixed

### 4.1 Menu freeze (all flavors)
- Cause: `NET_StringToAdr` on a master name calls lwIP DNS, which blocks the main thread for up to ~10 s per name. Q3 and TA have two master names.
- Fix: the async lookup (section 3).
- Hardware: Q3 SUCCESS (both masters answered, 512 servers).

### 4.2 TA crash on Multiplayer open
- Symptom: the log stops with no watchdog "stalled" line. The watchdog (`xbox_diag.c`) is silent only when the CPU faults, so this was a crash, not a hang. It is also the cause of the earlier TA "hang".
- Cause: the nxdk PDCLib `localtime()` and `asctime()` are stubs that always return NULL. `Com_RealTime` then left `qtime` uninitialized. The TA `UI_StartServerRefresh` (`code/ui/ui_main.c`) indexes `MonthAbbrev[q.tm_mon]` with that garbage value and crashes.
- Fix: `Com_RealTime` uses `gmtime()` when `localtime()` returns NULL. The Xbox clock is UTC, so the TA "Refresh Time" shows UTC.
- Not changed: `common.c` log-file code still calls `asctime(localtime())`. It runs only when the `logfile` cvar is set.

### 4.3 OA out of memory on Multiplayer open
- Symptom: `ERROR: Xbox ref: out of memory for 1048576 bytes` while decoding `menu/art_blueish/frame2_l.tga`. Only 304 KiB was free. The heap had grown by ~3 MiB since `Com_Init`.
- Cause: `cl_cin.c` allocates `cinStorage` (2.9 MiB, `malloc`) while a video plays. It was freed only in `CIN_StopCinematic`. OA's only video, `idlogo.RoQ` (in `pak6-misc.pk3`), plays to the end on its own. The end-of-file path in `CIN_RunCinematic` called `RoQShutdown()` but never freed the storage. Q3 hid the bug because its second video, `intro.RoQ`, was skipped, and a skip calls `CIN_StopCinematic`.
- Fix: `CIN_XboxReleaseStorage()` after `RoQShutdown()` in that path.

### 4.4 OA "could not resolve address of master dpmaster.deathmask.net"
- The PC resolves the name through the router (fritz.box, 192.168.178.1): 107.161.23.68.
- Probable cause (not proven): the lookup thread used the 1 MiB default stack, and only ~1.5 MiB was free after the leak in 4.3, so `CreateThread` failed.
- Fix: 32 KiB thread stack and the log lines in section 3. It worked on the next hardware run.

### 4.5 List needs several refreshes
- Cause 1: while a lookup ran, `CL_GlobalServers_f` returned before `cls.numglobalservers = -1`. The menus saw no wait and ended the refresh with 0 servers. The list arrived later, but the menu had stopped.
- Cause 2: with two masters, the rerun for the later master reset the count to -1. Its reply then overwrote the first master's servers.
- Fix: steps 3, 5 and 6 of section 3.
- Hardware: better ("a few" refreshes became one), but not solved. See section 6.

## 5. Master server facts

| Flavor | `sv_master1` | `sv_master2` | Protocol | Query sent |
|---|---|---|---|---|
| Q3 | `master.quake3arena.com` | `directory.ioquake3.org` | 68 (legacy) | `getservers 68 ...` |
| TA | same as Q3 | same as Q3 | 68 | `getservers 68 full empty` |
| OA | `dpmaster.deathmask.net` | empty | 71 | `getservers 71 ...` |

- The masters are set in `code/server/sv_init.c`. `MASTER_SERVER_NAME` is in `code/qcommon/qcommon.h`.
- The OA values match the stock OA 0.8.8 binaries (`openarena.exe`, `.i386`, `.x86_64` in `E:\Users\Matteo\Desktop\openarena-0.8.8`). Their strings hold only `dpmaster.deathmask.net`, and they send `getservers %s` with the protocol only.
- OA sets `GAMENAME_FOR_MASTER "Quake3Arena"` (legacy query format) and `PROTOCOL_LEGACY_VERSION 71` (`code/qcommon/q_shared.h`, `qcommon.h`). Protocol 71 is what separates OA servers from Q3 servers on the shared dpmaster.
- Menu sources: q3_ui and the OA ui send `globalservers <source-1> <protocol>`, so "Internet" is master 0 (all masters). The TA ui sends `globalservers <ui_netSource - UIAS_GLOBAL0> <protocol> full empty`.
- `servercache.dat`: only the TA ui loads and saves it (`LAN_LoadCachedServers` / `LAN_SaveServersToCache` in `cl_ui.c`). The file is in the home path root, `D:\`.

## 6. First open needs one refresh (history; the result is in section 9)

### What the menus do
- q3_ui and OA ui (`ArenaServers_DoRefresh`, `code/q3_ui/ui_servers2.c` and `oa/code/q3_ui/ui_servers2.c`):
  - `ArenaServers_StartRefresh` sets `refreshtime = uis.realtime + 5000`.
  - While `uis.realtime < refreshtime` and the count is < 0, the menu waits.
  - After 5 s the menu reads the count anyway. If it is still -1, `numqueriedservers = -1`, no ping goes out, `trap_LAN_GetPingQueueCount()` is 0, and `ArenaServers_StopRefresh()` ends the refresh with an empty list.
  - When the count is >= 0, the menu pings the servers and shows those that answer.
- `ArenaServers_SetType`: if the menu already holds servers, it shows them with "hit refresh to update" and does **not** refresh.
- TA ui (`UI_DoServerRefresh`, `code/ui/ui_main.c`): waits with no time limit while the count is < 0. `UI_StartServerRefresh(qtrue, qfalse)` skips the request when the engine already holds a server list.

### Hypotheses (ranked, not verified)
1. **The first lookup plus the master reply take more than the 5 s window** (q3_ui / OA ui). On a fresh boot the router or lwIP ARP may be cold. A lost first DNS query costs about 1 s more per retry. The second open, or a refresh, finds the IP in the cache, so the request goes out in the same frame, as on PC.
2. **A timing gap between the rerun and the menu.** The rerun runs at the top of `CL_Frame`. Check that the menu does not stop the refresh in the gap between the count becoming >= 0 and the first ping going out. Compare the time of `Requesting servers from` with the time the menu opened.
3. **TA only:** `UI_StartServerRefresh` returns early when `trap_LAN_GetServerCount() > 0`, for example with a stale list from `servercache.dat`. Then the menu shows the old list and does not fetch a new one.

### Get evidence first (next session)
1. Ask Matt for the DEBUG log of one fresh-boot first open, for each flavor that fails.
2. If the log has no timing, add temporary timestamps (`Sys_Milliseconds()`):
   - `Sys_XboxResolveAsync`: lookup start.
   - `Sys_XboxLookupThread`: lookup end, and the time it took.
   - `CL_GlobalServers_f`: the "pending" branch and the rerun.
   - The existing `Requesting servers from` and `CL_ServersResponsePacket` lines.
3. Compare them with the moment the menu opens. If lookup plus reply is over 5 s, hypothesis 1 is confirmed.

### Candidate fixes (not implemented, Matt must choose)
- **A. Look up the master names at boot.** After `NET_Init` / `CL_Init`, call `Sys_XboxResolveAsync` once for each non-empty `sv_masterN`. When the menu opens, the IPs are already in the cache, so the first request goes out at once, as on PC. This change is engine only and covers all three flavors. It is the most direct fix for "not instant". Risk: a lookup that fails at boot is cached as failed for 30 s. After that, a menu open tries again.
- **B. Make the Q3/OA menu wait longer while the count is -1.** In both `ui_servers2.c` files, move `refreshtime` forward while the count stays < 0, up to a cap. This changes the gamecode of two flavors.
- **C. Both A and B.** A makes the list instant in the normal case. B covers a slow first lookup.

## 7. Other open items (not acted on)
- Some DNS lookups still block the main thread:
  - `CL_RequestMotd` (`update.quake3arena.com`) and `CL_RequestAuthorization` (`authorize.quake3arena.com`) run on every connect in Q3 and TA.
  - A `connect` command given a hostname.
  - `rcon`, `ping` and `serverstatus` given a hostname.
  - `SV_MasterHeartbeat` (`sv_main.c`) when the Xbox hosts an Internet server.
- The watchdog thread (`xbox_diag.c`, marked DIAGNOSTIC) also uses `CreateThread(NULL, 0, ...)`, so it holds a 1 MiB stack for the whole session.
- The TA "Refresh Time" shows UTC. The dashboard time zone is not applied.
- The TA ui writes `servercache.dat` on every ui shutdown, including the Q3/TA hand-over. The file holds the full `cls.globalServers` array.

## 8. Build, deploy and test reference
- Build commands and flavor rules: see memory `xbox-docker-build`. Short form (Git Bash, repo root):
  `MSYS_NO_PATHCONV=1 docker run --rm -v "E:/Users/Matteo/Desktop/quake3/ioQuake3-Xbox/ioQuake3-Xbox:/src" -w /src xboxdev/nxdk:latest sh -c 'make -f Makefile.xbox -j8 [ta|oa] [DEBUG=y]'`
- For a DEBUG build, first delete `code/sys/sys_xbox.obj`. A flavor switch rebuilds all engine objects. The in-tree engine objects are now the TA flavor.
- Last builds (the refresh-race fix, section 4.5). The DEBUG builds are in the output folders:

| XBE | Release md5 | DEBUG md5 |
|---|---|---|
| Q3 `build_xbox/default.xbe` | `3848017bb6175679c5c1f89b1a2cffa1` | `b694ad85936cbe9e7fdb90e5e14ee2db` |
| TA `build_xbox_ta/default.xbe` (deploy as `ta.xbe`) | `d9b59b09ff911dcdb5ceef459967c66b` | `aa404ea1a7d4e032d9204bb04167513c` |
| OA `build_xbox_oa/default.xbe` | `315fba7f4792369f0cef5aa7590d28ff` | `de57b197bc8bb0815229cdd176be1d4a` |

- Deploy: Q3 and TA go in the same folder (`default.xbe` and `ta.xbe`). OA goes in its own folder.
- Logs: `D:\ioquake3.log`, `D:\ioquake3_ta.log`, `D:\ioquake3_oa.log` (DEBUG builds only).
- Test for the open issue: boot each XBE fresh, open Multiplayer with the Internet source, and do not press refresh. SUCCESS means the servers appear by themselves.
- The md5 table above is for the older builds. The current builds are in section 9.

## 9. Diagnosis of the last report and the fixes (Matt chose all three)

### 9.1 TA: out of memory when the browser opens
- Log: `ERROR: Xbox ref: out of memory for 1048576 bytes` while decoding `levelshots/pro-q3dm6.jpg` (512x512, baseq3 `pak6.pk3`). Available 940 KiB. No `Requesting servers` line.
- Chain:
  1. `servercache.dat` (the TA ui writes it at every ui shutdown) holds the list of the earlier run.
  2. `joinserver` `onOpen` runs `uiScript UpdateFilter`. `UI_StartServerRefresh` returned at "have cached list", then `UI_FeederSelection(FEEDER_SERVERS, 0)` decoded the levelshot of the first cached server (1 MiB RGBA).
  3. The main menu plays `mpintro.roq` (item `cinematic`). `Menus_Activate` runs `onOpen` first and `Display_CloseCinematics()` after it, so the 2.9 MiB `cinStorage` was still allocated. Proof: heap "mapped" grew by 3044 KiB after Com_Init, the exact `cinStorage_t` size.
  4. The ERR_DROP unloads the ui and returns to the main menu. Every open repeats it.
- Same pattern, not yet seen: a click on a server moves to the next levelshot while the old map video (TA maps have `video/<map>.roq`) still holds 2.9 MiB.
- Fix:
  - `ui_shared.c` `Menus_Activate` (XBOX): `Display_CloseCinematics()` also before `onOpen`. The call after `onOpen` stays, so a video started in `onOpen` still closes as before.
  - `ui_main.c` `UI_FeederSelection` FEEDER_SERVERS (XBOX): stop the old map video before `trap_R_RegisterShaderNoMip` of the levelshot.
  - `ui_main.c` `UI_StartServerRefresh` (Matt's choice): the "have cached list" return is `#ifndef XBOX`. TA asks the masters on every browser open, as q3_ui does. The list is empty until the servers answer the pings (the refresh resets the pings), as in Q3/OA.

### 9.2 Q3/OA: the first open still needs one refresh
- Evidence: each log has only one `Requesting servers from` line per master. A rerun and a later Refresh each print that line, so the lookup was still running when Matt pressed Refresh. The first lookup takes longer than the 5 s menu window plus the time to press Refresh.
- lwIP timing (nxdk `dns.c`): sends at about 0, 1, 2 and 4 s, then after ~7 s it moves to DNS server 2 or fails. A lookup that ends after 5 s means lost replies or a dead server 1. nxdk `nxNetInit` uses the dashboard manual DNS when it is set, else the DHCP DNS. Not proven which; the new log lines show it.
- Fix:
  - A: `CL_XboxPrefetchMasters` (end of `CL_Init`, after `SV_Init` registers `sv_masterN`) starts the lookups at boot. The menu then finds the IP cached, or a lookup already running.
  - B: q3_ui and OA ui `ArenaServers_DoRefresh` (XBOX): while the count is -1 on an Internet source, wait until `refreshtime + 10000` (15 s after the refresh start). lwIP worst case with two DNS servers is ~14 s.
  - DEBUG log lines: `Xbox net: ip=... dns=<server 1>,<server 2> link=up` at boot; `Xbox net: DNS lookup of <host> took <N> ms` or `failed (<err>) after <N> ms`.

### 9.3 Builds (2026-09-26, DEBUG builds are in the output folders)

| XBE | Release md5 | DEBUG md5 |
|---|---|---|
| Q3 `build_xbox/default.xbe` | `0d8afdc4b1e3009315eea926505fe8bb` | `6539bb0a11921f16e11b48d267a88d9b` |
| TA `build_xbox_ta/default.xbe` (deploy as `ta.xbe`) | `b3540485dcd448777569e37a110a2de8` | `ded5f41cf7a7d4b53bd4b79e2f90449b` |
| OA `build_xbox_oa/default.xbe` | `fb4a280aec9cd959aea3428fdf679b87` | `cb2d0542d215f0d5d8ab7c7f5f2f532f` |

- The in-tree engine objects are now the TA flavor.

### 9.4 Hardware test
1. Boot each XBE fresh. Go to Multiplayer with the Internet source. Do not press Refresh.
2. SUCCESS: the servers appear by themselves (Q3, OA, TA). TA must not show the out-of-memory error.
3. TA only: click some servers on TA maps (for example mpteam1, mpterra1) to load their levelshots and videos.
4. Send the three logs. Read the `dns=` boot line and the `took <N> ms` lines. A time near 7000 ms or more points to DNS server 1.

### 9.5 Hardware result (2026-09-26): SUCCESS on all three
- Boot line: `dns=46.101.64.175,1.1.1.1` (dashboard manual DNS). Lookups took 6156 to 6950 ms, near the lwIP switch to server 2 at ~7 s, so server 1 did not answer.
- Q3 and TA: the boot lookups ended before the browser opened, and the list came with no Refresh.
- OA: the lookup (6673 ms) ended after the browser opened; the 15 s wait held the refresh, and the list came with no Refresh.
- TA: `levelshots/pro-q3dm6.jpg` 512x512 decoded with no error; list arrived (512 servers).
- Not tested: clicks on servers with TA maps that have videos (the log shows only non-TA maps).
