# sco-core documentation

Start with the [README](../README.md) for what sco-core is. Pick your path below. Where sco-core and sc-offline stand, where they are heading and what the work so far taught: [Framework plan](framework.md).

## Writing a plugin

| Doc | What |
|---|---|
| [Plugin SDK](../sdk/README.md) | Build the examples, start from the template, check and install a plugin |
| [Plugin API v1](api-v1.md) | `sco_api.h`: exports, the host table, commands, events, capabilities, threading, compatibility, layout |
| [C++ SDK](sdk-cpp.md) | `include/scosdk/`: the C++20 layer over `sco_api.h`: plugin class, commands, services, raw handlers, storage, lifetimes, the exception boundary |
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
| [C++ API reference](api.md) | Every function and type in `include/sco/`, including the host kit (`sco/app.h`), `caps`, `host` and the runtime |
| [Building: sco-host-sim](building.md#sco-host-sim) | Running plugins through the real host kit, without the game |
| [Framework plan § Lessons](framework.md#lessons) | Rules for hosts learned in game: where to call `Stop`, starting the host kit, adopting sco-core's types |

## Game core

| Doc | What |
|---|---|
| [How it works](architecture.md) | Signature rows, results, resolving, the report, startup |
| [Adding a signature](adding-signatures.md) | Moving a feature's addresses into a table, and the rules rows follow |
| [Checking a game build](sigcheck.md) | `sco-sigcheck`: output, exit codes, the patch-day routine |
| [Checking a DataCore file](datacore.md) | `sco-dcb info` and `records`: extracting `Game2.dcb`, the layout check, exit codes |
| [Design: game-file overrides and DataCore patches](design/vfs-datacore.md) | `sco::vfs`, `sco::game::pak`, the DataCore layout, a semantic patcher (with `AddRecord`), `.toml` overrides in data packs and the `sco.datacore` service; decisions of 2026-10-09 |

## Working on sco-core

| Doc | What |
|---|---|
| [Building and testing](building.md) | `tools/test.sh`, CMake and CTest, the Windows run, the SDK zip, CI |
| [Contributing](../CONTRIBUTING.md) | What fits, how to make a change, how it reaches sc-offline, how lessons come back |
| [Changelog](../CHANGELOG.md) | What changed |
