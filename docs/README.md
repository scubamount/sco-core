# sco-core documentation

Start with the [README](../README.md) for what sco-core is. Pick your path below. Where sco-core and sc-offline stand, where they are heading and what the work so far taught: [Framework plan](framework.md).

## Writing a plugin

| Doc | What |
|---|---|
| [Plugin SDK](../sdk/README.md) | Build the examples, start from the template, check and install a plugin |
| [Plugin API v1](api-v1.md) | `sco_api.h`: exports, the host table, commands, events, capabilities, threading, compatibility, layout |
| [C++ SDK](sdk-cpp.md) | `include/scosdk/`: the C++20 layer over `sco_api.h`: plugin class, commands, services, raw handlers, storage, lifetimes, the exception boundary |
| [C# SDK](sdk-csharp.md) | `sdk/csharp/Sco.Sdk`: the C# layer, published with NativeAOT: setup, plugin class, commands, services, raw handlers, storage, DataCore, UI, lifetimes, threads, the exception boundary, AOT and trimming rules |
| [Storage](storage.md) | The host service `sco.storage`: per-plugin key-value and SQL over SQLite, transactions, crash safety, what SQL may run, limits |
| [UI](ui.md) | The host service `sco.ui`: tabs, overlays and badges in the product's menu, hotkeys bound to commands, chords, conflicts, hosting it |
| [plugin.ini](../sdk/docs/plugin-ini.md) | Every key of the plugin manifest |
| [Data packs](../sdk/docs/data-packs.md) | Missions, rules, scripts and lists without code |
| [Lua plugins](../sdk/docs/lua.md) | The `sco` table, the sandbox and its limits |
| [Plugin rules](../sdk/docs/plugin-rules.md) | What plugins may and may not do, and what the host guarantees |

## Hosting plugins (sc-offline and sco-core developers)

| Doc | What |
|---|---|
| [Plugins](plugins.md) | Discovery, `plugin.ini` checks, the native loader, crash containment, Lua, data packs, status |
| [sco-lua](../plugins/lua/README.md) | The Lua runtime: sandbox, step and memory limits, updating Lua |
| [How it works](architecture.md) | The runtime (tasks, events, commands, owners), threading and limits |
| [Multiplayer messages](net.md) | `sco.net`: channels, quotas, threads, events and the LAN rule, for plugin authors (and the product's session control) |
| [Local IPC](ipc.md) | `sco.ipc` and the MIT wire `sc_ipc.h`: channels, rings, blocks, epochs, heartbeats and the hostile-peer rules, for plugin and bridge authors |
| [sco.net wire format](net-wire.md) | The packets, handshake, keys, replay window and reliable delivery of `sco.net` as built (plan PR 4a), and the MIT wire header `sc_net.h` |
| [C++ API reference](api.md) | Every function and type in `include/sco/`, including the host kit (`sco/app.h`), `caps`, `host` and the runtime |
| [Building: sco-host-sim](building.md#sco-host-sim) | Running plugins through the real host kit, without the game |
| [Framework plan § Lessons](framework.md#lessons) | Rules for hosts learned in game: where to call `Stop`, starting the host kit, adopting sco-core's types |

## Game core

| Doc | What |
|---|---|
| [How it works](architecture.md) | Signature rows, results, resolving, the report, startup |
| [Adding a signature](adding-signatures.md) | Moving a feature's addresses into a table, and the rules rows follow |
  | [ASOP, hangar and ATC rows](game/asop.md) | The 62 rows behind ship terminals, lifts, hangars and ATC, their capabilities and root causes |
  | [Building, missions, cvars and quantum rows](game/world.md) | The 36 rows behind build mode, the mission system, cvar storage and quantum boost, their capabilities |
  | [Contracts rows](game/contracts.md) | The 83 rows and 15 capabilities behind sc-offline's contracts feature |
  | [Features rows](game/features.md) | The rows moved out of sc-offline's five FindPattern scans, and their capabilities |
  | [Offline patches and startup rows](game/offline.md) | The 60 rows and 19 capabilities behind sc-offline's offline patches, boot and startup hooks |
  | [Actors rows](game/actors.md) | The 33 rows behind spawning, NPCs, ammo and the player loadout |
| [Checking a game build](sigcheck.md) | `sco-sigcheck`: output, exit codes, the patch-day routine |
| [DataCore files and packs](datacore.md) | `sco-dcb info`, `records`, `lint`, `check`, `show` and `diff`: extracting `Game2.dcb`, the layout check, the `.toml` pack format, the `sco.datacore` service, exit codes |
| [Design: multiplayer and cross-game bridges](design/multiplayer.md) | The scope change of 2026-10-09, what the fork's multiplayer code does, the `sco.net` and `sco.ipc` services, the `sco::game::net` rows, sc-offline's built-ins and the PR plan; in review |
| [Design: game-file overrides and DataCore patches](design/vfs-datacore.md) | `sco::vfs`, `sco::game::pak`, the DataCore layout, a semantic patcher (with `AddRecord`), `.toml` overrides in data packs and the `sco.datacore` service; decisions of 2026-10-09 |

## Working on sco-core

| Doc | What |
|---|---|
| [Building and testing](building.md) | `tools/test.sh`, CMake and CTest, the Windows run, the SDK zip, CI |
| [Contributing](../CONTRIBUTING.md) | What fits, how to make a change, how it reaches sc-offline, how lessons come back |
| [Changelog](../CHANGELOG.md) | What changed |
