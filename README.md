# sco-core

[![test](https://github.com/scubamount/sco-core/actions/workflows/test.yml/badge.svg)](https://github.com/scubamount/sco-core/actions/workflows/test.yml)
[![cmake](https://github.com/scubamount/sco-core/actions/workflows/cmake.yml/badge.svg)](https://github.com/scubamount/sco-core/actions/workflows/cmake.yml)
[![sdk](https://github.com/scubamount/sco-core/actions/workflows/sdk.yml/badge.svg)](https://github.com/scubamount/sco-core/actions/workflows/sdk.yml)

sco-core is the game-facing core of [sc-offline](https://github.com/scubamount/sc-offline), the Star Citizen offline mod, and the home of its plugin platform: the `sco_api` plugin ABI, the host that implements it, the plugin loader, the sandboxed Lua runtime and the SDK modders build against.

It has two halves:

- **Game core.** Finds the game's code and data by byte pattern. Every game address sc-offline uses is one named row in a signature table (`teleport.to_camera`), resolved once at startup and reported in one block of `mod.log`. The same tables run against a `StarCitizen.exe` on disk, so a new game build can be checked without starting the game.
- **Plugin platform.** Lets features and plugins work through one small, versioned C interface instead of raw game access: a game-thread runtime (tasks, events, commands), capabilities, the host's `sco_api` table, discovery and loading of native, Lua and data-pack plugins, and the SDK.

> **Status: pre-release.** The plugin ABI is `1.0-pre` and can still change until the `sdk-v1.0.0` tag; from then on version 1 only grows. The C++ headers in `include/sco/` are internal and change with sc-offline. sc-offline compiles the game core today; wiring the plugin platform into sc-offline (and switching on plugin loading, off by default) is a separate sc-offline change.

## How it fits together

```
 plugins      native DLL (C)         Lua script            data pack
                   |                      |                     |
                   |                 sco-lua (sandbox)          |
                   |                      |                     |
 ABI        ------ sco_api.h : plain C, versioned, size-prefixed -----
                   |                                            |
 host       sco/host.h   the sco_api table, per-plugin handles   |
            sco/caps.h   capabilities ("teleport" ready?)        |
            sco/plugins.h  discover, plugin.ini, load, crash guard, content index
                   |
 runtime    sco/runtime.h  game-thread tasks, event bus, command registry, owners
                   |
 game core  sco/signatures.h, sco/scan.h, sco/game/*   rows, scanners, report
            sco/log.h, sco/status.h                     mod.log, status line
```

A plugin only ever sees `sco_api.h`. Everything it adds (subscriptions, commands, queued tasks) is owned by its handle, so unloading, refusing or disabling a plugin removes all of it in one call.

## Quick start

### Build and test the core

```sh
git clone https://github.com/scubamount/sco-core.git
cd sco-core
tools/test.sh                                   # every unit test, under ASan+UBSan and TSan (clang or gcc)
```

or with CMake, on any platform including Windows with MSVC:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Details, options and what CI runs: [Building and testing](docs/building.md).

### Check a game build

```sh
tools/sigcheck.sh /path/to/StarCitizen.exe      # exit 0 = every row OK
```

```
[core] signatures: 0/4 OK
[core] FAILED   teleport.to_camera (layout changed at +0x2b0)
[core] BLOCKED  teleport.client_mgr (needs teleport.to_camera)
```

See [Checking a game build](docs/sigcheck.md).

### Write a plugin

The SDK in [`sdk/`](sdk/README.md) has a template, three examples (a native C plugin, a data pack and a Lua script), a CMake helper and two checkers that run a plugin without the game:

```sh
cd sdk
cmake -S . -B build && cmake --build build && cmake --install build --prefix out
out/bin/sco-plugin-check out/data/plugins/hello --invoke hello.wave "Pilot One"
```

CI packages it as `sco-sdk-<version>.zip`. Start with the [SDK README](sdk/README.md) and the [plugin API reference](docs/api-v1.md).

## Repository layout

| Path | What |
|---|---|
| [`include/sco_api.h`](include/sco_api.h) | The plugin ABI: the only header a plugin includes |
| [`include/sco/`](include/sco/) | Internal C++ API used by sc-offline: signatures, scanners, runtime, caps, host, plugins |
| `src/` | The core (`sco_*.cpp`), the runtime (`api/`), the host table (`host/`), the plugin loader (`plugins/`), game tables (`game/`) |
| [`plugins/lua/`](plugins/lua/README.md) | sco-lua: Lua 5.4.8 (vendored, MIT) in a sandbox, built only on `sco_api.h` |
| [`sdk/`](sdk/README.md) | The plugin SDK: template, examples, CMake helper, checkers, packaging |
| `tests/` | Unit tests, the ABI pin (`abi_v1.c`) and plugin fixtures |
| `tools/` | `test.sh`, `test-win.sh` (Windows build under Wine), `sigcheck.sh`, `sco-sigcheck` |
| `docs/` | Everything else: [docs index](docs/README.md) |

Only `src/sco_image_win.cpp`, the Windows halves of the loader and its crash guard are Windows code. Everything else builds and is tested on Linux and macOS, so neither the tests nor `sco-sigcheck` need Windows or the game.

## Documentation

| Doc | For |
|---|---|
| [Docs index](docs/README.md) | Every document, by audience |
| [How it works](docs/architecture.md) | Rows, results, startup, threading, the runtime, limits |
| [Plugin API v1](docs/api-v1.md) | `sco_api.h`: every function, struct and rule of the ABI |
| [Plugins](docs/plugins.md) | Discovery, `plugin.ini`, the native loader and crash containment, Lua, data packs |
| [Plugin SDK](sdk/README.md) | Building, checking and installing plugins |
| [C++ API reference](docs/api.md) | Every function and type in `include/sco/` |
| [Building and testing](docs/building.md) | `tools/test.sh`, CMake, the Windows run, CI |
| [Contributing](CONTRIBUTING.md) | What fits, tests, and how changes reach sc-offline |
| [Changelog](CHANGELOG.md) | What changed |

## Use from sc-offline

sc-offline includes this repository as a git submodule at `external/sco-core` and compiles its sources into the one `dinput8.dll`; there is no second DLL. At startup sc-offline:

1. sends sco-core's log lines to `mod.log` (`sco::SetLogSink`),
2. registers and resolves the game tables (`sco::game::RegisterGameSignatures`, `sco::ResolveAll(sco::ModuleImage())`),
3. starts each feature only if its rows are OK, and
4. writes the report (`sco::LogSignatureReport(false)`).

With the plugin platform wired in, it then sets capabilities from feature readiness (`sco::caps`), builds the `sco_api` table (`sco::host::BuildApi`), and after `game.ready` discovers and loads plugins (`sco::plugins`), driving the runtime from its main-thread tick (`sco::GameThreadTick`). See [How it works § Startup](docs/architecture.md#startup) and [Plugins](docs/plugins.md).

## Scope

sco-core is for an offline, single-player mod. It finds addresses and reports on them, and gives plugins a bounded way to call features; it never connects to anything. Contributions or plugins that help online play, cheating or anti-cheat bypass are out of scope, as in [sc-offline](https://github.com/scubamount/sc-offline/blob/main/CONTRIBUTING.md#what-fits-this-project) and the [plugin rules](sdk/docs/plugin-rules.md).

## License

GPL-3.0, the same as sc-offline. See [LICENSE](LICENSE). Plugins built against `sco_api.h` are GPL-3.0 too; there is no linking exception. Lua is MIT ([`plugins/lua/third_party/lua/LICENSE`](plugins/lua/third_party/lua/LICENSE)).
