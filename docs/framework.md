# Framework plan: sco-core as the heart, sc-offline on the SDK

**Status: Phases 1 to 3 done, Phase 4 in progress.** This page records where sco-core and sc-offline stand, the decisions taken so far, what the finished phases taught ([Lessons](#lessons)), and the order of work that makes sco-core the core of a framework for mods of any kind, with sc-offline as the first product built on it. Each phase lands as its own pull requests; this page changes as decisions are made.

| Phase | State | Landed in |
|---|---|---|
| [1. sc-offline builds with CMake](#phase-1-sc-offline-builds-with-cmake) | Done | sc-offline PR #55, merged as `88e7830` |
| [2. The host kit in sco-core](#phase-2-the-host-kit-in-sco-core) | Done | sco-core PR #8, merged as `94ba952` |
| [3. sc-offline runs on the host kit](#phase-3-sc-offline-runs-on-the-host-kit) | Done, played in game 2026-10-09 | sc-offline PR #56, merged as `78756af` |
| [4. Features become built-in plugins](#phase-4-sc-offlines-features-become-built-in-plugins) | In progress | |
| [5. Services and storage](#phase-5-services-and-storage) | Planned | |
| [6. The framework grows](#phase-6-the-framework-grows) | Planned | |

## Goal

sco-core is the heart: the runtime, the plugin system, the services and the SDK that every mod is built from. A product such as sc-offline is a thin **bootstrap** (inject, patch the game offline, find the main thread, draw a menu) plus a set of **plugins**. sc-offline's own features become plugins built into its DLL, using the same plain-C API ([`sco_api.h`](api-v1.md)) as third-party plugins, so the API is exercised by the code that ships. Another mod suite is another set of plugins on the same core, with sc-offline's bootstrap or its own.

The [scope rules](../sdk/docs/plugin-rules.md) don't change: offline and single-player only, nothing connects to anything, nothing that helps online play, cheating or anti-cheat bypass.

## Where things stand

**sco-core** (`main` at `94ba952`):

- Game core: scanners, the signature registry and its `[core]` report, `teleport.*` rows, `sco-sigcheck`.
- Runtime: game-thread task queue, event bus, command registry, owners and `Release`, crash containment for every plugin callout (`sco::SetCalloutGuard`, `sco::plugins::ContainCallouts`).
- Host: capabilities, the `sco_api` table, per-plugin handles.
- Plugins: discovery and `plugin.ini`, the native loader, sco-lua (sandboxed Lua 5.4.8), the data-pack content index, built-in plugins.
- Host kit: `sco::app::Start`/`Tick`/`Stop` and `sco-host-sim` (Phase 2).
- SDK: template, examples, CMake helper, `sco-plugin-check`, `lua-check.lua`, the packaged zip.
- Builds: `tools/test.sh` and a root CMake build; CI on Linux (ASan, UBSan, TSan) and Windows MSVC x64.

**sc-offline** (`main` at `78756af`; last release 0.7.0) runs on sco-core:

- sco-core is a submodule at `external/sco-core`, pinned at `94ba952`. The scanners, `Log` and the status line come from sco-core; teleport reads the `teleport.*` rows instead of scanning.
- `StartHostKit` in `src/dllmain.cpp` calls `sco::app::Start` on the game thread from the first main-thread tick, `OnMainThreadTick` calls `sco::app::Tick`. No built-ins yet. `plugins = on|off` in `sc-offline.ini`, off by default.
- The build is CMake (Phase 1): `dinput8.dll` and `sc-offline.exe`, static CRT, x64; CI checks the binaries (`DirectInput8Create` forwarded at ordinal 1, x64, static CRT, launcher `asInvoker`, SegmentHeap).
- About a dozen features follow one pattern: `Resolve<Feature>Api(g_text, g_rdata)` scans at startup (`StartOffline` in `src/dllmain.cpp`), `Process<Feature>()` runs from `OnMainThreadTick`, a `WH_GETMESSAGE` hook throttled to 100 ms. Readiness is reported as `[+]`/`[!]` lines in `LogStartup`.
- The ImGui menu (`src/menu.cpp`, tabs Player, Travel, Vehicles, Crew, NPCs, Build, Squadron 42, Menu) calls about 60 `Menu_*` functions declared in `src/menu.h`.
- Feature state lives in loose text files under `data/` (`ships.txt`, `outfits.txt`, `wallet.txt`, saved spots).

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

**Status: in progress.** First come the two Phase 3 fixes: the `system.quit` signature row for `CSystem::Quit` (sco-core PR #9, merged), which sc-offline hooks to call `sco::app::Stop` (sc-offline PR #58), and starting the host kit whatever teleport does (sc-offline PR #57, which also makes teleport the first built-in plugin). Then the features, in the order below.

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

The first additions to the ABI: `sco_api` 1.1. **Landed early (sco_api 1.1):** `SCO_FAILED` (issue #12); services, `provide_service(self, name, version, vtable)` / `query_service(name, min_version, out)` / `release_service`, with direct tables ([API v1 § Services](api-v1.md#services-11)); raw handlers, `register_raw` / `invoke_raw`, for byte-in byte-out calls ([API v1 § Raw handlers](api-v1.md#raw-handlers-11)); and outside the ABI `sco/hook.h`, the detour engine sc-offline's hooks move onto. Storage and settings are still to come. Additions follow [Plugin API v1 § Compatibility](api-v1.md#compatibility): new functions at the end of `sco_api`, `SCO_API_MINOR` up by one, `tests/abi_v1.c` extended, each with its own design review before code.

### Storage

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
| Plugin panels | Plugins with their own UI | Contributions to the frontend model: labels, buttons bound to commands, argument widgets, settings |
| More events | Mods that react to the game | `player.spawned`, `zone.changed`, `ship.spawned`, `menu.opened`; each `data` struct starts with a size |
| Plugin dependencies | Plugins built on other plugins | `depends = <id> >= <version>`, on the same load-order machinery as `uses` |
| Game services | Plugins that spawn, teleport or query entities | sc-offline's built-in features as capability-gated commands and services, so no plugin needs a raw game address |
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

## Testing

- sco-core: unit tests and CTest on Linux (sanitizers) and Windows MSVC, the ABI pin, the SDK zip built and checked on both. From Phase 2 on, `sco-host-sim` runs the SDK examples and built-in plugins through the real host in CI. Storage: each backend against the same test suite, plus the SQLite hardening (refused `ATTACH`, `PRAGMA`, oversized statements, quota, cancelled queries).
- sc-offline: `tools/check.sh` and the CMake MSVC build on every pull request; built-in plugins that don't need the running game also run under `sco-host-sim`. Game code can't run in CI, so every phase that touches runtime paths names its in-game checks in the pull request, from `docs/features.md`.

## Versions

- sc-offline 0.8.0: Phase 1 and Phase 3 (both on sc-offline's `main`, not yet in a tagged release).
- sco-core `sdk-v1.0.0`: when Phase 3 has loaded plugins in game (done 2026-10-09; not tagged yet); the SDK zip becomes a release asset. From then on, version 1 only grows; Phase 5 is 1.1.

## Open questions

Answered since the plan was written: the `game.exit` signal (the game's quit path, [lesson 1](#1-the-game-quits-without-wm_quit)) and where host features' command prefixes are reserved (by the built-in plugins, which load and register first; see [Decisions taken](#decisions-taken)).

- Release PDBs: keep none (today) or upload `dinput8.pdb` as a separate release asset for crash reports?
- Whether the launcher grows the mod manager or a separate tool does.
- Storage: whether plugins may share data (a read-only view of another plugin's keys, granted in `plugin.ini`) or only through services.
- Services: decided for 1.1, direct tables (faster; a fault in the provider is attributed to the caller). Host-interposed trampolines can be added later as an opt-in per service.
