# Framework plan: sc-offline on the sco-core SDK

**Status: proposal for review.** Nothing on this page is built yet unless it says so. It records where sco-core and sc-offline stand, the decisions taken so far, and the order of work that turns sc-offline into a host for mods of any kind. Each phase lands as its own pull requests; this page changes as decisions are made.

## Goal

One framework for offline, single-player Star Citizen mods. sc-offline is the host: it injects, patches the game offline, hooks the main thread and draws the menu. Everything a mod does goes through the versioned plain-C API in [`sco_api.h`](api-v1.md): commands, events, capabilities, status and log. sc-offline's own features use the same surface as third-party plugins, so the API is exercised by the code that ships, not only by examples.

The [scope rules](../sdk/docs/plugin-rules.md) don't change: offline and single-player only, no network, nothing that helps online play, cheating or anti-cheat bypass.

## Where things stand

**sco-core** (`main` at `addd37f`):

- Game core: scanners, the signature registry and its `[core]` report, `teleport.*` rows, `sco-sigcheck`.
- Runtime: game-thread task queue, event bus, command registry, owners and `Release`, crash containment for every plugin callout (`sco::SetCalloutGuard`, `sco::plugins::ContainCallouts`).
- Host: capabilities, the `sco_api` table, per-plugin handles.
- Plugins: discovery and `plugin.ini`, the native loader, sco-lua (sandboxed Lua 5.4.8), the data-pack content index.
- SDK: template, examples, CMake helper, `sco-plugin-check`, `lua-check.lua`, the packaged zip.
- Builds: `tools/test.sh` and a root CMake build; CI on Linux (ASan, UBSan, TSan) and Windows MSVC x64.

**sc-offline** (`main` at `fe0a61a`, 0.7.0) doesn't use sco-core yet:

- No submodule. `src/common.cpp` keeps its own copies of the scanners sco-core took over, and `src/teleport.cpp` still scans for the four addresses that are `teleport.*` rows in sco-core.
- About a dozen features follow one pattern: `Resolve<Feature>Api(g_text, g_rdata)` scans at startup (`StartOffline` in `src/dllmain.cpp`), `Process<Feature>()` runs from `OnMainThreadTick`, a `WH_GETMESSAGE` hook throttled to 100 ms. Readiness is reported as `[+]`/`[!]` lines in `LogStartup`.
- The ImGui menu (`src/menu.cpp`, tabs Player, Travel, Vehicles, Crew, NPCs, Build, Squadron 42, Menu) calls about 60 `Menu_*` functions declared in `src/menu.h`.
- The build is MSBuild: `sc-offline.slnx` with `src/sc-offline-dll.vcxproj` (`dinput8.dll`) and `launcher/sc-offline.vcxproj` (`sc-offline.exe`), v145 toolset, static CRT, C++20, `/W3`, LTCG in Release. CI (`.github/workflows/build.yml`) runs `tools/check.sh` on Linux, then MSBuild on `windows-2025-vs2026`, then the release job on tags.

## Decisions taken

| Decision | Choice |
|---|---|
| How sc-offline builds | Move from MSBuild to **CMake**, consuming sco-core with `add_subdirectory(external/sco-core)` |
| First deliverable | **This plan**, reviewed before any sc-offline code changes |

## Phase 1: sc-offline builds with CMake

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

CI: `build.yml` replaces `msbuild sc-offline.slnx` with configure, build and an artifact path under `build/`. The release job (`manifest.json`, the zip layout, the round-trip check) keeps its inputs, so `tools/release-manifest.py` and its tests don't change. `tools/check.sh` keeps parsing `src/*.cpp` and adds `-I external/sco-core/include` once Phase 2 lands. Visual Studio users open the folder (CMake is native in VS); `.slnx` and both `.vcxproj` files go.

Done when: CI's CMake build produces `dinput8.dll` and `sc-offline.exe`, a tagged pre-release zip has the same file list as 0.7.0, and one in-game run of the launcher and menu works. To check during this phase: how the launcher gets its administrator prompt (embedded manifest or runtime elevation), so the CMake build keeps it.

## Phase 2: wire sco-core into sc-offline

No feature behavior changes; `mod.log` gains the `[core]` and `[plugin]` blocks.

