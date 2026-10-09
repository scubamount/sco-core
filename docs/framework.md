# Framework plan: sco-core as the heart, sc-offline on the SDK

**Status: Phases 1 to 3 done; Phase 4 in review (sc-offline PRs #63 to #66, waiting on the in-game run); most of Phases 5 and 6 landed; the SDK is released as `sdk-v1.1.0`.** This page records where sco-core and sc-offline stand, the decisions taken so far, what the finished phases taught ([Lessons](#lessons)), and the order of work that makes sco-core the core of a framework for mods of any kind, with sc-offline as the first product built on it. Each phase lands as its own pull requests; this page changes as decisions are made.

| Phase | State | Landed in |
|---|---|---|
| [1. sc-offline builds with CMake](#phase-1-sc-offline-builds-with-cmake) | Done | sc-offline PR #55, merged as `88e7830` |
| [2. The host kit in sco-core](#phase-2-the-host-kit-in-sco-core) | Done | sco-core PR #8, merged as `94ba952` |
| [3. sc-offline runs on the host kit](#phase-3-sc-offline-runs-on-the-host-kit) | Done, played in game 2026-10-09 | sc-offline PR #56, merged as `78756af` |
| [4. Features become built-in plugins](#phase-4-sc-offlines-features-become-built-in-plugins) | In review: teleport and spawn merged; the other features, storage, the menu shell and the quantum drive on `sco::game::pak` in sc-offline PRs #63 to #66 | sc-offline PRs #57, #60 (merged), #63 to #66 (open) |
| [5. Services and storage](#phase-5-services-and-storage) | Mostly landed: `sco_api` 1.1 services and raw handlers, host-owned services, `sco.storage` (C, C++, C#, Lua). Open: settings, `uses =` load order | sco-core PRs #14, #15, #24, #32 |
| [6. The framework grows](#phase-6-the-framework-grows) | Partly landed: `sco.ui` (panels, hotkeys), game-file overrides and DataCore patches (`sco::vfs`, `sco::datacore`, `sco::game::pak`, data packs, `sco.datacore`), the C# layer. Open: lists, declarative widgets, more events, dependencies, developer reload, mod manager | sco-core PRs #21 to #31, #33, #34 |

## Goal

sco-core is the heart: the runtime, the plugin system, the services and the SDK that every mod is built from. A product such as sc-offline is a thin **bootstrap** (inject, patch the game offline, find the main thread, draw a menu) plus a set of **plugins**. sc-offline's own features become plugins built into its DLL, using the same plain-C API ([`sco_api.h`](api-v1.md)) as third-party plugins, so the API is exercised by the code that ships. Another mod suite is another set of plugins on the same core, with sc-offline's bootstrap or its own.

The [scope rules](../sdk/docs/plugin-rules.md) don't change: offline and single-player only, nothing connects to anything, nothing that helps online play, cheating or anti-cheat bypass.

## Where things stand

**sco-core** (released as `sdk-v1.1.0`):

- Plugin ABI: `sco_api.h` 1.1 (commands, events, tasks, capabilities, services, raw handlers, `SCO_FAILED`), pinned by `tests/abi_v1.c`; stable, version 1 only grows.
- Language layers over it: C, C++20 (`include/scosdk/`), C# (`sdk/csharp/Sco.Sdk`, NativeAOT), Lua 5.4 (sco-lua, sandboxed), data packs.
- Host-owned services under the reserved id `sco`: `sco.storage` (SQLite), `sco.ui` (tabs, overlays, hotkeys), `sco.datacore` 1.1 (DataCore patches, saved for the next launch after the load). Each has its own pinned header.
- Runtime: game-thread task queue (a fixed ring plus an overflow list), event bus, command registry, owners and `Release`, crash containment for every plugin callout.
- Host and host kit: capabilities, the `sco_api` table, discovery and `plugin.ini`, the native loader, built-in plugins, `sco::app::Start`/`Tick`/`Stop`, `sco-host-sim`.
- Engine side for products: `sco/hook.h` (detours with length decoding, far jumps, `Transaction`, slot swaps), `sco/engine/` (64-bit math, the zone tree), `sco::vfs`, `sco::datacore` (parser, patcher, AddRecord, `.toml` packs), `sco::game::pak` (the game's DataCore load), signature rows and `sco-sigcheck`, `sco-dcb`.
- SDK: `sco-sdk-1.1.0.zip` with the headers, a template, seven examples, the CMake helper and `sco-plugin-check`; every example built and checked from the zip on Linux and Windows in CI.
- Builds: `tools/test.sh` and CMake; CI on Linux (ASan, UBSan, TSan) and Windows MSVC x64; tag-triggered releases (`release` workflow).

**sc-offline** (`main` runs on sco-core; last release 0.7.0):

- sco-core is a submodule at `external/sco-core`. Every detour runs on `sco::hook`; teleport (with the `teleport.spatial` service) and spawn (`spawn.ship`, `spawn.entities`) are built-in plugins; the game's own quit path stops the host kit. Played in game 2026-10-09.
- In review (PRs #63 to #66, tested together through the do-not-merge build #67): crew, NPCs, loadout, ammo, quantum and travel, build mode and contracts as built-ins; saved spots, bookmarks and the wallet on `sco.storage`; the menu as a shell over `sco.ui` with each built-in drawing its own tab and the keys on the hotkey registry; the quantum drive's game data through `sco::game::pak`.
- Not yet: the quantum drive as a data pack (design PR 9), feature signatures as sco-core rows (Phase 4 step 1).

## Decisions taken

| Decision | Choice |
|---|---|
| How sc-offline builds | Move from MSBuild to **CMake**, consuming sco-core with `add_subdirectory(external/sco-core)` |
| First deliverable | **This plan**, reviewed before any sc-offline code changes |
| Host features' command prefixes | **Reserved by built-in plugins**: built-ins load before any discovered plugin and register their commands first, so each owns its id as a command prefix: a plugin folder with the same id gets `SCO_BAD_ARG` from `register_command` for any command under that prefix (and is refused if it fails its load on that). A disabled or refused built-in leaves its prefix free; discovery doesn't yet refuse a folder by id alone |
| Signal for `game.exit` | **The game's own quit path**, on the game thread, not `WM_QUIT` and never `DLL_PROCESS_DETACH` ([lesson 1](#1-the-game-quits-without-wm_quit)) |
| Storage engines | **Local and embedded only** (memory, files, SQLite and similar). Network databases (Postgres, MySQL, Redis, ...) are out of scope: sco-core never connects to anything |

## Architecture

```text
 product      sc-offline bootstrap: dinput8 proxy, offline patches, anti-cheat check,
              main-thread hook, ImGui frontend              (another product: its own bootstrap)
                   |  sco::app::Start / Tick / Stop
 sco-core   +------+----------------------------------------------------------------+
 (the heart)| host kit     startup order, built-in plugins, plugin manager, frontend model |
            | services     store (kv + sql), settings, game services, plugin-provided ones |
            | sco_api.h    the versioned plain-C ABI every plugin uses                     |
            | runtime      tasks, events, commands, owners, crash containment              |
            | game core    signatures, scanners, report                                    |
            +------+----------------------------------------------------------------+
                   |
 plugins      built-in (compiled into the product)   native DLL   Lua script   data pack
```

- **Host kit.** The startup sequence every product needs lives in sco-core once, instead of in each product's `dllmain.cpp`: log sink, signatures, capabilities, the `sco_api` table, built-in plugins, discovered plugins, the content index, the report, `game.ready`; then the tick; then `game.exit` and unload. The product supplies only what is specific to it (sketch):

  ```cpp
  namespace sco::app {
  struct Platform {
      const char* hostVersion;                    // "sc-offline 0.8.0"
      std::filesystem::path dataRoot;             // data/
      bool pluginsEnabled;                        // plugins = on|off
      const plugins::Builtin* builtins; size_t nBuiltins;
      const plugins::ScriptRuntime* scripts;      // sco-lua, or nullptr
  };
  bool Start(const Platform&);   // game thread, after the product's own patches
  void Tick(uint32_t nowMs);     // from the product's main-thread hook
  void Stop();                   // game.exit; unload newest first, built-ins last
  }
  ```

- **Built-in plugins.** A feature compiled into the product exports the same three functions as a plugin DLL (`sco_plugin_query`, `sco_plugin_load`, `sco_plugin_unload`) and is listed in a table instead of loaded from a folder. The loader runs the same checks and the same crash guard, and the report lists it as `builtin`. A built-in plugin talks to other plugins only through `sco_api`; unlike an external one it may also read sco-core's internal C++ headers (signature rows, scanners), because it ships and is tested with the core. Moving a built-in out to its own DLL is a build change once the game access it needs is a service.
- **Services.** Functionality one part provides to others behind a versioned table: the host provides storage, settings and game services; plugins can provide their own later ([Phase 5](#phase-5-services-and-storage)).
- **Headless host.** `sco-host-sim`: the real host kit, runtime and loader outside the game. Signatures resolve against a `StarCitizen.exe` on disk (as `sco-sigcheck` does) or a synthetic image; capabilities come from flags; ticks and events come from a script; commands are invoked from the command line. It replaces the stand-in host in `sco-plugin-check` with the real one, and lets CI load built-in and external plugins on Linux and Windows.
- **Frontend model.** The menu draws a model (tabs, panels, controls bound to commands and settings) that built-in and external plugins contribute to. The product renders it; sc-offline with ImGui. ImGui never crosses the ABI.

## Phase 1: sc-offline builds with CMake

**Status: done** in sc-offline PR #55, merged as `88e7830`. CI verifies the binaries: `DirectInput8Create` forwarded at ordinal 1, x64, static CRT, the launcher's manifest `asInvoker`, SegmentHeap. The plan below is kept as written.

No behavior change. The DLL and the launcher built by CMake replace the MSBuild ones in the release zip.

Targets:

| Target | Type | Sources | Notes |
|---|---|---|---|
| `imgui` | static | `src/third_party/imgui/*.cpp` (core, tables, widgets, Win32 and DX11 backends) | Vendored; its own warnings off, like sco-core's vendored Lua |
| `dinput8` | shared | `src/*.cpp` | `OUTPUT_NAME dinput8`; the `DirectInput8Create` forwarder keeps its `#pragma comment(linker, "/export:...")`; links `imgui`, `d3d11`, `shlwapi`, `advapi32` |
| `sc-offline` | executable | `launcher/launcher.cpp` | Console subsystem, as today |

Settings that must carry over from the `.vcxproj` files:

| MSBuild | CMake |
|---|---|
| v145 toolset (VS 2026) | Generator `Visual Studio 18 2026 -A x64`; CI keeps `windows-2025-vs2026` |
| `RuntimeLibrary MultiThreaded` (Release) | `CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"` |
| `LanguageStandard stdcpp20`, `ConformanceMode` | `CMAKE_CXX_STANDARD 20`, `/permissive-` |
| `WarningLevel Level3` | `/W3` for sc-offline code; sco-core's own targets keep `/W4 /WX` |
| `WholeProgramOptimization` + LTCG (Release) | `INTERPROCEDURAL_OPTIMIZATION_RELEASE ON` |
| `GenerateDebugInformation false` (Release) | No PDB in the release artifact (or upload it separately; open question) |
| x64 and x86 platforms in `.slnx` | x64 only: sco-core is 64-bit only, and the game is x64 |

CI: `build.yml` replaces `msbuild sc-offline.slnx` with configure, build and an artifact path under `build/`. The release job (`manifest.json`, the zip layout, the round-trip check) keeps its inputs, so `tools/release-manifest.py` and its tests don't change. `tools/check.sh` keeps parsing `src/*.cpp` and adds `-I external/sco-core/include` once sco-core is wired in. Visual Studio users open the folder (CMake is native in VS); `.slnx` and both `.vcxproj` files go.

Done when: CI's CMake build produces `dinput8.dll` and `sc-offline.exe`, a tagged pre-release zip has the same file list as 0.7.0, and one in-game run of the launcher and menu works. To check during this phase: how the launcher gets its administrator prompt (embedded manifest or runtime elevation), so the CMake build keeps it.

## Phase 2: the host kit in sco-core

**Status: done** in sco-core PR #8, merged as `94ba952`. What landed: `sco/app.h` (`sco::app::Start`/`Tick`/`Stop`, `Platform` as sketched above plus `image`, `setCapabilities` and `moduleOps`, the `sco_app` library); built-in plugins in `sco/plugins.h` (`Kind::Builtin`, `Builtin`, `FromBuiltin`, `LoadBuiltin`, unloaded after every other plugin); `sco-host-sim`; `tests/test_app.cpp` and the CTest case `host_sim_examples`, which run on Linux (ASan+UBSan, `tools/test.sh` and CMake) and Windows (MSVC). `Platform` takes `pluginRoot` rather than `dataRoot` until Phase 5 gives the data folder a second use. See [API: sco/app.h](api.md#scoapph-the-host-kit) and [Plugins: built-in plugins](plugins.md#built-in-plugins).

Planned as sco-core only, in parallel with Phase 1; sc-offline adopted it in Phase 3.

1. `sco/app.h`: `Start`, `Tick`, `Stop` over the existing runtime, caps, host and loader, in the order above. Startup failures are reported, not fatal: a product with no signatures still loads plugins that need none.
2. Built-in plugins: `Kind::Builtin` in `sco/plugins.h`, a `Builtin` entry (`id` plus the three functions), loaded through `LoadNative`'s checks without a module, under the same guard. `plugin.ini` isn't needed for a built-in; its manifest comes from `sco_plugin_query`.
3. `game.exit` from `Stop()`, on the game thread, before `UnloadAll`.
4. `sco-host-sim` (`tools/`), plus CTest cases that load the SDK examples and a built-in test plugin through it on Linux and Windows.

Done when: CTest runs `hello`, `greeter`, `travel_pack` and a built-in plugin through `sco::app` and `sco-host-sim` on both CI platforms.

## Phase 3: sc-offline runs on the host kit

**Status: done** in sc-offline PR #56, merged as `78756af`: the sco-core submodule pinned at `94ba952`, scanners, log and status from sco-core, teleport on sco-core rows, the host kit on the game thread, `plugins = off` by default. Played in game by the maintainer on 2026-10-09 on Star Citizen 4.10.193.11644 (CL 12660092): `[core] signatures: 4/4 OK`, teleport on F7/F8, the menu and contracts unchanged; with `plugins = on`, `greeter` (Lua) and `travel_pack` (data) loaded, `[status] greeter: Greeter ready` and `[greeter] teleport is available`. Two things didn't hold as planned: step 3's "the game window closing calls `sco::app::Stop`" (sc-offline used `WM_QUIT`, which the game never sends on Quit, [lesson 1](#1-the-game-quits-without-wm_quit)), and the host kit started only when teleport resolved ([lesson 2](#2-the-host-kit-starts-whatever-any-feature-does)). Both are fixed in Phase 4. The `sdk-v1.0.0` tag hasn't been made yet.

No feature behavior changes; `mod.log` gains the `[core]` and `[plugin]` blocks.

1. Submodule `external/sco-core`, pinned at a sco-core commit; CI checks out with `submodules: true`.
2. `common.cpp`'s scanners are replaced by `sco/scan.h` (same functions, moved from sc-offline in sco-core's first commit); `Log` feeds `sco::SetLogSink`, `SetMenuStatus` becomes `sco::Status` so features and plugins share the status line.
3. `src/dllmain.cpp`: the offline patches, the anti-cheat check and the message hook stay; once the hook runs on the game thread it calls `sco::app::Start` (no built-ins yet), `OnMainThreadTick` calls `sco::app::Tick`, the game window closing calls `sco::app::Stop`. Teleport fills its `TeleportApi` from `sco::game::TeleportAddresses(addrs)` instead of scanning.
4. `plugins = on|off` (default `off`), read where the DLL reads its other options today (`ReadStartOptions`).

Done when: `mod.log` shows `[core] signatures: N/N OK` and `[plugin] N found, ...`; the in-game checklist in sc-offline's `docs/features.md` passes as before; with `plugins = on` the SDK examples load and answer their commands. At this point sco-core tags `sdk-v1.0.0` and the plugin ABI freezes for major 1.

## Phase 4: sc-offline's features become built-in plugins

**Status: in review.** Teleport (#57) and spawn (#60) are merged and played in game; every other feature is a built-in in sc-offline PR #63, with storage (#64), the quantum drive on `sco::game::pak` (#65) and the menu shell (#66) stacked on it, waiting on one in-game run of their combined build (#67). Still open after that: step 1 (feature signatures as sco-core rows). History: First come the two Phase 3 fixes: the `system.quit` signature row for `CSystem::Quit` (sco-core PR #9, merged), which sc-offline hooks to call `sco::app::Stop` (sc-offline PR #58), and starting the host kit whatever teleport does (sc-offline PR #57, which also makes teleport the first built-in plugin). Then the features, in the order below.

One feature per pull request, each the same moves:

1. Its addresses become rows in sco-core (`src/game/<feature>_sigs.cpp` with a typed accessor), following [Adding a signature](adding-signatures.md): moved byte for byte, then checked with `sco-sigcheck`.
2. It becomes a built-in plugin: `Resolve<Feature>Api` moves into `sco_plugin_load`, `Process<Feature>` into a `tick` subscription.
3. Its readiness becomes a capability (`caps::SetFromSignatures` or its own checks); its actions become commands with typed arguments (`spawn.ship <class> <height>`, `teleport.save`, `npc.spawn <class> <count>`).
4. Its menu tab draws from the frontend model and runs `invoke`; it greys out from `has()`.
5. Its loose files under `data/` move to the store once [Phase 5](#phase-5-services-and-storage) lands (read once from the old file, then kept in the store).

Order, simplest first and the largest last:

| Step | Feature | Code today |
|---|---|---|
| 1 | Teleport, Travel, Quantum | `teleport.cpp` (rows exist), `travel.cpp`, `quantum.cpp` |
| 2 | Vehicles | `spawner.cpp` (1.7k lines) |
| 3 | Crew | seat control in `spawner.cpp` / `menu.cpp` |
| 4 | NPCs | `npc.cpp` |
| 5 | Player | `loadout.cpp`, `outfits.cpp`, `ammo.cpp`, `cvars.cpp` |
| 6 | Build | `build.cpp` |
| 7 | Missions and Contracts | `missions.cpp`, `contracts.cpp` (2.7k lines) |

When the last tab is done, `dllmain.cpp` is the bootstrap only, and plugins' commands and panels appear in the menu the same way as sc-offline's own.

## Phase 5: services and storage

The first additions to the ABI: `sco_api` 1.1. **Landed early (sco_api 1.1):** `SCO_FAILED` (issue #12); services, `provide_service(self, name, version, vtable)` / `query_service(name, min_version, out)` / `release_service`, with direct tables ([API v1 § Services](api-v1.md#services-11)); raw handlers, `register_raw` / `invoke_raw`, for byte-in byte-out calls ([API v1 § Raw handlers](api-v1.md#raw-handlers-11)); and outside the ABI `sco/hook.h`, the detour engine sc-offline's hooks move onto. **Landed: host-owned services and `sco.storage` 1.0** ([Storage](storage.md)): services the host publishes under the reserved id `sco` ([API v1 § Host-owned services](api-v1.md#host-owned-services)), and per-plugin storage over vendored SQLite as the first one, with `sco_api.h` unchanged. Settings are still to come. Additions follow [Plugin API v1 § Compatibility](api-v1.md#compatibility): new functions at the end of `sco_api`, `SCO_API_MINOR` up by one, `tests/abi_v1.c` extended, each with its own design review before code.

### Storage

**Status: landed as the host service `sco.storage` 1.0** ([docs](storage.md)). What changed from the sketch below, and why:

- **A host-owned service, not new `sco_api` functions.** `query_service` (1.1) already reaches it, so `sco_api.h` and `tests/abi_v1.c` stay as they are; the table has its own header (`sco_storage.h`) and pin (`tests/abi_storage.c`), and versions on its own.
- **One backend, SQLite.** Key-value is a table (`sco_kv`) in the plugin's database; `memory` and `files` backends were not needed by any product. There is no `Backend` interface yet.
- **SQL is synchronous**, with a time budget per call (1 s, then interrupted), instead of a storage thread with `row` / `done` callbacks. Calls work from any thread and are serialized per plugin; a query returns a cursor id, never a pointer. An asynchronous form can be added as a later minor if a plugin needs long queries off the game thread.
- **Where data lives:** `<dataRoot>/storage/<plugin id>.db` (sc-offline: `data/storage/`), set by `Platform::dataRoot`. Still outside the plugin's folder.
- **Durability:** WAL and `synchronous = FULL`; a child process killed mid-transaction keeps the last committed state (`tests/test_storage.cpp`).
- **Lua:** `sco.store` in sco-lua (`get`, `put`, `delete`, `keys`, `begin` / `commit` / `rollback`, `exec`, `sql(sql, params[, fn])`) over the same service, with rows as tables and each call and row counted against the step budget ([Lua reference](../sdk/docs/lua.md#scostore)).

The original plan, kept as written:

**For plugins** (native and Lua), every key and database private to the plugin:

| Function (sketch) | Thread | Does |
|---|---|---|
| `store_put(self, key, data, size)` | Any | Writes a value (bytes). Keys UTF-8, at most 255 bytes; values at most 1 MiB |
| `store_get(self, key, buf, cap, &size)` | Any | Reads a value; `size` gets the full length when `cap` is too small |
| `store_delete(self, key)` | Any | Removes a key; `SCO_NOT_FOUND` when absent |
| `store_list(self, prefix, fn, ctx)` | Any | Calls `fn(key, ctx)` for each key with the prefix, in key order |
| `store_sql(self, sql, params, nparams, row, done, ctx)` | Any | Capability `store.sql`. Runs one statement on the plugin's own database on the storage thread; each row comes to `row(columns, n, ctx)` and the result to `done(result, error, ctx)`, on the game thread. Parameters are bound, never pasted into the SQL |

Lua gets `sco.store.get/put/delete/list` and `sco.store.sql(sql, params, fn)`; scripts still have no file access, and storage calls count against the step budget per row.

**In the host** (`sco/store.h`, C++): a `Backend` interface with key-value operations and an optional SQL part. Built-in backends:

| Backend | Uses | Key-value | SQL |
|---|---|---|---|
| `memory` | Tests, `sco-host-sim` | Yes | No |
| `files` | No dependency: one file per key, written to a temporary file then renamed | Yes | No |
| `sqlite` | SQLite amalgamation, vendored like Lua (public domain), built as C with its own warnings off | Yes (a `kv` table) | Yes |

The product picks the backend in `Platform`; sc-offline uses `sqlite`, which also makes `store.sql` ready.

**Where data lives:** `data/plugin-data/<id>/`, outside the plugin's folder, so updating, reinstalling or disabling a plugin keeps its data. Removing a plugin's data is an explicit action (the mod manager).

**Limits and safety:**

- A quota per plugin (default 64 MiB): byte accounting for `files`, `max_page_count` for SQLite. Over the quota a write answers `SCO_TOO_MANY`.
- SQLite hardened for untrusted SQL: built without extension loading; `SQLITE_DBCONFIG_DEFENSIVE` and `trusted_schema` off; an authorizer that refuses `ATTACH`, `DETACH` and every `PRAGMA` outside a short allowlist; `sqlite3_limit` on statement length, columns and attached databases (0); no URI filenames; a progress handler that stops a statement past its time budget; WAL mode, so a crash leaves a consistent database.
- Threading: key-value calls are short and synchronous under the backend's lock. SQL runs on one storage thread, in order per plugin, so a slow query never stalls the game thread. `Release(owner)` cancels the plugin's queued statements, interrupts a running one, and never calls their `done`, the same rules as a queued `invoke`.

### Settings

A `[settings]` section in `plugin.ini` declares typed keys with defaults (`fly_speed = float 10 1..100`); values live in the plugin's store; the menu draws them from the frontend model. Built-in plugins use the same mechanism instead of `sc-offline.ini` keys of their own.

### Services between plugins

`provide_service(self, name, version, table)` and `get_service(self, name, min_version)`, with `uses = <service> >= <version>` in `plugin.ini` for load order (providers load first and unload last) and a refusal reason when a provider is missing. Service tables start with `uint32_t size` like every ABI struct. The design review settles crash attribution: today a call from one plugin into another's table runs under the caller's guard, so a fault in the provider would disable the caller; the host can interpose a trampoline per service function, as it does for commands. A storage backend provided by a plugin builds on this, once the built-in backends have settled the interface.

## Phase 6: the framework grows

Each item gets its own design review before code; every ABI change is a 1.x minor.

| Item | Why | Sketch |
|---|---|---|
| Lists and queries | The menu's ship, gear and NPC lists, and any plugin that offers a choice ([open in api-v1.md](api-v1.md#commands)) | A `query` function that streams typed rows to a callback, the same row shape as `store_sql` |
| Plugin panels | Plugins with their own UI | **Landed as the host service `sco.ui` 1.0** ([UI](ui.md)): plugins register tabs (with an order and a badge) and overlays with a draw function the product calls during its frame with its own frame context (sc-offline: ImGui), as a guarded callout, so a crashing draw disables only that plugin; hotkeys bind key chords to commands, refused when taken or reserved by the product, dispatched through the command registry. Everything is withdrawn when the plugin unloads. Still to come: declarative widgets (labels, buttons bound to commands, argument widgets, settings) for plugins that don't draw, and tabs from Lua (G018) |
| More events | Mods that react to the game | `player.spawned`, `zone.changed`, `ship.spawned`, `menu.opened`; each `data` struct starts with a size |
| Plugin dependencies | Plugins built on other plugins | `depends = <id> >= <version>`, on the same load-order machinery as `uses` |
| Game services | Plugins that spawn, teleport or query entities | sc-offline's built-in features as capability-gated commands and services, so no plugin needs a raw game address |
| Game-file overrides and DataCore patches | Data mods (sc-offline's quantum drive first) that survive game patches instead of turning off at every one | `sco::vfs` serves virtual game files (base ranges plus replacement bytes) through the engine's file calls; `sco::datacore` turns named record/field overrides from data packs into splices computed from the loaded file's own tables. [Design: vfs-datacore.md](design/vfs-datacore.md). **Landed** (sco-core #21 to #31, #34): `sco::vfs`, the DataCore parser and patcher with AddRecord, `.toml` packs, `sco-dcb`, `sco.datacore` 1.1 and `sco::game::pak`. Still to come: the quantum drive as a pack in sc-offline (design PR 9) and design PR 10 (all game files, replace mounts, the size slot) |
| Developer reload | Faster plugin development | Unload and reload one plugin from the menu or `sco-host-sim`, behind a developer switch |
| Mod manager | Players install and switch mods | The launcher lists `data/plugins/`, switches them with the `disabled` file, shows the `LogReport` states, and removes a plugin's data on request |

## Lessons

What the finished phases taught, each with the rule it produced. **Lessons flow back to sco-core:** every product-side workaround that reflects a sco-core gap becomes a sco-core issue or pull request (docs, API, a test or a signature row) in the same cycle, and gets an entry here. How: [CONTRIBUTING § Lessons flow back](../CONTRIBUTING.md#lessons-flow-back).

### 1. The game quits without WM_QUIT

The game's menu Quit calls `CSystem::Quit` (`Quit via console command`), then `System Fast Shutdown (ExitOnQuit enabled)`: the process ends without the message loop ever getting `WM_QUIT`. sc-offline's `WM_QUIT` hook (Phase 3) therefore never ran `sco::app::Stop`, and plugins never saw `game.exit`.

**Rule for hosts:** call `sco::app::Stop` from the game's own quit path, on the game thread. sco-core's `system.quit` signature row finds `CSystem::Quit` (`sco/game/system.h`, `QuitFunction`), and sc-offline hooks it (sc-offline PR #58). Never call `Stop` from `DLL_PROCESS_DETACH`: it runs under the loader lock, at the wrong time. **Rule for plugins:** `game.exit` is best effort; a crash or a killed process never sends it, so a plugin must not rely on it for durability. See [`sco/app.h`](api.md#scoapph-the-host-kit) and [Plugins § Unloading](plugins.md#unloading).

### 2. The host kit starts whatever any feature does

In sc-offline the main-thread hook, and so the host kit and every plugin, only started when teleport resolved its addresses: a game build that broke teleport would also have switched off every plugin.

**Rule for hosts:** start the host kit regardless of any one feature. A feature that fails reports itself unready (a capability), and the plugins that need it are refused or grey out; the others keep running. Phase 4 fixes sc-offline.

### 3. Adopting sco-core's types: aliases don't share names

When a product swaps its own types for sco-core's, an alias is the smallest change (sc-offline's `Section` became `using Section = sco::Section`). But an alias can't share its name with a function the way a struct could: `menu.cpp` had a `Section()` helper beside `struct Section`, and with the alias that no longer compiles.

**Rule for adoption:** before aliasing, look for functions or variables with the type's name and rename them in the same change. This is why sco-core keeps its names in `namespace sco` instead of the global namespace: a product chooses which names to pull in.

### 4. Building sco-core inside a product

sc-offline adds sco-core with `add_subdirectory(external/sco-core EXCLUDE_FROM_ALL)` after setting `CMAKE_MSVC_RUNTIME_LIBRARY`, so the static CRT reaches sco-core's libraries too (CI checks `dinput8.dll` imports no dynamic CRT). It sets `SCO_BUILD_TESTS OFF` (sco-core's own CI runs them) and `SCO_WERROR OFF` (a warning only a newer toolset emits must not break the product build).

**Rule:** the CMake settings a product must make are documented in [Building § Using sco-core from another CMake project](building.md#using-sco-core-from-another-cmake-project).

### 5. Windows shows a dialog for a bad DLL

`LoadLibraryExW` on a file that isn't a valid PE (a broken or wrong-architecture plugin) raised a modal "Bad Image" box over the game. Fixed in `933701a`: the loader calls it under `SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX)`, so the plugin is refused with a reason instead.

**Rule:** plugin loading never shows UI; every failure is a reason in `mod.log` and `status`. See [Plugins § Native plugins](plugins.md#native-plugins).

### 6. Services hand out ids, never pointers

Friction point 1 in the services spec: what a service should give a caller for "that ship" or "the player". A pointer into the game is the obvious answer and the wrong one: Star Citizen streams objects in and out (object container streaming), so a pointer a plugin keeps across ticks dangles once its object streams out, and the next call through it faults in the caller. The ids are not what they look like either: in the sc-offline spawn test the player's entity id is `0xCAE11A7400000000`, a tagged value, not a small index.

**Rule for services:** take and return entity and zone ids, opaque `uint64_t` values as the game's are; pass them back, never decode them. Resolve the id on every call, on the game thread, and answer `SCO_NOT_FOUND` or `SCO_UNAVAILABLE` when the entity has streamed out. Never hand out a game pointer. See [API § Services](api-v1.md#services-11) and [C++ SDK § Services](sdk-cpp.md#services).

## Testing

- sco-core: unit tests and CTest on Linux (sanitizers) and Windows MSVC, the ABI pin, the SDK zip built and checked on both. From Phase 2 on, `sco-host-sim` runs the SDK examples and built-in plugins through the real host in CI. Storage: each backend against the same test suite, plus the SQLite hardening (refused `ATTACH`, `PRAGMA`, oversized statements, quota, cancelled queries).
- sc-offline: `tools/check.sh` and the CMake MSVC build on every pull request; built-in plugins that don't need the running game also run under `sco-host-sim`. Game code can't run in CI, so every phase that touches runtime paths names its in-game checks in the pull request, from `docs/features.md`.

## Versions

- sc-offline 0.8.0: Phase 1 and Phase 3 (both on sc-offline's `main`, not yet in a tagged release).
- sco-core `sdk-v1.1.0`: the first SDK release, with the plugin ABI at 1.1 (the planned `sdk-v1.0.0` was never tagged: Phase 5's 1.1 additions landed first). The SDK zip is a release asset ([Building § Releases](building.md#releases)). From here on, version 1 only grows.

## Open questions

Answered since the plan was written: the `game.exit` signal (the game's quit path, [lesson 1](#1-the-game-quits-without-wm_quit)) and where host features' command prefixes are reserved (by the built-in plugins, which load and register first; see [Decisions taken](#decisions-taken)).

- Release PDBs: keep none (today) or upload `dinput8.pdb` as a separate release asset for crash reports?
- Whether the launcher grows the mod manager or a separate tool does.
- Storage: whether plugins may share data (a read-only view of another plugin's keys, granted in `plugin.ini`) or only through services.
- Services: decided for 1.1, direct tables (faster; a fault in the provider is attributed to the caller). Host-interposed trampolines can be added later as an opt-in per service.
