# Design: co-presence multiplayer and cross-game bridges on the SDK (`sco.net`, `sc_ipc.h`, `sco::game::net`)

**Status: design for review, updated to the maintainer's directive of 2026-10-09. No code yet.** Phase 6 item ([Framework plan](../framework.md#phase-6-the-framework-grows)). This changes sco-core's and sc-offline's scope, which the maintainer reviewed and decided to proceed with on 2026-10-09 ([section 1](#1-scope-what-changes-and-what-doesnt)). The **co-presence** model below replaces the fork's dedicated-server approach, which depended on patches this project now permanently excludes ([section 2](#permanent-exclusions-never-ported)). Every PR in the plan ([section 6](#6-plan-pull-requests-in-order)) gets its own review.

## Why

A parallel fork of sc-offline (a download dated 2026-10-09, forked before sco-core was integrated) added co-op multiplayer between players running sc-offline, plus two bridges that put Titanfall 2 and a Minecraft-style voxel game inside Star Citizen over shared memory. It works by patching the game straight from one 2,166-line file (`multiplayer.cpp`) and a 3,743-line `build.cpp`, with calls into multiplayer scattered through the spawner, contracts, ammo, loadout, NPC, build, menu and console code.

That's the pattern sco-core exists to replace. Every mod that wants co-op would repeat the same 30 or so game addresses, its own UDP protocol and its own threads, and all of them break on every patch. The maintainer's decisions of 2026-10-09:

1. **Scope changes.** sco-core and sc-offline grow from "offline, single-player, never connects to anything" to allowing multiplayer between players running sc-offline.
2. **The bridges become optional plugins:** Titanfall 2 (TitanLink) and Minecraft.
3. **Everything is built as SDK services and abstractions in sco-core** for plugins to use. sc-offline's multiplayer becomes a built-in plugin on top of them.

The directive of 2026-10-09 then fixed the model. The fork's approach — a second game process booted as a dedicated server, joined with the game's own `connect` — is **dropped**: it depended on the patches this project now permanently excludes ([section 2](#permanent-exclusions-never-ported)). The model is **co-presence by ghost replication**:

- Each player runs their own ordinary offline game. No process is booted as a server, and the game's netcode, server and matchmaking are neither used nor altered.
- One player's client **hosts the session** (pure peer-to-peer; a headless broker is deferred to a v1.x release).
- Poses and spawn announcements travel over `sco.net`. Remote players and ships are spawned **locally as ghost entities** and moved with a new transform primitive (`spawn.entities` 1.2, section 3.4). Nothing of another player's game is trusted as code or pointers; only semantic fields cross the wire.

This document reads the fork's code (section 2), then designs:

- the sco-core side (section 3): a game-agnostic `sco.net` host service, the game-specific network rows and hooks (`sco::game::net`), a `sco.ipc` service for the bridges, and the engine helpers;
- the sc-offline side (section 4): `builtins/multiplayer`, how the domain features use it without hard dependencies, and the bridges as optional built-ins.

Section 5 covers the risks and section 6 the plan; the maintainer's directive resolved the earlier open questions, recorded at the end.

**Sources.** The fork is at `sc-offline-source-2026-10-09` (untrusted download, read only; nothing built or run). `F:` below means its `sc-offline/src/` folder. Lines are 1-based, from the files as downloaded. sco-core is at `main` `2196c5a`; sc-offline is at `main` `1f634c0`. A third-party handoff described the fork, and parts of it are wrong. It names `ExpectPlayer` and `net_staging`, which don't appear in `multiplayer.cpp`: the hooked function is the game's `CNetNub::ExpectIncomingConnection` and the variables are `pl_staging.*`. It also names `HookEngine::Instance`, `ZoneManager`, `SCO_PLUGIN_DEFINE` and `GetPluginSdk().QueryService`, none of which exist in sco-core. This design works from the code only.

## 1. Scope: what changes and what doesn't

### The new rules

Proposed wording. It replaces the scope paragraphs of sco-core's `README.md` (§ Scope), `CONTRIBUTING.md` (§ What fits) and `sdk/docs/plugin-rules.md`, and sc-offline's `README.md` and `CONTRIBUTING.md` (§ What fits this project). It lands in plan PR 1, before any code.

> **Scope.** sco-core and sc-offline are for playing Star Citizen offline: alone, or **with other players who also run sc-offline, in a session one of them hosts**. Since 2026-10-09 (maintainer's decision) this is allowed:
>
> - **Private sessions between sc-offline players.** One player hosts a session on their own PC (their own running game; no dedicated server process is booted); others join it by address over a LAN or a VPN they choose. Every peer runs sc-offline with the game offline. Sessions go through sco-core's `sco.net` service and its rules (an explicit join, a session passphrase, limits).
> - **Local links to other programs on the same PC,** such as another game for a cross-game bridge, through sco-core's `sco.ipc` service (named shared memory in the user's own session, nothing over the network).
>
> This stays out of scope, and PRs or plugins that do it are closed:
>
> - Connecting to Cloud Imperium Games' services or servers in any way (login, game servers, the launcher's backend, the website's APIs), or making the game talk to them.
> - Bypassing or weakening anti-cheat, signature checks, or integrity checks. That includes running a session where such a check would otherwise refuse.
> - Anything that helps cheating in the official game, or that could carry an advantage into it: no memory readers or overlays for live play, no transferring items, money or progress to an online account.
> - Telemetry, analytics, master servers, public server lists, matchmaking or relays run by the project. Sessions are found by sharing an address; nothing reports them anywhere.
> - Collecting or sending personal data. A session shares the player names players choose, nothing else.

Plugin rules gain three lines: a plugin may use `sco.net` and `sco.ipc`, and only them, to talk to other processes or machines. Raw sockets, HTTP clients and their own shared memory are not allowed: a native plugin can technically open them (it runs with the game's rights), so this is a review rule, not a sandbox. Lua plugins get `sco.net` messages inside their sandbox, and no `sco.ipc`.

### Risks this decision brings (for the maintainer to weigh)

- **Game-patch fragility.** Co-presence needs only a few **benign** engine points (session connect, the main tick, the fly-mode validation toggle for the local player's own noclip), far fewer and shallower than the fork's ~30, and none of the excluded patches. They still move: `teleport.to_camera` held for several builds and still moved in 4.10.196. So they are signature rows kept honest by `sco-sigcheck` **each patch**, never assumed stable, and a broken row switches off only its own capability (section 3.2).
- **Terms of service.** Letting players connect their offline games to each other may raise questions under CIG's terms that offline single-player didn't. The maintainer reviewed this and **decided to proceed**; this document takes no legal position. Where code lives (sco-core, a built-in, an optional plugin, another repository) is an architecture boundary only: it changes nothing about the terms-of-service question or about what the scope rules allow.
- **Security exposure.** sc-offline today never listens on a port. With multiplayer, a player's PC accepts packets, and the fork's protocol has no authentication (section 2, "Security findings"). That is the main reason for a host-owned service with secure defaults instead of per-mod sockets.
- **Permanent exclusions.** The fork's dedicated-server path relied on patches touching anti-cheat, account entitlement, host-type and network context, plus an unauthenticated remote console. These are **permanently excluded** from every repo, header and official plugin, named in [section 2](#permanent-exclusions-never-ported). The co-presence model does not need them; nothing here restores or works around them.
- **Support load.** Multiplayer bugs need two or more machines and logs from each.

## 2. What the fork's `multiplayer.cpp` does

*This section documents the fork for reference. The design in section 3 does not reproduce its dedicated-server model, and section 2's [Permanent exclusions](#permanent-exclusions-never-ported) are never ported.*

### Session model

| Role | How it starts | What it is |
|---|---|---|
| Dedicated server | The fork's launcher `host` command starts a second `StarCitizen.exe` with `SC_OFFLINE_DEDICATED=1`, its own `SC_USER` folder and `data\mod-server.log` (fork `launcher/launcher.cpp:4233`, `:4325`) | The game client booted as a dedicated game server: `StartupFillerHook` sets `params[0x41] = 1` (`F:multiplayer.cpp:400-404`). The mod thread never reaches the menu: it loops `ProcessDedicatedServer()` every 100 ms (`F:dllmain.cpp:203`), and game-thread work runs from the `CCryAction::PostUpdate` hook (`F:multiplayer.cpp:1254-1259`) |
| Client | `join <ip>` sets `SC_OFFLINE_JOIN=<ip>`, `SC_OFFLINE_NAME`, and optionally `SC_OFFLINE_JOIN_PORT` (launcher `:4305-4306`; `F:multiplayer.cpp:335-339`, `:2130-2133`) | An ordinary sc-offline game that, once in its menu, asks the server to expect it and then runs the game's own `connect` |
| Hosting player | `host` starts the server, then the player's own game joins `127.0.0.1` (launcher `:4233-4234`) | A client of their own server |

There is no listen-server mode. Only a dedicated process answers `SCOHELLO` (`F:multiplayer.cpp:253`). A client that reaches a non-dedicated host logs that ship spawns and contracts stay local (`:279`). The server keeps at most 16 joiners and never forgets one (`:138-157`). Players get a per-player database file copied from `data/OfflineDB/default_1.xml` (`EnsurePlayerData`, `:343-356`). Their names come from `SC_OFFLINE_NAME` or the Windows user name, written into the game's `default_%` name template (`UseOwnPlayerName`, `:358-385`).

### Join sequence

1. The client's own `CNetNub::ExpectIncomingConnection` call is intercepted to capture its session id, node id, player entity id (`+0x18`) and nickname (`+0x28`) (`ExpectHook`, `:126-136`).
2. The game's session state reaches 3 or 4, then 5 s pass and the menu settles: the universe layout has been asked for, or 60 s have passed (`ProcessMultiplayer`, `:1293-1326`; `MenuSettled`, `:1710`). The client then sends `SCOHELLO` every 2 s (`:1314-1321`). F9 restarts the attempt (`:1312-1313`).
3. The server builds `SIncomingConnectionParams` for the joiner and calls the game's own `ExpectIncomingConnection` on the server nub, captured from `CNetNub::CreateSocketGroups` (`ExpectJoiner`, `:167-185`). It answers `SCOOK` with its player id, game port and a dedicated flag (`:262-268`).
4. The client starts a thread that fetches the universe layout over TCP (`LayoutOnConnect`, `:1695-1701`). It then calls the game's `connect <host> <port>` through the console-command function it found (`Connect`, `:1275-1280`).
5. When the game asks for the layout, the fetched blob is fed to `HandleSvRecvUniverseHierarchyData` graph by graph (`LayoutTick`, `InjectLayout`, `:1669-1708`).
6. Once the client has been in the world for 20 s, it sends `SCOREADY` (`TellServerInWorld`, `:1925-1940`). The server then starts showing it the other players (`:289-296`, `:2010-2064`).

### Hook points

Every one is found by a string in `.rdata` plus a prologue check, or by a unique `.text` pattern (`FindUniquePattern`). The anchors, as the fork uses them:

| Purpose | Anchor (string or pattern start) | Patch | Where | Role |
|---|---|---|---|---|
| Frame tick | `"CCryAction::PostUpdate"`, prologue `40 55 53 56 57 41 56 48 8D AC 24` | 7-byte detour; runs `ServerFrame`, `MinecraftFrame`, `TitanfallFrame` | `:1254-1265` | Both |
| Expect a connection | `"void __cdecl CNetNub::ExpectIncomingConnection(const struct SIncomingConnectionParams &)"`, prologue `48 89 5C 24 20 55 56 57 41 54 ...`; the params copy is the call after `48 8D 4E 28 E8` within 0x600 bytes | 16-byte detour; the original is also called directly | `:1348-1356` | Both |
| Server nub | `"CNetNub::CreateSocketGroups"`, prologue `48 8B C4 55 56 48 8D 68 D8 ...` | 9-byte detour, captures `this` | `:1357-1361`, `:162-165` | Server |
| Connect | `"[CSessionManager::ConnectCmd] Connect started!"`; the function is at the `lea` minus 0x22, checked; the framework global and the session-manager getter are read from it | Called, not patched; session state at `+0x30` | `:2134-2141`, `:1267-1273` | Client |
| Dedicated boot | `FF 15 ?? ?? ?? ?? 4C 8D 05 ...`, then the startup-settings filler with `C6 41 38 01` | 10-byte detour; sets the dedicated flag | `:476-485` | Server |
| Stand-in services hub | `FindServicesObject` (`F:services.cpp:153`) | Detour on its first user; swaps in the hub slot of sc-offline's existing offline stand-in | `:407-417` | Server |
| Server-only features | `"CSCPlayerMarkerSubscription::RestoreMarkerStates"`, `"AddMarkerToSubscriptionService"`, a quantum-travel-group pattern, two mission-entity class-switch patterns | Jumps over dedicated-server checks; one detour (markers become streamable) | `SkipServerFeatures`, `:448-474`; `:428-437` | Server |
| Host-type forcing | — | **Permanently excluded** ([see exclusions](#permanent-exclusions-never-ported)); not described here | `:1366` | Server |
| Network-context override | — | **Permanently excluded** ([see exclusions](#permanent-exclusions-never-ported)); not described here | `:1399` | Server |
| Anti-cheat step | — | **Permanently excluded** ([see exclusions](#permanent-exclusions-never-ported)); not described here | `:1417` | Server |
| Joiner entitlement/account | — | **Permanently excluded** ([see exclusions](#permanent-exclusions-never-ported)); not described here | `:1434` | Server |
| Layout send | `80 3D ?? ?? ?? ?? 00 48 8B DA 48 8B D1 75 45 ...` and three calls found inside the send function | Code stub; packs the server's universe graph | `SendLayoutToJoiners`, `:1712-1756`; `PackLayout`, `:1504-1547` | Server |
| Layout receive | `"...CStandaloneEntityGraph::HandleSvRecvUniverseHierarchyData(...)"`, `"CStandaloneEntityGraph::RequestOCHierarchyData"` | Two detours | `:1769-1781` | Client |
| Loading wait | `48 8B C8 E8 ?? ?? ?? ?? 84 C0 74 23 48 8B 0D ...` | Code stub; gives up waiting after 20 s | `RelaxJoinerLoadingWait`, `:1806-1831`; `:1786-1804` | Client |
| Staging | `"SSCActorStateCVars::LogStaging"`, the CVar names `pl_staging.debug`, `pl_ground.correctLinked`, `pl_performance.lods.overrideClientActorPerFrameBudget` | A detour; the CVars are held at fixed values (`pl_staging.forceClientValidation = 1` among them) | `:1851-1917` | Client |
| Validation wait | `"Movement state mismatch. Local is [%s], remote is [%s]. Forcing staging exit."` | Rewrites a data record's wait from 60 s to 15 s | `:1885-1891`, `:1910-1916` | Client |
| Fly-mode check | `"Fly mode mismatch. Local is [%s], remote is [%s]."` | Detour that skips the check while noclip is on | `:815-818`, `:2121-2126` | Client |
| Net timeout | `4C 8B DC 57 48 81 EC 30 01 00 00 ...` | Holds the network inactivity timeout at 300 s or more | `:1178-1197` | Both |
| Who sees what | The bind-to-player call pattern `48 8B 0E 4C 8B 47 08 ...`; `"CReplicationModel::NetworkTickPreUpdate"`; `"New IndependentlyStreamableEntity registered. ..."` | Two detours; binds entities and players to each joiner every 1 s | `ShowPlayersToJoiners`, `:2076-2092`; `:2010-2074` | Server |
| Player name | `"default_%"` in `.rdata` and the pointer slot that references it | Rewrites the slot to the mod's own template | `:358-385` | Client |

`ResolveMultiplayerApi` (`:2094-2142`) wires them per role. `LogMultiplayer` (`:2144-2165`) prints one `[!]` line for each piece that's missing and says what fails because of it. The code stubs are written into `VirtualAlloc(PAGE_EXECUTE_READWRITE)` pages (`NewStub`, `:1390-1394`).

### Permanent exclusions (never ported)

These fork pieces are **permanently excluded** from every sco-core and sc-offline repository, every shipped header, and every official plugin. They are named here with their fork location so a reviewer can recognise and reject them; this document does **not** describe how any of them work, and no PR in the plan touches them. There is no research task to make a dedicated-server join work without them: that path is dropped with the dedicated-server model.

| Excluded | Fork location |
|---|---|
| Skipping the anti-cheat step | `F:multiplayer.cpp:1417` |
| Joiner entitlement / account tampering | `F:multiplayer.cpp:1434` |
| Host-type forcing | `F:multiplayer.cpp:1366` |
| Network-context override | `F:multiplayer.cpp:1399` |
| Unauthenticated remote console (`Cmd_Console`) | `F:multiplayer.cpp:980-1043`, dispatch at `:1032-1036` |

The co-presence model (section 3) reaches none of these: it never boots the client as a server, never alters the game's connection, host-type or account handling, and carries no remote-command channel. A plugin or PR that reintroduces any of them is out of scope and closed.

### Transport and packets

| Port | Proto | Who listens | What |
|---|---|---|---|
| 64090 (client), 12300 (server); `NETWORK_SERVER_MESH_GAME_BIND_PORT` overrides | UDP | The game itself | The game's own network traffic (`GamePort`, `:114-119`); the server's range is 12300-12311 (`:2152`) |
| 64091 | UDP | Server; clients bind an ephemeral port | The mod's side channel (`kHelloPort`, `:43`; `OpenSocket`, `:187-197`): non-blocking, polled |
| 64091 | TCP | Server | The universe layout (`kLayoutPort`, `:44`; `LayoutServer`, `:1614-1627`). One thread per connection |

The launcher opens these with a firewall rule (`netsh ... dir=in action=allow profile=any`, UDP 12300-12311 and 64091, TCP 64091, launcher `:1987-1994`). The rule has no remote-address scope.

Every side-channel packet starts with an 8-byte magic. Packed structs are sent raw, little-endian. There's no version field, no checksum and no authentication:

| Magic | Direction | Size | Carries | Code |
|---|---|---|---|---|
| `SCOHELLO` | c -> s | 160 | session, node id, player id, nickname | `:57`, `:322-333`, `:253-269` |
| `SCOOK` | s -> c | 16 | server player id, game port, dedicated flag | `:262-268`, `:270-282` |
| `SCONAMEQ` / `SCONAMER` | launcher -> s -> launcher | 8 / 8+name | a player name for this IP from `names.txt`, else `client<n>` | `:213-243`, `:287` |
| `SCOSPAWN`, `SCOATCRQ` | c -> s | `SpawnRequest` | spawn a ship (and seat me), or retrieve my ship from a spaceport's ATC | `:487-495`, `:507-551` |
| `SCOSPWND` | s -> c | `SpawnReply` | status text, ship id, flag "press Flight Ready" | `:496-501`, `:552-560`, `:572-590` |
| `SCOREADY` | c -> s | 16 | I've been in the world for 20 s | `:289-296`, `:1925-1940` |
| `SCOCMD` | c -> s | `CommandPacket` (sequence, command, flags, player, zone, position, rotation, value, target, 200 chars of text) | noclip, god, ammo, spawn (entity or `.socpak`), undo, clear, seat actions, **console command** | `:83-95`, `:99`, `:787-805`, `:980-1043` |
| `SCOGEAR` | c -> s | 16 + XML up to 60,000 bytes | my loadout | `:1055-1066`, `:1072-1139` |
| `SCOPLAYQ` / `SCOPLAYR` | c -> s / s -> c | 8 / 9 + 16 x 40 | the player list, asked every 3 s | `:902-933` |
| `SCOOFFER` | c -> s | up to 1,908 | a contract offer from the joiner's game | `:523-531`, `F:contracts.cpp:2975`, `:2980` |
| `SCOOBJ` | s -> c | up to 1,640 | a contract objective: mission GUID, **vtable RVA, 272 raw bytes of the game object**, texts, up to 16 marker ids | `:61-74`, `:700-745`, `:765-776` |
| `SCOEND` | s -> c | `MissionEndPacket` | mission ended | `:75-82`, `:747-763` |
| `SCOLAYQ1` -> size + `SCOLAYT1` blob | TCP | up to 512 MiB | the universe hierarchy: graphs of 88-byte nodes | `:1594-1658` |

### What gets replicated, and by whom

Player and ship transforms are not sent by the mod. **The game's own netcode replicates them**, server-authoritative, once the client is connected. The mod's job is getting the game to accept peers and to show them to each other:

| Thing | How | Code |
|---|---|---|
| Players, ships, NPCs, markers seeing each other | Every second, on the game's replication tick, each streamable entity the mod noted (spawned ships and entities, mission markers, entities the game registers) and every other ready player is bound to each joiner with the game's bind-to-player call. Players are shown 30 s after they exist; entities missing 10 times are dropped | `BindStreamablesToJoiners`, `:2010-2064`; `NoteStreamable`, `:1995-2002` |
| Bodies and gear | 3 s after a player is first bound to someone, the server re-equips their last loadout, so others see it | `RedressShownPlayers`, `:1141-1150` |
| Ship spawns | Client spawns go to the server, which spawns there (so every peer sees the same entity id). Optional seat job: up to 8 seat requests, 3 min timeout, then power-up or "press Flight Ready" | `ServerFrame`, `:1199-1249`; `UpdateServerSeatJobs`, `:620-660`; `F:spawner.cpp:1399`, `:1550`, `:1563`, `:1582` |
| ASOP / fleet retrieval | `SCOATCRQ`: the server runs the ATC request for the joiner | `F:hooks.cpp:458`, `:487-488` |
| NPCs and build pieces | `Cmd_Spawn` with kind NPC or Build; the server keeps up to 256 ids per player and kind for undo and clear | `:964-978`, `:999-1018`; `F:npc.cpp:159`, `:203-212`; `F:build.cpp:397-400`, `:476-481` |
| Seating and crew | `Cmd_Seat` -> `ServerSeatAction` | `:1020-1031`; `F:spawner.cpp:1669`, `:1686` |
| Noclip, god mode, ammo | Desired states sent when they change and every 5 s; the server enforces them on its tick | `:807-849`, `:1152-1174`; `F:ammo.cpp:20`, `:161`, `:171`; `F:spawner.cpp:1788`, `:1813`, `:1875`, `:1932` |
| Loadout | `SCOGEAR` XML written to `SC_USER` and loaded for that player | `PutGearOn`, `:1096-1119`; `F:loadout.cpp:214`, `:229-230` |
| Contracts | The server runs mission scripts once someone has joined. Objectives and mission ends go to the owning player (resent twice at 5 s and 15 s); joiners' offers come to the server; wallets saved every 10 s | `:1236-1248`; `F:contracts.cpp:226-271`, `:1314`, `:1380-1383`, `:1398`, `:2975`, `:3107`, `:3211` |
| Console | `Cmd_Console`: any text runs on the server's console | `:864-866`, `:1032-1036`; `F:cvars.cpp:224` |
| Universe layout | TCP blob, injected into the client's entity graph | above |

### Threading

- **Server.** The mod thread polls the UDP socket every 100 ms (`F:dllmain.cpp:203`, `:1289-1291`) and queues spawns, commands and gear under one SRW lock (`g_spawnLock`). The game thread drains the queues in `ServerFrame` from `PostUpdate`. The replication tick hook runs on whatever thread the game calls it from (`:2066-2074`). The TCP layout server has an accept thread plus one thread per connection (`:1594-1627`). The layout is packed from inside the game's own send path (`SendLayoutTo`, `:1549-1570`).
- **Client.** The game thread (through the message-hook tick, `F:dllmain.cpp:150`) polls the socket, sends and runs the join state machine. A worker thread fetches the layout. The player list is under an SRW lock, and "go to player" is an interlocked id (`:871-933`).
- **Races.** `g_joiners` and `g_joinerCount` are written on the mod thread (`RememberJoiner`, `:142-157`) and read without a lock on the game thread and the replication thread (`:667-671`, `:2018-2031`).

### Failure handling

- Every hook that isn't found logs `[!]` with its consequence and leaves that piece off. Calls into game code sit in `__try/__except` and log the fault.
- **Retries.** Hello every 2 s until answered, and F9 by hand. Layout fetch up to 200 times, 3 s apart. Seat requests 8 times. Objective and mission-end packets are sent 3 times at most, with no acknowledgement. Desired states are resent every 5 s.
- **Timeouts.** The game's inactivity timeout is raised to 300 s so slow loads aren't dropped. The loading-screen wait gives up after 20 s, the spawn validation wait is cut to 15 s, and the seat job stops after 3 min.
- **What's missing.** There's no peer timeout or leave: a joiner stays in the table and the 16th is the last. There's no version check between peers.

### Security findings (the reason for section 3's defaults)

1. **No authentication.** Anyone who can reach UDP 64091 on a server can send commands. `QueueCommand` (`:939-949`) and `QueueGear` (`:1072-1085`) only check that the claimed player id is a known joiner, and never compare the source address. Player ids show up in the player list (`SCOPLAYR`), which anyone can ask for (`:301-302`).
2. **Remote console.** `Cmd_Console` runs any text on the server's console (`:1032-1036`).
3. **Raw game objects from the network.** A client builds a game objective from a network-supplied **vtable RVA and 272 raw bytes**, which include pointers that belong to the server's process (`F:contracts.cpp:279-293`). A malicious or buggy host can point a client's objective at arbitrary code in the game module. This must never be ported: peers exchange semantic fields, and each side builds its own objects with game calls.
4. **Files from the network.** Gear XML up to 60 KB per message is written to `SC_USER` (`:1096-1119`). `names.txt` grows by one line per new IP (`:213-243`).
5. **The TCP layout server** starts a thread for every connection, and each thread can wait 15 minutes. That's an easy way to exhaust threads.
6. **The firewall rule** allows every remote address on every network profile (launcher `:1987-1994`).

## 3. sco-core design

### 3.1 `sco.net` 1.0: a session and typed channels, game-agnostic

A host-owned service like `sco.storage`: published under the reserved id `sco`, found with `query_service`, `sco_api.h` unchanged. The table is `sco_net_v1` in `include/sco_net.h`, pinned by `tests/abi_net.c`. It follows our conventions, not a C++ class: first field `uint32_t size`, plain C function pointers (**no virtuals, no `IScoNetService`**), `int` returns rather than `bool`, `uint32_t` sizes rather than `size_t`, and **ids, never pointers** ([lesson 6](../framework.md#6-services-hand-out-ids-never-pointers)). A blueprint that suggested a virtual `class IScoNetService` with an inline `char name[32]` and `size_t` is **not** used: that would break the ABI. The peer name comes from a sized copy (`get_peer_name`), not an inline array.

**Capability.** The host sets the capability `sco.net` (through `caps::Set`) whenever the service is published, so a plugin manifest with `requires = sco.net` validates in discovery (`src/plugins/discover.cpp:118-119` checks each `requires` against `opts.has`). A plugin that needs multiplayer is refused with a clear reason on a build without it, instead of querying a NULL table.

**Who controls the session.** Hosting, joining and leaving are **the product's** decisions, from the launcher or the menu, through the host-side C++ API `sco/net.h` — not the plugin table. sc-offline's `builtins/multiplayer` calls it; built-ins may read internal headers. Plugins see whether a session is active, its peers, and their own channels, and cannot open or join one. That is least privilege: a third-party plugin can't make a player's PC listen.

```cpp
// include/sco/net.h (host side, C++; sketch). Co-presence: no dedicated server.
namespace sco::net {
struct HostOptions { uint16_t port = 64091; std::string passphrase, playerName;
                     uint32_t maxPeers = 8; BindScope bind = BindScope::Lan; };
struct JoinOptions { std::string address; uint16_t port = 64091; std::string passphrase, playerName; };
Result Host(const HostOptions&);     // this player's own game becomes the session host (P2P)
Result Join(const JoinOptions&);     // join another player's game by address
void   Leave(const char* reason);
void   SetPeerEntity(PeerId, uint64_t entityId);   // the ghost entity this product spawned for a peer
}
```

**The plugin table.**

```c
/* include/sco_net.h: service "sco.net", version 1.0 */
#define SCO_NET_NAME          "sco.net"
#define SCO_NET_VERSION_1_0   0x00010000u
#define SCO_NET_MAX_PEERS     16u
#define SCO_NET_MAX_UNREL     1200u           /* bytes in one unreliable datagram (poses) */
#define SCO_NET_MAX_RELIABLE  (256u * 1024u)  /* bytes in one reliable message (announcements) */

/* register_channel flags */
#define SCO_NET_RELIABLE   0x1u   /* default is unreliable; set for spawn/announce channels */
#define SCO_NET_FROM_HOST  0x2u   /* accept only when the sender is the session host */
#define SCO_NET_TO_HOST    0x4u   /* send only to the host */

/* A peer in the session. Ids are opaque; entity_id is session-scoped, 0 if streamed out. */
typedef struct sco_net_peer { uint64_t peer_id; uint64_t entity_id; } sco_net_peer;

/* Channel callback: runs on the game thread during the host-kit tick. buf valid for the call only. */
typedef void (*sco_net_on_message)(uint64_t sender_peer_id, const void* buf, uint32_t len, void* ctx);

typedef struct sco_net_v1 {
    uint32_t size;   /* sizeof(sco_net_v1) as the host built it */
    uint32_t _pad;
    /* 1 while a session is up (hosting or joined), else 0. Any thread. */
    int (*is_active)(void);
    /* Writes up to max peers (self included) to out and returns the count, or the count needed
     * when max is too small (out NULL with max 0 asks the count). Any thread. */
    int (*get_peers)(sco_net_peer* out, uint32_t max);
    /* Copies the peer's chosen name into buf, UTF-8, NUL-terminated, cut to cap. 1 on success,
     * 0 for an unknown peer or a NULL/zero buffer. Any thread. */
    int (*get_peer_name)(uint64_t peer_id, char* buf, uint32_t cap);
    /* Sends len bytes on channel_fqn ("<this plugin id>.<channel>") to the other peers (or only
     * the host when the channel has SCO_NET_TO_HOST). 1 when queued; 0 for no session, a channel
     * outside this plugin's id, len over the channel limit, or the plugin's send-rate quota hit.
     * Copies buf. Any thread. */
    int (*send_channel)(sco_plugin* self, const char* channel_fqn, const void* buf, uint32_t len);
    /* Registers "<this plugin id>.<channel>" with flags and a per-message byte limit; cb runs on
     * the game thread for each delivered message. 1, or 0 for a bad/taken name, a name outside
     * this plugin's id, or bad flags. Any thread. */
    int (*register_channel)(sco_plugin* self, const char* channel_fqn, uint32_t flags,
                            uint32_t max_len, sco_net_on_message cb, void* ctx);
} sco_net_v1;
```

`self` is passed to `send_channel` and `register_channel` the way every `sco_api` self-call is, so the host can enforce ownership and quotas.

**Channels.** A channel name is `"<plugin id>.<channel>"`. The host **refuses a channel outside the caller's plugin id**, so one plugin can't send or register on another's. Names need no central numbering; at join the host and peer exchange the channels each has registered, and `send_channel` to a channel the other side hasn't registered is a no-op counted as refused. Each plugin has a **send-rate quota** per peer (bytes/s and messages/s); over it, `send_channel` returns 0 and the message is dropped. Every received length is checked against the channel's `max_len` before any reassembly allocates.

**Co-presence traffic.** `builtins/multiplayer` registers `multiplayer.pose` (unreliable, the local player's zone-relative pose, sent a few times a second) and `multiplayer.spawn` / `multiplayer.despawn` (reliable announcements: a ghost class and its session-scoped entity id). A receiver spawns the announced entity **locally as a ghost** and moves it each pose with the new transform primitive (section 3.4). Domain built-ins register their own channels the same way (section 4.2). No raw game objects, pointers or code addresses ever cross the wire.

**Delivery and limits.**

- **Unreliable** (default): at most 1,200 bytes, one datagram, may be lost or reordered; for poses, which are resent anyway.
- **Reliable** (`SCO_NET_RELIABLE`): at most 256 KiB, fragmented, exactly once and in order per channel and peer; for announcements.
- Peers: 16 per session cap, `maxPeers` default 8. Per-plugin, per-peer reliable queue capped (then `send_channel` returns 0). A peer over its receive rate or sending unknown channels past a threshold is disconnected with a reason.

**Events** on the existing bus, game thread, each `data` starting with `size`: `net.state` `{ size, active, reason[128] }` and `net.peer` `{ size, what (JOINED|LEFT|ENTITY), peer }`.

**Threads.** The host owns the one UDP socket and one network thread (receive, acks, resends, keepalive, 30 s peer timeout), the handshake, the channel tables, queues and quotas. It hands delivered messages to the game thread, drained in `sco::app::Tick` before `tick` subscribers, as guarded callouts per plugin (a faulting callback disables only that plugin). `send_channel` works from any thread and copies. On unload or crash the host drops the plugin's channels and queued messages.

**Transport.** One UDP port (64091 by default) carries everything; there is no second TCP channel and no universe-layout transfer (the co-presence model doesn't move the game's universe graph). The packet framing, authentication and the passphrase handshake are in [section 3.5](#35-session-crypto-d4). `BindScope::Lan` refuses handshakes from public addresses (RFC 1918, link-local, loopback, and CGNAT `100.64.0.0/10` for VPNs are allowed); `Any` is an explicit opt-in. sc-offline's firewall rule is `remoteip=localsubnet` by default, with VPN ranges added when the player picks "VPN". Nothing discovers or reports sessions.

**Language layers.** All four wrap the one C table.

- **`scosdk/net.hpp`:** a `Net` handle over `ServiceRef<sco_net_v1>`; `Channel<T>` registered with a lambda, `Send(const T&)` for trivially copyable `T` and `Send(std::span<const std::byte>)`; `Peers()` as a small vector. `noexcept`, `sco_result`-returning, like the rest of the SDK.
- **`Sco.Sdk` (C#):** a `Net` class with `RegisterChannel(name, flags, maxLen, Action<ulong, ReadOnlySpan<byte>>)` and `SendChannel(name, ReadOnlySpan<byte>)`; AOT-safe, pinned in `Pins.cs`.
- **sco-lua:** `sco.net` in the sandbox **only when the manifest `requires = sco.net`**, and limited to the **peer list and pub/sub**: `is_active()`, `peers()` (a list of `{peer_id, entity_id, name}`), `register(channel, {reliable, from_host, to_host, max_len}, fn)` with `fn(sender, bytes)`, and `send(channel, bytes)`. No session control. Calls and delivered messages count against the step budget; the sandbox stays closed (typed messages in a product-opened session only, never a socket or address).

**Tests.** `sco::net::Core` is a class with no globals, so one binary runs **two or more cores in one process** on `127.0.0.1` with ephemeral ports, plus an in-memory transport that drops, duplicates and reorders from a fixed seed, driven by `Pump(nowMs)` with a virtual clock (no sleeps): handshake (right/wrong passphrase, wrong protocol version, address spoofing), exactly-once ordered reliable delivery under loss, fragmentation at the limits, quotas, peer timeout, channel-ownership refusal, direction flags, and `send` from 8 threads under TSan. The service binding is tested with **two `sco-host-sim` processes on loopback** (new `--net-host` / `--net-join` / `--net-pass` / `--until-event --timeout-ms` options and a `sdk/examples/net_echo` plugin). `tests/abi_net.c` pins the table; a C# pin goes in `Pins.cs`. The socket layer is `src/net/socket_win.cpp` and `src/net/socket_posix.cpp`; CONTRIBUTING's Windows-headers list grows by the first in the same PR.

### 3.2 `sco::game::net`: a few benign engine points

Co-presence needs almost nothing from the engine's networking: each player's own game runs normally, ghosts are spawned locally and moved locally, so the fork's dedicated-server, universe-layout, visibility-binding and joiner rows are **not** ported — they belong to the dropped model and to the [permanent exclusions](#permanent-exclusions-never-ported). What remains is a short, benign set in `src/game/net_sigs.cpp` (hooks in `src/game/net_hooks.cpp`, accessor and switches in `include/sco/game/net.h`), following the `sco::game::pak` decision that the engine adapter lives in sco-core. Rows move byte for byte from the fork in a first commit, then any 4.10.196 fix in a second ([Adding a signature](../adding-signatures.md)).

| Capability | Row | What it is for | From |
|---|---|---|---|
| `game.frame` | `game.post_update` (`CCryAction::PostUpdate`, prologue check) | A per-frame tick, so ghost poses interpolate smoothly between the 10 Hz `tick`; read-only | `:1261-1265` |
| `net.session` | `session_state` (the `CSessionManager` state read, `connect_cmd` anchor), `local_params` (the local player's own session id, entity id `+0x18` and name `+0x28`) | Reads the **local** player's in-world state and identity to drive presence. It observes only; it starts no connection and accepts none | `:126-136`, `:1267-1273`, `:2134-2141` |
| `net.flymode` | `fly_mode_check` | Lets the **local** player's own noclip (an existing offline feature) work while a session is active | `:2121-2126` |

That is three capabilities, not the fork's ~35. `net.flymode` is **off** unless a session is active and the anti-cheat-absent check has passed (`AntiCheatPresent`, `F:dllmain.cpp:92`, which sc-offline keeps): it relaxes only the local player's own movement, the same relaxation noclip already makes offline, and nothing on another peer. No excluded patch sits under any of these capabilities.

**Honesty, not stability.** None of these rows is assumed stable. `sco-sigcheck` runs them against `StarCitizen.exe` **every patch** (acceptance for the PR: the `net.*` and `game.post_update` rows `OK` on 4.10.196, pasted into the PR), each resolver checks the bytes it relies on and fails with a static reason, and a failed row disables only its own capability — presence keeps working when `net.flymode` breaks, and so on. CI runs the registry rules against a synthetic image, as today.

**Hooks** go through `sco::hook` (`Transaction`, detours; the `MidHook` facility of section 3.4), never hand-assembled RWX stubs. Switches (`include/sco/game/net.h`, sketch):

```cpp
namespace sco::game::net {
struct Callbacks { void (*frame)(uint32_t ms, void* ctx); void* ctx; };
int  Enable(const Callbacks&);   // installs each capability whose rows are OK; logs the rest. 1 if any came up
void Disable();
int  LocalPlayer(uint64_t* out_entity, char* name, uint32_t cap);   // the local player's own id and name
int  SetLocalNoclip(int on);     // gated on net.flymode + anti-cheat-absent + an active session
}
```

### 3.3 `sc_ipc.h` (the wire, MIT) and the `sco.ipc` service (the in-game wrapper)

Both fork bridges share a shape: a named mapping, a header with magic/version/pids/heartbeat (`F:build.cpp:2823-2845`, `:1285-1305`), seqlock snapshot blocks (`TfPublish` `:2859-2877`; `TfReadTf` `:2846-2857`; TitanLink `plugin.cpp:189-201`), and single-producer rings (`McColWrite` `:1307-1329`; `McInput` `:1331-1343`). Two pieces, with two licenses:

**`include/sc_ipc.h` — the wire format, MIT.** A single header-only C file, no sco-core dependency, that **both** sides of a bridge include: sc-offline's built-in on one side, and the other program (a Northstar plugin, a voxel-game mod) on the other. Because that other side isn't a GPL plugin, the header carries a per-file `SPDX-License-Identifier: MIT`, and `LICENSE` and `CONTRIBUTING.md` note the **interface exception**: `sc_ipc.h` is MIT so a non-GPL program may speak the protocol, while everything else stays GPL-3.0. It defines:

- the channel header (magic `SCO_*`, version, both pids, a monotonic **epoch** and heartbeat);
- a **single-producer single-consumer lock-free ring** (head and tail on separate cache lines, `memcpy` payloads, power-of-two wrap, the padding case from `McColWrite`);
- `static_assert`s on every struct's size and offset, so a layout drift is a compile error on both sides;
- **epoch/sequence validation** (a reader rejects a torn or stale snapshot and re-reads; a consumer ignores a ring that re-opened under a new epoch);
- **hostile-peer bounds checks**: every offset and length read from shared memory is checked against the mapping size before use, since the other process can write anything.

```c
/* include/sc_ipc.h */
/* SPDX-License-Identifier: MIT */
/* Shared-memory wire for sco.ipc bridges. MIT so the non-GPL side of a bridge may include it;
 * see LICENSE (interface exception). No sco-core dependency. */
#define SC_IPC_MAGIC   0x5343494Fu  /* "SCIO" */  /* mapping name: Local\SCO_<plugin>.<channel> */
typedef struct sc_ipc_hdr {
    uint32_t magic, version;
    uint32_t producer_pid, consumer_pid;
    uint64_t epoch;            /* bumped each time a side (re)creates the mapping */
    uint64_t heartbeat_ms;     /* GetTickCount64 of the last writer touch */
    uint64_t bytes;            /* total mapping size, for bounds checks */
} sc_ipc_hdr;
_Static_assert(sizeof(sc_ipc_hdr) == 40, "sc_ipc_hdr");
/* sc_ipc_ring_push / sc_ipc_ring_pop: SPSC, bounds-checked against hdr->bytes; return 0 on a
 * full ring (push) or empty/torn ring (pop). sc_ipc_block_write / sc_ipc_block_read: seqlock
 * snapshots validated by sequence. */
```

**The `sco.ipc` service — the in-game wrapper, GPL.** Published by sco-core's host (table `sco_ipc_v1` in `include/sco_ipc.h`, pinned by `tests/abi_ipc.c`). It is the only way an sc-offline plugin creates a channel, and it enforces what a library alone can't: the name is always `Local\SCO_<plugin id>.<channel>` (never `Global\`, never another plugin's prefix, never an arbitrary name — the fork used `SCTitanLink_v1` and `SkyCraft_v1`); the mapping's security descriptor is **the current user only**; sizes are capped (256 MiB per channel, 512 MiB per plugin — the Minecraft bridge maps ~196 MiB); and the channel is unmapped and its heartbeat stopped when the plugin unloads or crashes, so the other side sees the drop within its timeout. The service lays the `sc_ipc.h` header and rings into the mapping; the bridge's own blocks keep their layouts behind it.

```c
/* include/sco_ipc.h: service "sco.ipc", version 1.0 (sketch) */
typedef struct sco_ipc_v1 {
    uint32_t size; uint32_t _pad;
    int (*create)(sco_plugin* self, const char* name, uint64_t bytes,
                  uint32_t layout_id, uint32_t layout_version, uint64_t* out_channel);
    int (*peer_age_ms)(sco_plugin* self, uint64_t channel, uint32_t* out_ms);
    int (*block_write)(sco_plugin* self, uint64_t channel, uint64_t offset, const void* data, uint32_t size);
    int (*block_read)(sco_plugin* self, uint64_t channel, uint64_t offset, void* out, uint32_t size);
    int (*ring_push)(sco_plugin* self, uint64_t channel, uint64_t offset, uint32_t type, const void* data, uint32_t size);
    int (*ring_pop)(sco_plugin* self, uint64_t channel, uint64_t offset, uint32_t* out_type, void* out, uint32_t* inout_size);
    /* Bulk regions (video frames): a pointer into the plugin's own channel, valid until close or
     * unload — the one allowed pointer, since it names the plugin's own memory, never the game,
     * and 33 MB frames can't be copied per call. */
    int (*view)(sco_plugin* self, uint64_t channel, void** out_base, uint64_t* out_bytes);
    int (*close)(sco_plugin* self, uint64_t channel);
} sco_ipc_v1;
```

Any thread; the heartbeat is driven from the host tick; `block_*` and `ring_*` never block. Tests: a second process attaches using only `sc_ipc.h`; a writer and reader on threads under TSan; the ring wrap and padding from `McColWrite`; refusal of bad names and oversized channels; the heartbeat stopping on unload. Platform half `src/ipc/shm_win.cpp`, with `shm_open` on POSIX for the tests. scosdk and C# wrappers in the same PR; Lua gets no `sco.ipc`.

### 3.4 Engine-side helpers for ghost replication

- **Ghost entities and session-scoped ids.** Unlike the fork, co-presence does **not** rely on the game giving every peer the same entity id. Each player's own game assigns its own ids, so a shared thing gets a **session-scoped id** from `sco.net` when it is announced, and every receiver spawns the announced class **locally as a ghost** (through `spawn.entities`) and keeps a small map `session id -> its own local ghost entity id`. `sco_net_peer.entity_id` and the `multiplayer.spawn` payload carry the **session-scoped** id, 0 once that thing has streamed out. Ids stay opaque `uint64_t`, never decoded, and no real game pointer or id of one peer is ever trusted by another.
- **The transform primitive (`spawn.entities` 1.2).** A ghost is moved every pose with one new call on sc-offline's `spawn.entities` service (section 4.3): `int set_entity_transform(uint64_t entity_id, uint64_t zone_id, const double pos[3], const double rot[4])` — `zone_id` 0 is the world frame, else that zone's local frame; `rot` is `xyzw` like `teleport.spatial`'s `player_pose`; and it moves **only entities the calling plugin spawned** (so a plugin can move its own ghosts and nothing else).
- **`ZonePose` over the wire, by zone name.** A game `zone_id` is a volatile streaming handle, meaningful only inside one game process, so it is never sent. `scosdk/net.hpp` has a fixed `ZonePose` codec that carries the **zone name** (`"OOC_Stanton_2b_Daymar"`) plus a double-precision position local to that zone and an `xyzw` quaternion. The receiver resolves the name to its **own** local `zone_id` (through `teleport.spatial` / `sco::engine::ZoneTree`), then calls `set_entity_transform`. Zone-local doubles keep full precision however far the zone is from the origin. This is a codec, not a replication system; poses go unreliable at the sender's chosen rate and late ones are dropped.
- **`sco::hook::MidHook`.** The benign `net.*` hooks (and any future one) use a proper mid-function hook: it saves the volatile registers, calls a C++ callback with a register context that may continue or branch, relocates instructions through the existing length decoder, and takes its trampoline from `sco::hook`'s allocator inside a `Transaction` — never an ad-hoc RWX page. It replaces the fork's hand-written byte stubs. Tested on synthetic code like the existing detour tests.
- **A frame event.** With `game.frame` resolved, the host kit posts `frame` (`{ size, frame_ms }`) from `CCryAction::PostUpdate`, so ghosts interpolate smoothly between the 10 Hz `tick` and the bridges (section 4.4) get a per-frame callback. Subscribers must stay well under a millisecond; the event is gated on `game.frame`.

### 3.5 Session crypto (D4)

No OpenSSL and no new large dependency. sco-core vendors a **single-file C** implementation of **SHA-256, HMAC-SHA-256 and PBKDF2-HMAC-SHA256** (`third_party/sha2/`, public-domain, built with its own warnings off like Lua and SQLite), tested against the NIST and RFC 4231 / RFC 6070 vectors.

- **Key.** The session passphrase is stretched with **PBKDF2-HMAC-SHA256**, a **per-session random salt** the host sends in the handshake, and a **documented iteration count** (`SCO_NET_PBKDF2_ITERS = 200000`, a named constant so it can be raised by a protocol-version bump). Both sides derive the same 32-byte session key; the passphrase itself never crosses the wire.
- **Handshake.** Client `HELLO` (protocol version, name, client nonce, its channel names) -> host `CHALLENGE` (salt, host nonce) -> client `PROOF` (an HMAC over both nonces under the derived key) -> the product admits -> host `WELCOME` (peer id, channel table, peers). A wrong passphrase fails the `PROOF` check and the join is refused.
- **Per-packet authentication.** Every packet carries a truncated **HMAC-SHA-256 over `protocol_version || channel_fqn || sender_peer_id || seq || payload`** under the session key. A packet that fails the MAC, or whose source address doesn't match its peer, is dropped.
- **Framing.** 32-bit length caps on every field (checked before any allocation), **monotonic per-sender sequence numbers**, a **sliding replay window** that rejects stale or duplicate `seq`, and **constant-time** MAC comparison.
- **Confidentiality.** Traffic is **authenticated and integrity-protected but plaintext on the wire** — contents can be read by someone on the path. State this plainly to players: run sessions over a **VPN (Tailscale, ZeroTier, Radmin)** for confidentiality. Default binding is **LAN-only**; VPN ranges are opt-in (section 3.1).

**As built (plan PR 4a).** The platform-free core is in `src/net/` and `include/sco/net/`; the wire, handshake and delivery rules as built are in [sco.net wire format](../net-wire.md). Where it differs from the text above, and why:

- **The crypto is written for sco-core, not vendored.** `src/net/sha2.c` (plain C11, about 200 lines, under the project's own warnings) instead of `third_party/sha2/`: nothing to fetch, pin or audit from outside, and it is checked against the FIPS 180-4, RFC 4231, RFC 7914 and RFC 6070-style vectors in `tests/test_net.cpp`.
- **Per-link keys.** Packets are MACed under `HMAC(K, "sco.net link" || cn || hn)`, derived from the session key and both handshake nonces, not under `K` itself: a packet from an earlier join (same peer, same `seq`) never verifies again, and one joiner can't forge another's packets.
- **WELCOME is authenticated, and the table and peers follow it.** WELCOME carries an HMAC under `K` that the joiner checks (mutual authentication: a host without the passphrase admits no one). The channel table and the peer list, which can exceed one datagram, are the first reliable message on the control channel (SYNC). HELLO carries the channel names that fit; the rest are registered on the control channel.
- **Star topology.** Joiners talk only to the host, which relays to the other joiners under its own link keys. So `sender_peer_id` is the link's sender (the host, for relayed messages), and the message's origin peer travels in the DATA body; the host checks a joiner can only be its own origin.
- **The iteration count is not on the wire.** `kPbkdf2Iters` (200,000) is a protocol constant; raising it needs a `kProtocolVersion` bump. Tests lower it through `Options` on both sides.

## 4. sc-offline design

### 4.1 `builtins/multiplayer`

A built-in plugin (`src/builtins/multiplayer_plugin.cpp`, id `multiplayer`), like the other built-ins in `src/builtins/`. It is the one place that opens a session and runs co-presence; every other feature only announces.

- **Session control.** It reads the launcher's settings (host or join, the peer's address, the player name, the passphrase) and calls `sco::net::Host` or `sco::net::Join`. There is no dedicated-server process and no `SC_OFFLINE_DEDICATED`. It enables `sco::game::net` (the `frame` event, the local-player read, and `net.flymode` gated).
- **Presence.** It registers `multiplayer.pose` (unreliable) and `multiplayer.spawn` / `multiplayer.despawn` (reliable). Each `frame` it sends the local player's `ZonePose` (zone name + zone-local pose). When a peer's pose first arrives it spawns a **ghost avatar** locally through `spawn.entities`, maps the peer's session-scoped id to that local ghost (`SetPeerEntity`), and moves it every pose with `set_entity_transform`; `multiplayer.despawn` or a `net.peer LEFT` event removes the ghost. The game's netcode is never touched.
- **UI.** A `Multiplayer` tab through `sco.ui`: the peer list from `get_peers` / `get_peer_name`, "Go to" (teleport to a peer's local ghost through `teleport.spatial`), session state and the reason on a drop, and the host's allow-switches (e.g. allow ghost ships).
- **Commands:** `multiplayer.status`, `multiplayer.goto <peer name>`, `multiplayer.leave`.
- **Offline.** With no session, `is_active()` is 0, nothing is announced, and every feature runs exactly as it does today — the "offline behavior unchanged" guarantee, checked by the `docs/features.md` offline checklist in every PR.

The launcher's `host` and `join` commands and the firewall rule (now `remoteip=localsubnet` by default, VPN ranges opt-in, listed and undone per CONTRIBUTING) come from the fork's launcher in PR 7.

### 4.2 Domain features: announce, don't centralize

Co-presence is **announce-and-ghost**, not host-authoritative request/reply. A feature always does its real work in the player's **own** game, exactly as offline; when a session is active it additionally **announces** the result so peers show a ghost:

```text
do_it_locally_as_today();                       // real, functional, in this player's game
if (net->is_active()) net->send_channel(self, "<id>.<channel>", announce, len);   // peers ghost it
```

So the only added behavior in a session is the announcement; offline there is none. Nothing is sent to a "host" to run, no peer runs another peer's action, and no raw object, pointer or code address crosses the wire. A receiver builds its own ghost from the announced **semantic fields** with its own game calls.

| Built-in | Announces (channel) | A peer shows |
|---|---|---|
| `multiplayer` | `multiplayer.pose` (unreliable), `multiplayer.spawn`/`despawn` (reliable) | the player's ghost avatar, moved by pose |
| `spawn` | `spawn.ghost` (reliable: class, session id) + its pose on `spawn.pose` (unreliable) | a ghost of the spawned ship/vehicle, moved by pose |
| `npc`, `build` | `npc.ghost`, `build.ghost` (reliable: class, session id, static ZonePose) | a static ghost of the NPC or building |
| `loadout` | `loadout.look` (reliable, at most 64 KiB, the appearance only, validated before use) | the right gear on the player's ghost |
| `ammo`, `god`, `noclip`, `contracts` | nothing in 1.0 | each is personal to the player's own game; not replicated |

Console forwarding is **not** a feature (it is a [permanent exclusion](#permanent-exclusions-never-ported)). Each built-in lists its channels in its header comment, as `spawn_service.h` documents its table today; the host's ownership, direction and quota checks apply to a third-party plugin the same way.

### 4.3 `spawn.entities` 1.2: moving ghosts

The ghost model needs one new call on sc-offline's `spawn.entities` service, so it becomes 1.2 (`SC_SPAWN_SERVICE_VERSION 0x00010002u`, a field appended after `entity_alive`, `size`-gated like every minor):

```c
/* 1.2: move an entity this plugin spawned. zone_id 0 = world frame, else that zone's local
 * frame; rot is (x, y, z, w) like teleport.spatial's player_pose. 1 on success; 0 for an entity
 * this plugin didn't spawn, an id that isn't streamed in, a bad zone, or a NULL pointer.
 * Game thread. Call only when size > offsetof(sc_spawn_service_v1, set_entity_transform). */
int (*set_entity_transform)(uint64_t entity_id, uint64_t zone_id, const double pos[3], const double rot[4]);
```

Only entities the **calling plugin** spawned can be moved, so `multiplayer` moves its own ghost avatars, `spawn` moves its ghost ships, and neither can move the other's or a real game entity. The receiver resolves the ZonePose's zone **name** to its own `zone_id` (section 3.4) before each call.

### 4.4 The bridges: optional built-ins on `sc_ipc.h`

`build.cpp` in the fork is three features in one file: build mode (`F:build.cpp:1-639`, already `builtins/build_plugin.cpp` on `main`), the Minecraft bridge (`:640-2262`, `Local\SkyCraft_v1`, a DirectComposition overlay, raw-input handover, collision and render rings, world scanning), and the Titanfall 2 bridge (`:2263-3743`, `Local\SCTitanLink_v1`, its own overlay, starting the EA app and `NorthstarLauncher.exe`, a pilot simulation). Each becomes an **optional built-in**:

- **Behind a CMake option,** `SC_OFFLINE_BRIDGE_TITANLINK` / `SC_OFFLINE_BRIDGE_VOXEL`, and **excluded from the default release zip** (resolved: bridges are not in the shipped build). Built or not, a bridge is off until the player enables it.
- **On `sco.ipc`.** The mappings become `Local\SCO_titanlink.link` and `Local\SCO_voxel_bridge.link` with the `sc_ipc.h` header and rings; the bridge's own blocks (`ScBlock`, `TfBlock`) keep their layouts behind it, pinned by `static_assert`. The other side (the Northstar plugin `TitanLink/src/plugin.cpp`; the voxel-game mod, whose source isn't in the download) includes only MIT `sc_ipc.h`.
- **Overlay and input** become one sc-offline helper (`src/bridges/overlay.cpp`); raw-input "capture while a mode is active" is a candidate for a later `sco.ui` minor. **Process launching** (the EA app, Northstar) stays in `builtins/titanlink`, only on the player's key press, logged with the full command line.
- **To settle in the bridge PRs:** where the Northstar and voxel-game sides live (sc-offline, their own repos, or outside), and whether TitanLink's Titanfall-side local-match settings (`ns_auth_allow_insecure`, `sv_cheats` in Titanfall 2, not Star Citizen) are in scope — a Titanfall local match, flagged for the maintainer at that PR. Neither bridge connects to any online service beyond what the player's own second game already does.

### 4.5 Packaging prerequisites

Before the services ship, the SDK zip must carry the headers plugins build against:

- **`include/sc_spatial.h`** is promoted from sc-offline's `src/builtins/spatial_service.h` (the `teleport.spatial` table) into a shipped header and added to `sdk/package.py`'s file list, so a plugin that reads positions (and the ghost receiver) has a stable header. `spawn.entities` (now 1.2) ships the same way.
- **`sdk/third_party/imgui/`** gains `imconfig.h`, `imgui.h` and `LICENSE.txt` (**never `imgui_internal.h`**), so a plugin that draws in an `sco.ui` overlay compiles against the same ImGui the product uses. A CI check compares these **byte for byte** against sc-offline's `src/third_party/imgui/` copies, failing if they drift.
- **Zone persistence.** A `zone_id` is a volatile streaming handle, so anything saved or sent keeps the **zone name plus double coordinates**, never the raw id (section 3.4). This already holds for `sco.storage` saves (saved spots) and now for `ZonePose` on the wire; `package.py` and the docs state it as a rule.

## 5. Risks

| Risk | Mitigation |
|---|---|
| A patch moves one of the benign rows | Only three benign capabilities (session read, `game.post_update`, `net.flymode`); `sco-sigcheck` runs them each patch, every resolver fails with a static reason, a failed row disables only its capability, and `MidHook` means a moved site is a row fix, not a stub rewrite |
| A permanently-excluded patch creeps back | Each is named with its fork location in [section 2](#permanent-exclusions-never-ported); a PR or plugin reintroducing one is out of scope and closed |
| A session exposes a player's PC | PBKDF2 passphrase, per-packet HMAC, monotonic `seq` with a replay window, LAN-only default, a `localsubnet` firewall rule, per-plugin quotas, channel-ownership checks, no remote console, no raw objects or code addresses on the wire |
| Traffic readable on the path | Authenticated and integrity-protected but plaintext; players are told plainly to run sessions over a VPN for confidentiality |
| A hostile peer feeds bad data | Only semantic fields cross the wire and each side builds its own objects with game calls; `sc_ipc.h` bounds-checks every offset and length against the mapping size and validates epoch and sequence |
| Peers with different plugins or versions | `requires = sco.net` gates discovery; channel tables are exchanged at join and a missing channel is a no-op; the protocol version is checked in the handshake |
| Ghost drift or precision | Poses carry a zone **name** plus zone-local doubles, resolved on the receiver; `set_entity_transform` moves only the caller's own ghosts |
| Bridges launching other programs or sharing memory | Optional, excluded from the release zip, off until the player enables them; mappings are user-only, per plugin, size-capped and released on unload |
| Network thread or callback faults | Guarded callouts per plugin; the network thread never calls plugin code |

## 6. Plan: pull requests in order

The directive's order. "In game" means checks the maintainer runs, from sc-offline's `docs/features.md` plus the multiplayer additions. Each sc-offline PR moves the submodule pin.

| # | Repo | PR | Done when |
|---|---|---|---|
| 1 | both (docs) | **Scope rules** (gate, handled by another worker): the [section 1](#1-scope-what-changes-and-what-doesnt) wording in sco-core `README.md`, `CONTRIBUTING.md`, `sdk/docs/plugin-rules.md`, `docs/framework.md`, and sc-offline `README.md`, `CONTRIBUTING.md` | CI green in both repos; the maintainer approves the wording |
| 2 | sco-core (docs) | **Changelog + tag:** a `CHANGELOG.md` entry for this design and the SDK `VERSION`/tag note; links from `docs/framework.md` and `docs/README.md` | CI green; the entry and links are in place |
| 3 | sco-core | **Packaging prerequisites:** promote `include/sc_spatial.h` from sc-offline, add it and `spawn.entities` to `sdk/package.py`; vendor `sdk/third_party/imgui/` (`imconfig.h`, `imgui.h`, `LICENSE.txt`, never `imgui_internal.h`) with a byte-for-byte CI check against sc-offline | CI green incl. the ImGui drift check; the SDK zip carries `sc_spatial.h` and an example builds against it |
| 4 | sco-core | **`sc_ipc.h` + bridges:** MIT `include/sc_ipc.h` (SPDX tag; interface exception in `LICENSE`/`CONTRIBUTING`), the `sco.ipc` service (`include/sco_ipc.h`, `tests/abi_ipc.c`, `src/ipc/`), the voxel-bridge wire; two-process and TSan tests | CI green incl. the second-process test on Windows |
| 5 | sco-core | **`sco.net`:** `src/net/` (`socket_win/posix`, `Core`, reliable+unreliable, vendored `third_party/sha2` SHA-256/HMAC/PBKDF2, handshake, quotas), `include/sco_net.h` + `tests/abi_net.c`, the `sco.net` capability, `sco::game::net` benign rows + `net_hooks.cpp` + `MidHook` + `game.post_update` + the `frame` event, scosdk/C#/Lua, the two-process `sco-host-sim` test + `net_echo` | CI green incl. TSan; the two-process test passes in CI; `sco-sigcheck` on 4.10.196 shows `net.*` and `game.post_update` OK, pasted in the PR |
| 6 | sc-offline | **`spawn.entities` 1.2:** `set_entity_transform`, the version bump and pin | CMake MSVC build and `tools/check.sh` green; in game: an entity the plugin spawned moves via the call; the offline spawn checklist is unchanged |
| 7 | sc-offline | **`builtins/multiplayer` + `builtins/titanlink`:** session, presence, ghosts and the Multiplayer tab; `titanlink` on `sco.ipc` behind its option; the launcher `host`/`join` and the `localsubnet` firewall rule (listed and undone). `voxel_bridge` may follow | CMake MSVC + `check.sh` green; offline checklist unchanged with no session; two PCs over a LAN or VPN: host, join with the right passphrase, refused with a wrong one, both see each other's ghost avatars and ships, "Go to" works, leave and rejoin; with Titanfall 2: link, pilot mode, unlink when either side quits |

PRs 1-5 need no running game (PR 5 needs the exe only for `sco-sigcheck`).

## Resolved by the maintainer's directive (2026-10-09)

The directive settled the earlier open questions. This document takes no legal position on any of them.

| Topic | Decision |
|---|---|
| Model | Co-presence by ghost replication; the dedicated-server model and the game's `connect` are dropped. Pure P2P (one player's client hosts); a headless broker is deferred to v1.x |
| Permanent exclusions | The anti-cheat skip (`F:multiplayer.cpp:1417`), joiner entitlement/account tampering (`:1434`), host-type forcing (`:1366`), network-context override (`:1399`) and the unauthenticated remote console (`Cmd_Console`) are never in any repo, header or official plugin. No research into joining without the anti-cheat step (R1 removed) |
| Benign rows | Only session connect, the main tick, and the local-player fly-mode toggle; kept honest by `sco-sigcheck` each patch, never assumed stable |
| `sco.net` ABI | A plain C table `sco_net_v1` (no virtual `IScoNetService`); `uint32_t size` first, `int` returns, `uint32` sizes, ids never pointers, the peer name by sized copy (no inline `char[32]`) |
| `sco.net` capability | The host publishes the capability `sco.net` so `requires = sco.net` validates in `discover.cpp` |
| Crypto | No OpenSSL; vendored single-file SHA-256/HMAC/PBKDF2; PBKDF2 from the passphrase with a per-session salt and a documented iteration count; per-packet HMAC, monotonic `seq`, replay window, constant-time compare; authenticated but plaintext on the wire, a VPN for confidentiality; LAN-only by default |
| IPC header | `include/sc_ipc.h`, MIT with an SPDX tag and an interface exception in `LICENSE`/`CONTRIBUTING`; the `sco.ipc` service stays as the in-game wrapper |
| `spawn.entities` | 1.2 adds `set_entity_transform`, own-spawned entities only |
| Bridges | Optional built-ins, excluded from the default release zip |
| Remote console | Dropped |
| Movement-validation relaxations | Dropped (the staging / validation-wait cluster); only the local-player noclip fly-mode toggle is kept, gated on no-anti-cheat and an active session |
| In-game hosting | None beyond the P2P host client |
| Peer entity field | Kept, session-scoped (0 when streamed out) |
| Lua | Peer list + pub/sub only, when the manifest `requires = sco.net`; no session control |
| Terms of service | The maintainer decided to proceed; this document takes no legal position |
| `sco.net` location (maintainer, 2026-10-10) | `sco.net` lives in sco-core as a host service: sockets, sessions, crypto, channel isolation and quotas, as section 3.1 says. This overrides the directive's line "sco-core contains zero socket code" |
