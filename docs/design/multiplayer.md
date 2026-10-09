# Design: multiplayer and cross-game bridges on the SDK (`sco.net`, `sco.ipc`, `sco::game::net`)

**Status: design for review. No code yet.** Phase 6 item ([Framework plan](../framework.md#phase-6-the-framework-grows)). This changes sco-core's and sc-offline's scope: the maintainer decided it on 2026-10-09 ([section 1](#1-scope-what-changes-and-what-doesnt)). Every PR in the plan ([section 6](#6-plan-pull-requests-in-order)) gets its own review.

## Why

A parallel fork of sc-offline (a download dated 2026-10-09, forked before sco-core was integrated) added co-op multiplayer between players running sc-offline, plus two bridges that put Titanfall 2 and a Minecraft-style voxel game inside Star Citizen over shared memory. It works by patching the game straight from one 2,166-line file (`multiplayer.cpp`) and a 3,743-line `build.cpp`, with calls into multiplayer scattered through the spawner, contracts, ammo, loadout, NPC, build, menu and console code.

That's the pattern sco-core exists to replace. Every mod that wants co-op would repeat the same 30 or so game addresses, its own UDP protocol and its own threads, and all of them break on every patch. The maintainer's decisions of 2026-10-09:

1. **Scope changes.** sco-core and sc-offline grow from "offline, single-player, never connects to anything" to allowing multiplayer between players running sc-offline.
2. **The bridges become optional plugins:** Titanfall 2 (TitanLink) and Minecraft.
3. **Everything is built as SDK services and abstractions in sco-core** for plugins to use. sc-offline's multiplayer becomes a built-in plugin on top of them.

This document reads the fork's code (section 2), then designs:

- the sco-core side (section 3): a game-agnostic `sco.net` host service, the game-specific network rows and hooks (`sco::game::net`), a `sco.ipc` service for the bridges, and the engine helpers;
- the sc-offline side (section 4): `builtins/multiplayer`, how the domain features use it without hard dependencies, and the bridges as optional built-ins.

Sections 5 and 6 cover the risks and the plan. The open questions are at the end.

**Sources.** The fork is at `sc-offline-source-2026-10-09` (untrusted download, read only; nothing built or run). `F:` below means its `sc-offline/src/` folder. Lines are 1-based, from the files as downloaded. sco-core is at `main` `2196c5a`; sc-offline is at `main` `1f634c0`. A third-party handoff described the fork, and parts of it are wrong. It names `ExpectPlayer` and `net_staging`, which don't appear in `multiplayer.cpp`: the hooked function is the game's `CNetNub::ExpectIncomingConnection` and the variables are `pl_staging.*`. It also names `HookEngine::Instance`, `ZoneManager`, `SCO_PLUGIN_DEFINE` and `GetPluginSdk().QueryService`, none of which exist in sco-core. This design works from the code only.

## 1. Scope: what changes and what doesn't

### The new rules

Proposed wording. It replaces the scope paragraphs of sco-core's `README.md` (§ Scope), `CONTRIBUTING.md` (§ What fits) and `sdk/docs/plugin-rules.md`, and sc-offline's `README.md` and `CONTRIBUTING.md` (§ What fits this project). It lands in plan PR 1, before any code.

> **Scope.** sco-core and sc-offline are for playing Star Citizen offline: alone, or **with other players who also run sc-offline, in a session one of them hosts**. Since 2026-10-09 (maintainer's decision) this is allowed:
>
> - **Private sessions between sc-offline players.** One player hosts a session on their own PC (sc-offline's dedicated server process or their own game); others join it by address over a LAN or a VPN they choose. Every peer runs sc-offline with the game offline. Sessions go through sco-core's `sco.net` service and its rules (an explicit join, a session passphrase, limits).
> - **Local links to other programs on the same PC,** such as another game for a cross-game bridge, through sco-core's `sco.ipc` service (named shared memory in the user's own session, nothing over the network).
>
> This stays out of scope, and PRs or plugins that do it are closed:
>
> - Connecting to Cloud Imperium Games' services or servers in any way (login, game servers, the launcher's backend, the website's APIs), or making the game talk to them.
> - Bypassing or weakening anti-cheat, signature checks, or integrity checks. That includes running a session where such a check would otherwise refuse.
> - Anything that helps cheating in the official game, or that could carry an advantage into it: no memory readers or overlays for live play, no transferring items, money or progress to an online account.
> - Telemetry, analytics, master servers, public server lists, matchmaking or relays run by the project. Sessions are found by sharing an address; nothing reports them anywhere.
> - Collecting or sending personal data. A session shares the player names players choose, nothing else.

Plugin rules gain three lines: a plugin may use `sco.net` and `sco.ipc`, and only them, to talk to other processes or machines. Raw sockets, HTTP clients and their own shared memory are not allowed: a native plugin can technically open them (it runs with the game's rights), so this is a review rule, not a sandbox. Lua plugins get neither service in 1.0.

### Risks this decision brings (for the maintainer to weigh)

- **Game-patch fragility.** The fork needs about 30 game addresses for networking, many of them mid-function patches with hand-assembled code stubs (section 2, "Hook points"). They sit deeper in the engine than anything sco-core resolves today, and session code changes with server meshing work. Expect more `FAILED` rows per patch than teleport or the DataCore loader. Mitigation: rows with layout checks, `sco-sigcheck` on every build, and a capability per piece so a broken piece switches only itself off (section 3.2).
- **Terms of service.** Running the game client as a server, and letting players connect their offline games to each other, may raise questions under CIG's terms that offline single-player didn't. This doc gives no legal assessment. The maintainer judges it, and may want advice before shipping.
- **Security exposure.** sc-offline today never listens on a port. With multiplayer, a player's PC accepts packets, and the fork's protocol has no authentication (section 2, "Security findings"). That is the main reason for a host-owned service with secure defaults instead of per-mod sockets.
- **Anti-cheat adjacency.** One fork patch skips an anti-cheat step on the dedicated server (section 2). It isn't ported. If joining doesn't work without it, the server-based join is blocked (open question 1). Nothing in this design works around that.
- **Support load.** Multiplayer bugs need two or more machines and logs from each.

## 2. What the fork's `multiplayer.cpp` does

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
| Joiners as players | `49 8B 51 10 44 8B 40 28 48 85 D2 0F 84 ...` | Mid-function code stub: connection type 1 is treated as 3 | `ServeJoinersAsPlayers`, `:1366-1388` | Server |
| Game context | `48 8B 01 FF 50 18 48 8B C8 48 8B 00 FF 90 B8 00 00 00 ...` | Code stub: skips a context lookup | `ShareContextFromCVars`, `:1399-1411` | Server |
| **Anti-cheat step** | (named only) | Code stub | `SkipAntiCheatWithoutService`, `:1415-1432` | Server. **Not ported** (below) |
| Joiner account | `4D 8B 40 08 FF 50 38 49 8B 55 10 ...` | Code stub calls `FixJoinerAccount`, which writes the joiner's own player id, account number and nickname into the local server's account record | `:1436-1471` | Server |
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

**The anti-cheat step is out of scope** under both the old and the new rules, and is not ported. This document names it and doesn't describe it. The fork's own log says the server crashes when a player joins without it (`:2148`). Whether that holds on 4.10.196 is research item R1 in the plan; R1 only tests the join without the step and does nothing more.

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

### 3.1 `sco.net` 1.0: sessions and messages, game-agnostic

A host-owned service like `sco.storage`: published under the reserved id `sco`, found with `query_service`, `sco_api.h` unchanged. The table is in `include/sco_net.h` and pinned by `tests/abi_net.c`. It follows the rules of `sco_api.h`: 4-byte enums, `size` first, results instead of exceptions, ids instead of pointers ([lesson 6](../framework.md#6-services-hand-out-ids-never-pointers)).

**Who controls the session.** Hosting, joining and leaving are **the product's** decisions, made from the launcher or the menu. They go through the host-side C++ API `sco/net.h`, not through the plugin table. sc-offline's `builtins/multiplayer` calls it; built-ins may read internal headers. Plugins see the session, its peers and their own message types, and can't open or join sessions. That's least privilege: a third-party plugin can't make a player's PC listen. A product that wants to expose session control to plugins can wrap it in its own commands.

```cpp
// include/sco/net.h (host side, C++; sketch)
namespace sco::net {
struct HostOptions {
    uint16_t    port = 64091;               // UDP; one port for everything sco.net carries
    std::string passphrase;                 // required; shown to the host player to share
    std::string playerName;
    uint32_t    maxPeers = 8;               // at most SCO_NET_MAX_PEERS
    bool        dedicated = false;          // no local player in this process
    BindScope   bind = BindScope::Lan;      // Lan: refuse handshakes from non-private addresses; Any: opt-in
};
struct JoinOptions { std::string address; uint16_t port = 64091; std::string passphrase, playerName; };
// The product's game adapter answers here, on the game thread, before a peer is admitted
// (sc-offline: expect the joiner in the game's netcode and return the game port).
using AdmitFn = Result (*)(PeerId peer, std::span<const uint8_t> hello, std::vector<uint8_t>& welcome, void* ctx);
Result Host(const HostOptions&, AdmitFn, void* ctx);
Result Join(const JoinOptions&, void (*welcomed)(std::span<const uint8_t> welcome, void* ctx), void* ctx);
void   Leave(const char* reason);
void   SetPeerEntity(PeerId, uint64_t entityId);   // the game's id for that peer's player (section 3.4)
}
```

**The plugin table** (sketch):

```c
/* include/sco_net.h: service "sco.net", version 1.0 */
#define SCO_NET_NAME            "sco.net"
#define SCO_NET_VERSION_1_0     0x00010000u
#define SCO_NET_MAX_PEERS       16u
#define SCO_NET_MAX_UNRELIABLE  1200u           /* bytes: one datagram, no fragmentation */
#define SCO_NET_MAX_RELIABLE    (256u * 1024u)  /* bytes: fragmented and reassembled */
#define SCO_NET_MAX_TYPES       64u             /* message types per plugin */
#define SCO_NET_BROADCAST       0u              /* "to": every other peer */

typedef enum sco_net_state { SCO_NET_IDLE = 0, SCO_NET_HOSTING = 1, SCO_NET_JOINING = 2,
                             SCO_NET_JOINED = 3, SCO_NET_STATE_FORCE32 = 0x7fffffff } sco_net_state;

/* Type flags: delivery, and who may send it (checked by the host before any handler runs). */
#define SCO_NET_RELIABLE   0x1u   /* acknowledged, resent, in order per type and peer */
#define SCO_NET_FROM_HOST  0x2u   /* accepted only when the sender is the session host */
#define SCO_NET_TO_HOST    0x4u   /* accepted only by the session host */

/* Peer flags */
#define SCO_NET_PEER_SELF      0x1u
#define SCO_NET_PEER_HOST      0x2u
#define SCO_NET_PEER_DEDICATED 0x4u   /* the host process has no local player */

typedef struct sco_net_peer_info {
    uint32_t size;
    uint32_t flags;
    uint64_t peer;        /* opaque, unique for the session; 0 is never a peer */
    uint64_t entity;      /* the game's id of this peer's player entity, 0 until known */
    uint32_t rtt_ms;
    uint32_t _pad;
    char     name[32];    /* the name the player chose, NUL-terminated */
} sco_net_peer_info;

typedef struct sco_net_message {
    uint32_t    size;
    uint32_t    type;     /* the handle register_type returned */
    uint64_t    from;     /* peer id */
    const void* data;     /* valid during the handler only */
    uint32_t    data_size;
    uint32_t    _pad;
} sco_net_message;

typedef void (*sco_net_handler)(const sco_net_message* msg, void* ctx);   /* game thread */

typedef struct sco_net_stats {
    uint32_t size;
    uint32_t queued_bytes;          /* reliable bytes waiting for this plugin and peer */
    uint64_t sent, received, dropped_unreliable, refused;   /* messages */
} sco_net_stats;

typedef struct sco_net_v1 {
    uint32_t size;
    uint32_t _pad;
    /* Any thread. The session state; SCO_NET_IDLE when there is none (offline play). */
    uint32_t   (*state)(void);
    /* Any thread. This process's peer id and the host's; 0 when idle. */
    uint64_t   (*self_peer)(void);
    uint64_t   (*host_peer)(void);
    /* Any thread. Peer ids with the size handshake (*inout_count: capacity in, count out). */
    sco_result (*peers)(uint64_t* out, uint32_t* inout_count);
    /* Any thread. SCO_NOT_FOUND: no such peer (left, or never was). */
    sco_result (*peer_info)(uint64_t peer, sco_net_peer_info* out);
    /* Any thread, idle or in a session. Registers "<plugin id>.<name>" with flags and a
     * per-message size limit (at most SCO_NET_MAX_RELIABLE or _UNRELIABLE); fn runs on the game
     * thread for every accepted message of this type. SCO_BAD_ARG: bad or taken name, bad flags. */
    sco_result (*register_type)(sco_plugin* self, const char* name, uint32_t flags,
                                uint32_t max_size, sco_net_handler fn, void* ctx, uint32_t* out_type);
    /* Any thread. Copies data and queues it to one peer or SCO_NET_BROADCAST.
     * SCO_UNAVAILABLE: no session, or the peer lacks this type (its plugin isn't installed there).
     * SCO_TOO_MANY: this plugin's queue for that peer is full (reliable), or it is over its
     * bandwidth share (unreliable: the message is dropped and counted).
     * SCO_BAD_ARG: too big, wrong direction (a FROM_HOST type from a non-host, TO_HOST to a non-host). */
    sco_result (*send)(sco_plugin* self, uint32_t type, uint64_t to, const void* data, uint32_t size);
    /* Any thread. Counters for this plugin and peer (0: all peers). */
    sco_result (*stats)(sco_plugin* self, uint64_t peer, sco_net_stats* out);
} sco_net_v1;
```

**Events** on the existing bus, posted on the game thread. Each data struct starts with `size`:

- `net.state`: `{ size, state, reason[128] }`. Join refused, the host left, timed out, kicked.
- `net.peer`: `{ size, what (JOINED, LEFT, ENTITY), peer }`. `ENTITY` fires when the product sets a peer's game entity id.

**Message types.** Types are names, `"<plugin id>.<name>"`, so they need no central numbering. At admission the host sends each joiner its table: name -> 16-bit wire id, flags, size limit. The joiner sends back the names it has registered. A type only one side has is "missing" for that peer: `send` answers `SCO_UNAVAILABLE`, and a handler never sees it. Plugin lists can therefore differ between peers, and each feature says "the host doesn't have X". A type registered after admission is announced with an internal reliable control message. Handlers get only the bytes of their own type. The payload layout is the plugin's contract; the header recommends a leading `uint16_t version`.

**Delivery.**

- **Unreliable:** at most 1,200 bytes, may be lost, duplicated or reordered. For state that's resent anyway.
- **Reliable:** at most 256 KiB, fragmented. Delivered exactly once and in order per type and peer. Acks are piggybacked, with resend on RTT-based timeouts.
- **Ordering:** none between types; a plugin that needs it uses one type.

**Limits.**

- Peers: 16 per session (the fork's cap), `maxPeers` default 8.
- Send queue: at most 4 MiB of queued reliable bytes per plugin and peer, then `SCO_TOO_MANY`.
- Bandwidth: a share per plugin and peer. Default: 512 KiB/s for the whole session to each peer, split evenly between the plugins sending. The product can set the total.
- Receiving: a peer that sends more than 2 MiB/s, or unknown or refused types above a threshold, is disconnected with a reason.
- Every received length is checked against the type's `max_size` before reassembly allocates.

**Threads: what the host owns, what plugins own.**

| Host (`src/net/`) | Plugins |
|---|---|
| The one UDP socket, one network thread (receive, acks, resends, keepalive every 1 s, peer timeout 30 s), the handshake, the type table, queues and limits | Their message types, payload formats, handlers and the game work handlers do |
| Hands received messages to the game thread: a queue drained in `sco::app::Tick` before `tick` subscribers, as guarded callouts per plugin (a faulting handler disables only that plugin) | `send` from any thread (copies); handlers run on the game thread and must stay short (the `tick` rule) |
| On unload or crash: drops the plugin's queued messages and types, never calls its handlers again | Nothing to clean up |

**Transport and handshake.** One UDP port (64091 by default) replaces the fork's UDP and TCP pair. Bulk data such as the universe layout goes through the reliable channel's internal bulk path. That path is available to the game adapter in `sco::game::net`, not to plugins in 1.0: windowed, up to 512 MiB, with progress.

- **Packet header:** magic `SCN1`, protocol version, 64-bit session id, sender peer, sequence, ack and ack bits, and a 16-byte truncated HMAC-SHA-256.
- **Handshake:** the client sends `HELLO` (protocol version, name, client nonce, its type names). The host answers `CHALLENGE` (host nonce). The client replies `PROOF`: an HMAC over both nonces with a key derived from the passphrase. Then the product's `AdmitFn` runs on the game thread, and the host sends `WELCOME` with the peer id, the type table, the peers, and the adapter's welcome bytes (the game port).
- **Authentication:** every later packet is authenticated with the session key. A packet from an address that doesn't match its peer is dropped. A session needs a passphrase; there's no open mode.
- **No encryption in 1.0.** Contents can be read on the path, so this is for LAN or a trusted VPN. TLS-grade encryption would need a crypto dependency (open question 4). HMAC-SHA-256 is a small implementation in sco-core, tested against the RFC 4231 vectors, so no new dependency.

**Defaults.** `BindScope::Lan` refuses handshakes from public addresses. RFC 1918, link-local and loopback are allowed, plus CGNAT space (100.64.0.0/10), which some VPNs use. Allowing any address is an explicit host option. sc-offline's firewall rule moves to `remoteip=localsubnet` by default, with VPN ranges added when the player picks "VPN" (open question 3). Nothing in sco.net discovers sessions, reports them, or talks to anything but the address a player typed.

**Language layers.**

- **`scosdk/net.hpp`:** a `Net` handle over `ServiceRef<sco_net_v1>`; `MessageType<T>` registered with a lambda, with `Send(peer, const T&)` for trivially copyable `T` and `Send(peer, std::span<const std::byte>)`; `Peers()` returning a small vector of `PeerInfo`. Every call is `noexcept` and returns `sco_result`, as in the rest of the SDK.
- **`Sco.Sdk` (C#):** a `Net` class with `RegisterType(name, flags, maxSize, Action<NetMessage>)`, where the message exposes `ReadOnlySpan<byte>` during the callback. It's AOT-safe like the `Storage` layer and pinned in `Pins.cs`.
- **sco-lua: none in 1.0.** Network handlers inside the step budget are possible, but a Lua plugin talking to other machines is a bigger sandbox change. It gets its own review, as with Lua tabs (G018). Open question 6.

**Tests.**

- **The core, no service:** `sco::net::Core` is a class with no globals, so one test binary runs **two or more cores in one process**: a host and clients on `127.0.0.1` with ephemeral ports. For the reliable layer there's also an in-memory transport that drops, duplicates and reorders from a fixed seed, driven by `Pump(nowMs)` with a virtual clock, with no sleeps and no real time. Coverage: handshake (right and wrong passphrase, wrong protocol version, address spoofing), exactly-once in-order delivery under loss, fragmentation at the limits, queue and bandwidth limits, peer timeout, type tables with missing types, direction flags. Under TSan: `send` from 8 threads while the network thread runs.
- **The service through `sco-host-sim`:** the runtime is one per process (`sco/runtime.h`), so the service binding is tested with **two `sco-host-sim` processes on loopback**. New options: `--net-host <port> --net-pass <p>`, `--net-join 127.0.0.1:<port> --net-pass <p>`, and `--until-event <name> --timeout-ms <n>`, which ticks until the event, so there are no fixed sleeps. A CTest fixture starts the host, then the joiner. A new example `sdk/examples/net_echo` (C) registers `net_echo.ping` and `net_echo.pong`; the joiner pings, and the test passes when the joiner's log shows the pong. Run on Linux and Windows CI.
- **`tests/abi_net.c`:** pins the table, constants and signatures like `abi_storage.c`. A C# pin goes in `Pins.cs`.

**Portability.** The socket layer is `src/net/socket_win.cpp` (Winsock) and `src/net/socket_posix.cpp`. CONTRIBUTING's list of files allowed to include Windows headers grows by the first one in the same PR.

### 3.2 `sco::game::net`: the game-specific rows and hooks

Following the `sco::game::pak` decision ([vfs-datacore decision 1](vfs-datacore.md#decisions-maintainer-2026-10-09)), the engine adapter lives in sco-core. That means rows in `src/game/net_sigs.cpp`, hooks in `src/game/net_hooks.cpp`, and the accessor and switches in `include/sco/game/net.h`. sc-offline only enables the pieces it uses and supplies its callbacks. The rows move **byte for byte** from the fork ([Adding a signature](../adding-signatures.md) rule 1), in a separate commit from any fix.

The rows, grouped into capabilities so a broken group switches only itself off:

| Capability | Rows (`net.*` unless noted) | From |
|---|---|---|
| `game.frame` | `game.post_update` (`CCryAction::PostUpdate`, prologue check) | `:1261-1265` |
| `net.session` | `expect_incoming`, `expect_copy_params`, `connect_cmd`, `framework`, `session_mgr`, `inactivity_cvars` | `:1348-1356`, `:2134-2141`, `:1178-1184` |
| `net.dedicated` | `startup_filler`, `services_first_user` (reuses sc-offline's stand-in hub), `create_socket_groups`, `marker_restore`, `qt_groups`, `add_marker`, `mission_class_switch`, `mission_spawn_class_switch`, `host_type_site`, `context_site`, `account_site` | `:476-485`, `:407-417`, `:1357-1361`, `:448-474`, `:428-437`, `:1366-1411`, `:1451-1471` |
| `net.layout` | `layout_send_site`, `layout_root`, `layout_gather`, `layout_destroy`, `layout_free`, `layout_recv`, `layout_request` | `:1712-1756`, `:1769-1781` |
| `net.join` | `loading_wait_site`, `staging_log`, `staging_cvars`, `actor_cvars`, `validation_config`, `player_name_slot` | `:1806-1917`, `:358-385` |
| `net.visibility` | `bind_to_player`, `lookup_record`, `repl_net_tick`, `streamable_reg` | `:2076-2092` |
| `net.flymode` | `fly_mode_check` | `:2121-2126` |

That's about 35 rows. Not ported, under any capability: the anti-cheat step (`:1415-1432`).

**Acceptance.** `sco-sigcheck` on **4.10.196** (`StarCitizen.exe` of the current LIVE build) reports every `net.*` row and `game.post_update` `OK`, run locally and pasted into the PR. The fork was written against an earlier build, so rows that fail on 4.10.196 are fixed in a second commit with the reason, per the rules. Every resolver checks the bytes it relies on and fails with a static reason. CI runs the rows against a synthetic image for the registry rules, as today.

**Hooks.** They go through `sco::hook` (`Transaction`, detours) instead of the fork's `HookFunction`. Each enable is one transaction per capability, and a capability whose rows fail installs nothing. The fork's hand-assembled stubs (six of them, `:1366-1471`, `:1712-1756`, `:1806-1831`) become a new `sco::hook` facility (section 3.4), not byte arrays in the adapter.

**Switches** (`include/sco/game/net.h`, sketch):

```cpp
namespace sco::game::net {
enum class Role { Client, DedicatedServer };
struct Callbacks {
    void (*joinerExpected)(uint64_t entityId, void* ctx);           // server: a joiner is in the game's netcode
    void (*layoutProgress)(uint64_t done, uint64_t total, void* ctx);
    void* ctx;
};
bool Enable(Role, const Callbacks&);      // installs every capability whose rows are OK; logs the rest
void Disable();
// Server: register a joiner with the game (ExpectIncomingConnection with params built from the hello).
Result ExpectJoiner(const JoinerParams&);
// Client: run the game's connect to address:port once the session's welcome came.
Result Connect(const char* address, uint16_t gamePort);
// Visibility: the adapter binds noted entities and ready players to each joiner on the game's tick.
void NoteStreamable(uint64_t entityId);
}
```

**What needs the maintainer's judgment before it's ported** (open question 2): `net.flymode` and the `net.join` validation pieces. These are `pl_staging.forceClientValidation`, the 60 s to 15 s validation wait, and skipping the fly-mode mismatch check. They relax the game's own movement validation between a client and the server it joined. In a session hosted by an sc-offline player that's the host's choice (noclip is already an offline feature), and sc-offline never runs alongside anti-cheat (`AntiCheatPresent`, `F:dllmain.cpp:80`, `:190-200`, unchanged in sc-offline `main`). They are still validation relaxations in a public library. The proposal:

- They're separate capabilities, off unless the session host allows them. The host's setting is sent in the welcome.
- They're enabled only while a `sco.net` session is joined. They're never enabled without the anti-cheat-absent check passing.
- `net.dedicated`'s account row (`FixJoinerAccount`) writes only the local server's own account record, never anything of CIG's. The doc names it so the reviewer can confirm that.

### 3.3 `sco.ipc` 1.0: local shared-memory channels for bridges

**Choice: a host service, not just a library.** Both fork bridges already share a protocol shape:

- a named mapping in `Local\`;
- a header with magic, version, both processes' pids and a heartbeat (`F:build.cpp:2823-2845`, `:1285-1305`);
- seqlock snapshot blocks, where an odd sequence number means "writing" (`TfPublish`, `:2859-2877`; `TfReadTf`, `:2846-2857`; TitanLink `plugin.cpp:189-201`, `:277-282`);
- single-producer rings (`McColWrite`, `:1307-1329`; `McInput`, `:1331-1343`).

A library would give each bridge the same code. A service also gives the host what the new scope needs to enforce:

- **The namespace.** Channels are always `Local\sco.<plugin id>.<name>`: never `Global\`, never another plugin's prefix, never an arbitrary name (the fork used `SCTitanLink_v1` and `SkyCraft_v1`).
- **Access.** Mappings get a security descriptor for the current user only, instead of the default DACL.
- **Limits.** At most 256 MiB per channel and 512 MiB per plugin. The Minecraft bridge maps about 196 MiB: 3 x 4K frame slots, 32 MiB collision and 64 MiB render rings, `:640-646`.
- **Lifetime.** Channels are closed and unmapped when the plugin unloads or crashes, and the host stops their heartbeat, so the other side sees the link drop within its timeout.
- **One wire format,** versioned in one place. Bridges for other games reuse it.

The library half stays: **`include/sco_ipc_wire.h`**, a header-only C file with no sco-core dependency. It holds the header layout, the seqlock read/write and the ring push/pop. The other side (a Northstar plugin, a game mod) includes only that file. Its license matters, because those sides aren't GPL plugins (open question 5).

```c
/* include/sco_ipc.h: service "sco.ipc", version 1.0 (sketch) */
typedef struct sco_ipc_v1 {
    uint32_t size; uint32_t _pad;
    /* Creates Local\sco.<plugin id>.<name>, bytes long, with the wire header (layout id and
     * version are the bridge's own). The host writes this side's heartbeat every tick. */
    sco_result (*create)(sco_plugin* self, const char* name, uint64_t bytes, uint32_t layout_id,
                         uint32_t layout_version, uint64_t* out_channel);
    /* Ms since the other side's heartbeat; SCO_NOT_FOUND: it never attached. */
    sco_result (*peer_age)(sco_plugin* self, uint64_t channel, uint32_t* out_ms);
    /* Seqlock snapshot blocks at an offset inside the channel. */
    sco_result (*block_write)(sco_plugin* self, uint64_t channel, uint64_t offset, const void* data, uint32_t size);
    sco_result (*block_read)(sco_plugin* self, uint64_t channel, uint64_t offset, void* out, uint32_t size);
    /* SPSC rings at an offset (head and tail on separate cache lines, as the fork's rings). */
    sco_result (*ring_push)(sco_plugin* self, uint64_t channel, uint64_t offset, uint32_t type, const void* data, uint32_t size);
    sco_result (*ring_pop)(sco_plugin* self, uint64_t channel, uint64_t offset, uint32_t* out_type, void* out, uint32_t* inout_size);
    /* Bulk regions (video frames): a pointer to the mapping, valid until close or the plugin's
     * unload. The one deliberate exception to "ids, never pointers": it points into memory the
     * plugin's own channel owns, never into the game, and copying 33 MB frames per call is not an option. */
    sco_result (*view)(sco_plugin* self, uint64_t channel, void** out_base, uint64_t* out_bytes);
    sco_result (*close)(sco_plugin* self, uint64_t channel);
} sco_ipc_v1;
```

Any thread. The heartbeat is driven from the host's tick; `block_*` and `ring_*` never block. Tests: create a channel, attach a second process using only `sco_ipc_wire.h` (a small test program), and check the seqlock with a writer and reader on threads under TSan. Also check the ring wrap and padding cases from `McColWrite`, refusal of bad names and sizes, and the heartbeat stopping when the plugin unloads. The platform half is `src/ipc/shm_win.cpp`, with `shm_open` on POSIX for the tests. scosdk and C# wrappers come in the same PR; Lua gets none.

### 3.4 Engine-side helpers

- **Entity ids across peers.** Entities the host's game spawned keep the same entity id on every peer, because the game replicates them. The fork relies on this: ship ids in spawn replies, seat ids, "go to player" teleporting to a peer's id (`:931`). Entities a client spawned locally exist only on that client. The rule for plugins, in `sco_net.h` and `api-v1.md`: **an entity id is shared across peers only if the host spawned it.** So features that must be shared ask the host to spawn (section 4). `sco_net_peer_info.entity` gives each peer's player entity id, which the adapter sets from the expect parameters (`+0x18`). Ids stay opaque `uint64_t`, never decoded.
- **Zones and transforms.** The game replicates transforms for its own entities, and `sco.net` doesn't duplicate that. For plugin state the game doesn't replicate (bridge avatars, markers a plugin draws), `scosdk/net.hpp` has a fixed 64-byte `ZonePose` codec: a zone id, a position local to the zone in double precision, and a rotation quaternion. The receiver resolves it with its own `sco::engine::ZoneTree` (zone ids are game entities, shared as above). Positions stay zone-local, which avoids the precision loss of world coordinates. This is a codec, not a replication system. Plugins send poses at the rate they choose, unreliable, and drop late ones.
- **`sco::hook::MidHook`.** It replaces the fork's six hand-written stubs. At a site, it saves the volatile registers and calls a C++ callback with a register context. The callback can read and write the registers and choose "continue" or "jump to X". The relocated instructions are length-decoded by the existing engine, and the trampoline memory comes from `sco::hook`'s allocator, not ad-hoc RWX pages. It's part of a `Transaction`. Tests run on synthetic code, like the existing detour tests.
- **A frame event.** The dedicated server has no window or message loop, and the bridges need a callback every frame, not the 10 Hz `tick`. With `game.frame` resolved, the host kit posts `frame` from `CCryAction::PostUpdate` (`{ size, frame_ms }`). On a dedicated server, `sco::app::Tick` is driven from there too. Subscribers must stay well under a millisecond, and the event is gated on the `game.frame` capability.

## 4. sc-offline design

### 4.1 `builtins/multiplayer`

A built-in plugin (`src/builtins/multiplayer_plugin.cpp`, id `multiplayer`), in the same form as the other built-ins in `src/builtins/`:

- **Session control.** It reads the launcher's settings (the fork's `SC_OFFLINE_DEDICATED`, `SC_OFFLINE_JOIN`, `SC_OFFLINE_NAME` and `SC_OFFLINE_JOIN_PORT`, plus a new passphrase) and calls `sco::net::Host` or `Join`. It enables `sco::game::net` for the role.
- **Admission.** On the server, its `AdmitFn` calls `sco::game::net::ExpectJoiner` with the joiner's hello (session, node, entity id, name) and answers the game port. This is the fork's `SCOHELLO`/`SCOOK` (`:245-313`) as a product hook. It also sets `SetPeerEntity`. On the client, the welcome triggers `sco::game::net::Connect`.
- **What stays here:** the join state machine with F9 retry, the universe layout through the adapter's bulk path, visibility (`NoteStreamable` and the per-joiner binding), re-dress after first sight, the per-player data file, and the server-side "load mission scripts on the first join".
- **UI.** A `Multiplayer` tab through `sco.ui`: the peer list from `sco.net` peers, "Go to" (teleport to the peer's entity through `teleport.spatial`), session state and reasons, and the host's settings (allow noclip, allow console).
- **Commands:** `multiplayer.status`, `multiplayer.goto <peer name>`, `multiplayer.leave`.
- **Offline.** With no launcher setting, it loads, publishes nothing, and `sco.net` stays `IDLE`.

The launcher's `host` and `join` commands and the firewall rule (with the new remote scope) come over from the fork's launcher in their own PR.

### 4.2 Domain features: through services, without hard dependencies

Each built-in that has a multiplayer half **registers its own `sco.net` message types** and decides locally:

```text
if (net->state() == SCO_NET_JOINED && !(self is host))   -> send a request to host_peer()
else                                                      -> run it here, as today
```

`sco.net` is host-owned and always published, so there's no load-order dependency on `builtins/multiplayer`. In offline play `state()` is `IDLE` and every path is today's local path, with no message types used. That's the "offline behavior unchanged" guarantee, and the in-game offline checklist in `docs/features.md` checks it in every PR. On the host, the handler runs the same local function the command runs, with the requesting peer's entity id instead of the local player's.

| Built-in (sc-offline `main`) | Message types | Replaces |
|---|---|---|
| `spawn` | `spawn.request` (TO_HOST, reliable: class, zone, pose, sit, flight-ready), `spawn.reply` (FROM_HOST: status text, ship id, flags), `spawn.retrieve` (TO_HOST: ATC entity, class) | `SCOSPAWN`, `SCOSPWND`, `SCOATCRQ`; the server seat job (`:608-660`) moves into `spawn` on the host |
| `crew` | `crew.seat` (TO_HOST: action, ship, seat, NPC, replace), reply via `spawn.reply`'s shape | `Cmd_Seat` |
| `npc` | `npc.spawn`, `npc.undo`, `npc.clear` (TO_HOST) | `Cmd_Spawn`/`Undo`/`Clear` kind NPC |
| `build` | `build.place`, `build.undo`, `build.clear` (TO_HOST) | kind Build |
| `loadout` | `loadout.gear` (TO_HOST, reliable, at most 64 KiB, XML validated as a loadout before it's written to a file the host names) | `SCOGEAR` |
| `ammo` | `ammo.state` (TO_HOST, unreliable, resent every 5 s) | `Cmd_Ammo` |
| the noclip and god owner (`spawn` today) | `player.state` (TO_HOST, unreliable, resent) | `Cmd_Noclip`, `Cmd_God`; applied only if the host allows it |
| `contracts` | `contracts.objective` and `contracts.end` (FROM_HOST, reliable: **semantic fields only**, such as objective id, texts, state, marker entity ids and reason, rebuilt on the client with game calls); `contracts.offer` (TO_HOST); wallets on the host through `sco.storage` keyed by player name | `SCOOBJ` (no raw bytes or vtables over the wire, finding 3), `SCOEND`, `SCOOFFER`, `SaveServerWallet` |
| menu shell | none: the player list is `sco.net`'s peers | `SCOPLAYQ`/`SCOPLAYR`, `SCONAMEQ`/`SCONAMER` (names are chosen by the player and carried in the hello) |
| cvars and console | none in 1.0 | `Cmd_Console` is dropped. If the maintainer wants it back, it's an allowlist of CVars the host approves (open question 7) |

The types each built-in registers are listed in its header comment, as `spawn_service.h` documents its table today. A third-party plugin uses the same pattern, and the same host-side checks (direction flags, sizes, limits) apply to it.

### 4.3 The bridges: optional built-ins on `sco.ipc`

`build.cpp` in the fork is three features in one file:

| Lines | What | Where it goes |
|---|---|---|
| `F:build.cpp:1-639` (with `ResolveBuildApi`, `:86`) | Build mode: placement, ghost preview, undo and clear; its multiplayer branches at `:397-400`, `:476-481`, `:625-633` | Already `builtins/build_plugin.cpp` on `main`; it gains the `build.*` message types (section 4.2) |
| `:640-2262` | The Minecraft bridge (`[minecraft]`): the `Local\SkyCraft_v1` mapping, `McConfig`, a DirectComposition overlay thread with its own D3D11 device (`:906`), raw-input handover by patching `user32` imports (`McPatchImport`, `:714-743`), collision and render rings, scanning the world into blocks (`:1730`), NPC hits, body hiding | `builtins/voxel_bridge` |
| `:2263-3743` | The Titanfall 2 bridge (`[titanfall]`): the `Local\SCTitanLink_v1` mapping (`:2823-2840`), its own overlay (`:2551`), raw-input handover, starting the EA app and `NorthstarLauncher.exe` (`:2889-2940`), a pilot simulation (`:3053`), shooting and titan calls | `builtins/titanlink` |

`MinecraftFrame` and `TitanfallFrame` run from the multiplayer `PostUpdate` hook (`F:multiplayer.cpp:1257-1258`). The bridges move to the `frame` event (section 3.4) and stop depending on multiplayer.

Each bridge becomes:

- **A built-in behind a CMake option:** `SC_OFFLINE_BRIDGE_TITANLINK` and `SC_OFFLINE_BRIDGE_VOXEL`, default OFF (open question 8). When built, it is still off until the player enables it.
- **On `sco.ipc`.** The mappings become `Local\sco.titanlink.link` and `Local\sco.voxel_bridge.link` with the shared wire header (wire v2). The bridge's blocks keep their current layouts (`ScBlock` and `TfBlock`, TitanLink `plugin.cpp:117-156`) behind it. The Northstar side (`TitanLink/src/plugin.cpp`) moves to `sco_ipc_wire.h` in the same PR. The Minecraft-side program isn't in the download (`[minecraft] link open: start the Minecraft side now`, `:1303`), so its source is open question 9.
- **Overlay and input.** The two near-identical overlay threads become one sc-offline helper (`src/bridges/overlay.cpp`). Raw-input handover (swallowing keys while a bridge mode is on) is a candidate for a later `sco.ui` minor ("input capture while a mode is active"). Until then it stays in the helper, written once.
- **Process launching** (the EA app, Northstar) stays in `builtins/titanlink`, only on the player's key press, logged with the full command line.

TitanLink's Northstar side sets `ns_auth_allow_insecure 1`, `sv_cheats 1`, and turns off reporting to Northstar's master server for its local match (TitanLink `plugin.cpp:356-366`). That's a local, unlisted match. It still switches off an authentication check in another game, so it's flagged for the maintainer under the new scope rule (open question 10). Nothing in the bridge connects to EA's or Northstar's online services beyond what the player's own Titanfall 2 launch does.

## 5. Risks

| Risk | Mitigation |
|---|---|
| Game patches break network rows (about 35, deeper than any so far) | One capability per group, so a group fails alone. `sco-sigcheck` on patch day. Layout checks in every resolver. `MidHook` instead of byte stubs, so a moved site is a row fix, not a stub rewrite |
| Joining doesn't work without the anti-cheat step | Not worked around. R1 finds out; the maintainer decides what happens next (open question 1) |
| Terms-of-service questions | The maintainer judges, possibly with advice. The scope text says plainly what is and isn't done |
| A session exposes a player's PC | Passphrase and authenticated packets, LAN-only by default, a scoped firewall rule, direction flags, size and rate limits, no remote console, no raw game memory or code addresses on the wire, no files named by peers |
| Peers with different plugins or versions | Type tables negotiated at join; `SCO_UNAVAILABLE` per type; the protocol version is checked in the hello |
| Desync between peers' views | The game's netcode stays authoritative for entities. Shared things are spawned only by the host, and features ask the host instead of spawning locally |
| Validation relaxations seen as cheating aids | Separate capabilities, host-controlled, only in a joined session, never without the anti-cheat-absent check (open question 2) |
| Bridges launching other programs or sharing memory | Off by default at build time and run time; channels are user-only, per plugin, size-capped and released on unload |
| Network thread or handler faults | Guarded callouts per plugin. The network thread never calls plugin code |

## 6. Plan: pull requests in order

sco-core first, then sc-offline. "In game" means checks the maintainer runs, named in each PR from sc-offline's `docs/features.md` plus the multiplayer additions.

| # | Repo | PR | Done when |
|---|---|---|---|
| 1 | both (docs) | Scope rules: the section 1 wording in sco-core `README.md`, `CONTRIBUTING.md`, `sdk/docs/plugin-rules.md`, `docs/framework.md` (Goal, Decisions), and sc-offline `README.md`, `CONTRIBUTING.md` | CI green in both; the maintainer approves the wording |
| R1 | sco-core (docs) | Research, read-only. On 4.10.196, with the fork's dedicated server and client (built locally by the maintainer, not by CI) and **the anti-cheat step left out**, does a client join? Record the result here | The result is written into this doc. If the join fails, the plan stops after PR 5 for the session pieces, pending open question 1 |
| 2 | sco-core | `sco::net` core: `src/net/` (Winsock and POSIX socket layers, `Core`, reliable layer, HMAC-SHA-256, handshake, limits), `include/sco/net.h`; the in-process tests with two or more cores and the lossy transport; CONTRIBUTING's portable-files list | CI green on all jobs, including TSan |
| 3 | sco-core | The `sco.net` 1.0 service: `include/sco_net.h`, `tests/abi_net.c`, the events, the game-thread dispatch in `sco::app::Tick`, `scosdk/net.hpp`, `Sco.Sdk` `Net` with its pin, `sdk/examples/net_echo`, the `sco-host-sim` options and the two-process CTest; `docs/net.md` and `api-v1.md` § host-owned services | CI green on Linux and Windows; the two-process test passes in CI; the SDK zip builds and checks `net_echo` |
| 4 | sco-core | `sco.ipc` 1.0: `include/sco_ipc.h`, `include/sco_ipc_wire.h`, `tests/abi_ipc.c`, `src/ipc/`, the two-process and TSan tests, scosdk and C# wrappers, `docs/ipc.md` | CI green, including the second-process test on Windows |
| 5 | sco-core | `sco::hook::MidHook` with tests; the `game.post_update` row and the `frame` event | CI green; `sco-sigcheck` on 4.10.196 shows `game.post_update` OK |
| 6 | sco-core | `sco::game::net`: the rows of section 3.2 byte for byte (first commit), fixes for 4.10.196 (second commit), `net_hooks.cpp`, `include/sco/game/net.h`, the capabilities; the anti-cheat step not included. The flymode and validation pieces only as open question 2 decides | CI green; `sco-sigcheck` on 4.10.196 shows every `net.*` row OK, pasted in the PR |
| 7 | sc-offline | Bump sco-core. `builtins/multiplayer`: session control, admission, join state machine, layout, visibility, the Multiplayer tab. The launcher's `host` and `join`, passphrase, scoped firewall rule (listed and undone, per CONTRIBUTING). No domain features yet | CMake MSVC build and `tools/check.sh` green. In game: offline checklist unchanged with multiplayer off; two PCs (or one PC with the server plus a second PC): host, join with the right passphrase, refused with a wrong one, both players see each other, "Go to" works, leave and rejoin |
| 8 | sc-offline | `spawn` and `crew` on `sco.net`: spawn requests, ATC retrieval, seat jobs, seat actions | In game: a joiner spawns a ship and is seated, the other player sees it; ASOP retrieval works for the joiner; offline spawn checklist unchanged |
| 9 | sc-offline | `npc`, `build`, `loadout`, `ammo`, player states | In game: each feature from a joiner shows up for both players; gear seen by the other player; offline checklists unchanged |
| 10 | sc-offline | `contracts` on `sco.net` with semantic objective messages; wallets on `sco.storage` | In game: a joiner takes a contract, sees its objectives and markers, completes it, gets paid; the host's contracts unchanged; offline checklist unchanged |
| 11 | sc-offline | `builtins/titanlink` on `sco.ipc` behind `SC_OFFLINE_BRIDGE_TITANLINK`, with the Northstar side on `sco_ipc_wire.h` (where it lives: open question 9) | CI builds with the option ON and OFF; in game with Titanfall 2 installed: link, pilot mode, titan drop, unlink when either side quits |
| 12 | sc-offline | `builtins/voxel_bridge` on `sco.ipc` behind `SC_OFFLINE_BRIDGE_VOXEL` | CI builds both ways; in game, once the voxel side's source is settled (open question 9) |

PRs 2-5 need no game and can land in any order after PR 1. PR 6 needs the game exe only for `sco-sigcheck`. Each sc-offline PR moves the submodule pin.

## Open questions

1. **If R1 shows joining fails without the anti-cheat step,** what then? This design proposes no workaround. The options are to stop the server-based join, or for the maintainer to decide on a different approach after their own review.
2. **The validation relaxations** (`net.flymode`; `pl_staging.forceClientValidation`; the validation wait): port them as host-controlled capabilities as proposed, or leave them out?
3. **Network reach by default:** LAN only, with VPN ranges on request (proposed), or also any address behind an explicit setting? And which VPN ranges? The fork's launcher mentions Radmin VPN.
4. **Encryption:** is authenticated but unencrypted traffic acceptable for 1.0 (LAN or VPN), or should sessions be encrypted? Encryption would mean a crypto dependency (Windows CNG on Windows, something portable for the tests), which CONTRIBUTING asks to discuss first.
5. **License of `sco_ipc_wire.h`** for the other side of a bridge (a Northstar plugin, a game mod): GPL-3.0 like the rest, or a permissive license for that one header?
6. **Lua and `sco.net`:** none in 1.0 (proposed), or a sandboxed subset (messages only, inside the step budget) later?
7. **Remote console:** drop it (proposed), or an allowlist of CVars the host approves?
8. **Bridge builds:** in the regular release with the bridges off at run time, or only in a separate build?
9. **Where the other sides live:** TitanLink's Northstar plugin and mod (the fork's `TitanLink/`), and the Minecraft-side program, which isn't in the download at all. In sc-offline, in their own repos, or outside the project?
10. **TitanLink's local-match settings** in Titanfall 2 (`ns_auth_allow_insecure 1`, `sv_cheats 1`, master-server reporting off): acceptable under the new scope rule for a local, unlisted match?
11. **Listen servers:** the fork supports only a dedicated server process. Should a player's own game host directly (no second process)? That would need its own research.
12. **The `entity` field in `sco_net_peer_info`:** keep it game-specific in a game-agnostic table (proposed: it's an opaque id the product sets, 0 in products without one), or move it to a product service?
