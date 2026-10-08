# sco-core

[![test](https://github.com/scubamount/sco-core/actions/workflows/test.yml/badge.svg)](https://github.com/scubamount/sco-core/actions/workflows/test.yml)

sco-core is the part of [sc-offline](https://github.com/scubamount/sc-offline), the Star Citizen offline mod, that touches the game: it finds the game's code and data by byte pattern and reports what it found.

Every game address sc-offline uses is one named row in a signature table here, such as `teleport.to_camera`. Feature code asks for an address by name and never scans for it. At startup sco-core finds each row and writes one report to `mod.log`:

```
[core] signatures: 4/4 OK
```

After a game patch, the report shows which rows broke and which rows can't run because of them:

```
[core] signatures: 0/4 OK
[core] FAILED   teleport.to_camera (layout changed at +0x2b0)
[core] BLOCKED  teleport.client_mgr (needs teleport.to_camera)
[core] BLOCKED  teleport.handle_from_id (needs teleport.to_camera)
[core] BLOCKED  teleport.entity_system (needs teleport.to_camera)
```

The same tables also run outside the game: `tools/sigcheck.sh` reads a `StarCitizen.exe` from disk and reports which rows still match, on macOS or Linux, without starting the game.

> **Status: early.** The API is internal (version 0) and can change in any commit. sc-offline is its only user. Teleport is the first feature moved onto signature rows; the rest of sc-offline still scans for its own addresses and moves over one feature at a time.

## Quick start

```sh
git clone https://github.com/scubamount/sco-core.git
cd sco-core
tools/test.sh                                   # unit tests
tools/sigcheck.sh /path/to/StarCitizen.exe      # check a game build; exit 0 = every row OK
```

You need clang or gcc with C++20. Nothing else: no Windows, no game, no other libraries.

## What's in it

| Part | Header | What it does |
|---|---|---|
| Scanners | [`sco/scan.h`](include/sco/scan.h) | Find byte patterns (`48 8B ?? 05`), C strings and RIP-relative `lea`s in a loaded PE image |
| Signature registry | [`sco/signatures.h`](include/sco/signatures.h) | Named rows, resolved in dependency order, with one result per row and the startup report |
| Game tables | [`sco/game/`](include/sco/game/) | The game's rows, one file per feature area, plus typed accessors such as `TeleportAddresses()` |
| Log | [`sco/log.h`](include/sco/log.h) | Where sco-core's lines go; the host installs a sink |
| Status | [`sco/status.h`](include/sco/status.h) | The one-line message features show the player; features call `Status()`, the menu reads `GetStatus()` |
| PE file loader | [`sco/pe_file.h`](include/sco/pe_file.h) | Host tools only: lays out `StarCitizen.exe` from disk the way Windows would |
| sco-sigcheck | [`tools/sco-sigcheck.cpp`](tools/sco-sigcheck.cpp) | Runs every table against a `StarCitizen.exe` file |

The scanners and the registry are plain C++ with no Windows headers. Only [`src/sco_image_win.cpp`](src/sco_image_win.cpp), which finds the running game's image, is Windows code.

## Documentation

| Doc | For |
|---|---|
| [How it works](docs/architecture.md) | Rows, dependencies, result states, startup order, threading, limits |
| [Adding a signature](docs/adding-signatures.md) | Moving a feature's addresses into a table, step by step, and the rules rows follow |
| [Checking a game build](docs/sigcheck.md) | `sco-sigcheck`: reading its output, exit codes, the patch-day routine |
| [API reference](docs/api.md) | Every function and type in `include/sco/` |
| [Plugin API v1](docs/api-v1.md) | `include/sco_api.h`, the plain-C plugin ABI (1.0-pre, not hosted yet) |
| [Contributing](CONTRIBUTING.md) | What fits, tests, and how changes reach sc-offline |
| [Changelog](CHANGELOG.md) | What changed |

## Use from sc-offline

sc-offline includes this repository as a git submodule at `external/sco-core` and compiles its sources into the one `dinput8.dll`. There's no second DLL and no binary interface between the two. At startup sc-offline:

1. sends sco-core's log lines to `mod.log` (`sco::SetLogSink`),
2. registers the game tables (`sco::game::RegisterGameSignatures`),
3. resolves every row against the running game (`sco::ResolveAll(sco::ModuleImage())`),
4. starts each feature only if its rows are OK, and
5. writes the report (`sco::LogSignatureReport(false)`).

See [How it works § Startup](docs/architecture.md#startup).

## Scope

sco-core is for an offline, single-player mod. It finds addresses and reports on them; it doesn't patch, hook or call game code by itself, and it never connects to anything. Contributions that help online play, cheating or anti-cheat bypass are out of scope, as in [sc-offline](https://github.com/scubamount/sc-offline/blob/main/CONTRIBUTING.md#what-fits-this-project).

## License

GPL-3.0, the same as sc-offline. See [LICENSE](LICENSE).