1. Submodule `external/sco-core`, pinned at a sco-core commit; CI checks out with `submodules: true`.
2. `common.cpp`'s scanners are replaced by `sco/scan.h` (same functions, moved from sc-offline in sco-core's first commit); `Log` feeds `sco::SetLogSink`, `SetMenuStatus` becomes `sco::Status` so features and plugins share the status line.
3. Startup in `src/dllmain.cpp`:
   - `StartOffline`: `RegisterGameSignatures` and `ResolveAll(ModuleImage())` before the features resolve; teleport fills its `TeleportApi` from `sco::game::TeleportAddresses(addrs)` instead of scanning.
   - `LogStartup`: each `[+] <feature>: ready` line also sets the capability (`teleport`, `spawn.ship`, `outfits`, ...); `LogSignatureReport(false)`.
   - `RunMainThreadService`, once the message hook is installed: `SetGameThread` (first call on the game thread), `host::BuildApi({ SCO_TITLE })`, dispatch `game.ready`.
   - `OnMainThreadTick`: `sco::GameThreadTick(now)` alongside the `Process*` calls.
   - Plugins, when `plugins = on`: `Discover(data/plugins)`, `ContainCallouts(&list)`, `LoadNative` / `LoadScript` (sco-lua) / `ContentIndex::Build`, `LogReport`.
   - `game.exit` and `UnloadAll`: on the game window closing, from the game thread. Not from `DllMain`, which runs under the loader lock.
4. `plugins = on|off` (default `off`) is read where the DLL reads its other options today (`ReadStartOptions`).

Done when: `mod.log` shows `[core] signatures: N/N OK` and `[plugin] N found, ...`; the in-game checklist in sc-offline's `docs/features.md` passes as before; with `plugins = on` the SDK examples `hello`, `greeter` and `travel_pack` load and answer their commands. At this point sco-core tags `sdk-v1.0.0` and the plugin ABI freezes for major 1.

## Phase 3: sc-offline's features on the SDK

One feature per pull request, each the same moves:

1. Its addresses become rows in sco-core (`src/game/<feature>_sigs.cpp` with a typed accessor), following [Adding a signature](adding-signatures.md): moved byte for byte, then checked with `sco-sigcheck`.
2. Its readiness becomes a capability set from its rows (`caps::SetFromSignatures`) or from the feature's own checks.
3. Its actions become commands with typed arguments (`spawn.ship <class> <height>`, `teleport.save`, `npc.spawn <class> <count>`), registered by the feature as a host owner.
4. Its menu tab calls `invoke` instead of the `Menu_*` function, and greys out from `has()`.

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

When a tab is all commands, plugins' commands appear in the menu the same way, with no menu code per plugin.

## Phase 4: the framework grows

Every addition is a 1.x minor under [Plugin API v1 § Compatibility](api-v1.md#compatibility): new functions at the end of `sco_api`, new fields at the end of their struct, `SCO_API_MINOR` up by one, `tests/abi_v1.c` extended. Each item gets its own design review before code.

| Item | Why | Sketch |
|---|---|---|
| Lists and queries | The menu's ship, gear and NPC lists, and any plugin that offers a choice ([open in api-v1.md](api-v1.md#commands)) | A `query` function that streams rows to a callback, or commands that return a typed list |
| Plugin panels | Plugins with their own UI | A declarative description (labels, buttons bound to commands, argument widgets) the host draws with its ImGui. ImGui itself never crosses the ABI: it would pin plugins to one ImGui version and put drawing outside crash containment |
| More events | Mods that react to the game | `player.spawned`, `zone.changed`, `ship.spawned`, `menu.opened`; each with a documented `data` struct that starts with a size |
| Settings | Per-plugin options in the menu | A `[settings]` section in `plugin.ini` with typed keys; values saved under `data/plugins/<id>/` |
| Dependencies | Plugins built on other plugins | `depends = <id> >= <version>` in `plugin.ini`; load order by dependency; a refusal reason when one is missing |
| Plugin data | State that survives a restart, for Lua too | A small key-value store per plugin, so scripts keep no file access |
| Game services | Plugins that spawn, teleport or query entities | sc-offline's features exposed as capability-gated commands (Phase 3), so no plugin needs a raw game address |
| Mod manager | Players install and switch mods | The launcher lists `data/plugins/`, switches them with the `disabled` file and shows the `LogReport` states |

## Testing

- sco-core: unit tests and CTest on Linux (sanitizers) and Windows MSVC, the ABI pin, the SDK zip built and checked on both. Unchanged.
- sc-offline: `tools/check.sh` and the CMake MSVC build on every pull request. Game code can't run in CI, so every phase that touches runtime paths names its in-game checks in the pull request, from `docs/features.md`.
- The SDK examples are the plugin smoke test in game from Phase 2 on.

## Versions

- sc-offline 0.8.0: Phase 1 and Phase 2.
- sco-core `sdk-v1.0.0`: when Phase 2 has loaded plugins in game; the SDK zip becomes a release asset. From then on, version 1 only grows.

## Open questions

- Release PDBs: keep none (today) or upload `dinput8.pdb` as a separate release asset for crash reports?
- `game.exit`: which signal sc-offline uses for the game closing (window destroyed, process shutdown hook), so `UnloadAll` runs on the game thread.
- Where host features' command prefixes are reserved (`spawn`, `teleport`, ...) so a plugin can't take one before the feature registers.
- Whether the launcher grows the mod manager or a separate tool does.
