# Contributing to sco-core

sco-core is the game-facing core of [sc-offline](https://github.com/scubamount/sc-offline) and the home of its plugin platform (the `sco_api` ABI, the host, the loader, sco-lua and the SDK). Most changes start in sc-offline: a feature moving its addresses here, a game patch breaking a row, or a feature exposing a command to plugins. Read sc-offline's [CONTRIBUTING.md](https://github.com/scubamount/sc-offline/blob/main/CONTRIBUTING.md) first; its rules apply here too.

## What fits

- **Offline, never CIG's servers.** Two kinds of connection are allowed: local IPC with other processes on the same PC (bridges such as Titanfall 2/Northstar or Minecraft), and private co-presence between sc-offline players over a LAN or VPN through `sco.net`. Never: connecting to Star Citizen's servers or online services, public or official online play, getting around anti-cheat or skipping its steps, signature-check bypass, account or entitlement tampering, forcing the game's host type or network context, unauthenticated remote commands between peers, telemetry, or anything that helps cheating in the official game. Full list: [Plugin rules](sdk/docs/plugin-rules.md). PRs like that are closed.
- **No game files.** Byte patterns and the names of strings the game references are fine. Don't commit pieces of `StarCitizen.exe`, dumps or anything extracted from the game.
- **No secrets or personal data** in code, tests or logs you paste.
- **Portable core.** Only `src/sco_image_win.cpp`, `src/ipc/shm_win.cpp`, `src/net/udp_win.cpp` and the `_WIN32` halves of `src/plugins/loader.cpp` and `src/plugins/guard_win.cpp` may include Windows headers. Everything else must build with clang or gcc on macOS and Linux, so the tests and `sco-sigcheck` keep running without Windows, and with MSVC x64, the real target.
- **The plugin ABI only grows.** `include/sco_api.h` follows the rules in [Plugin API v1 § Compatibility](docs/api-v1.md#compatibility). Any change to it updates `tests/abi_v1.c` and `docs/api-v1.md` in the same PR.
- **No new dependencies** without discussing it in an issue first.

## Making a change

1. Open an issue first for anything bigger than a small fix.
2. Follow [Adding a signature](docs/adding-signatures.md) for new rows; its rules (move without changing, one address per row, unique or nothing) are checked in review.
3. Run `tools/test.sh` (or the CMake build and `ctest`; see [Building and testing](docs/building.md)). Every test binary must end with `0 failed`. New behavior gets a test next to its neighbors: registry and scanners in `tests/test_core.cpp`; tasks, events and commands in `tests/test_runtime.cpp`; capabilities and the `sco_api` table in `tests/test_host.cpp`; the C++ SDK layer (`include/scosdk/`) in `tests/test_sdk.cpp`; discovery, `plugin.ini`, the loader and the content index in `tests/test_plugins.cpp`; sco-lua in `tests/test_lua.cpp`. A new source file goes into both `tools/test.sh` and `CMakeLists.txt`.
4. Run `tools/sigcheck.sh` against a real `StarCitizen.exe` if you touched a table, and paste the `[core]` lines and the game build into the PR.
5. Update the docs your change affects (`README.md`, `docs/`, header comments) and add a line to [CHANGELOG.md](CHANGELOG.md).
6. Open the PR against `main`. CI (`test`, `cmake` and `sdk`) must pass.

## Reaching sc-offline

sc-offline pins sco-core as a submodule at `external/sco-core`. After a sco-core change merges, a sc-offline PR moves the pin (`git -C external/sco-core checkout <sha>` then commit `external/sco-core`) along with any code that uses the change. That PR is where the Windows build and in-game testing happen.

## Lessons flow back

A product built on sco-core (sc-offline first) sometimes has to work around something sco-core gets wrong or doesn't say: a missing API, an undocumented rule, a signature it scans for itself. Every such workaround becomes sco-core work in the same cycle, not later:

1. Open an issue or pull request here labelled `lesson`, linking the product change that taught it. A pull request fixes the gap in whichever form fits: docs, an API change, a test, or a signature row.
2. Add an entry to [Framework plan § Lessons](docs/framework.md#lessons): what happened, and the rule it produced for hosts, plugins or adopters.
3. Where the lesson can be checked without the game, add a test that fails without the fix (`tests/`, or a CTest case through `sco-host-sim`). Where it can't (game behavior such as the quit path), name the in-game check in the pull request.

The product change and its sco-core counterpart can merge in either order; the product moves its submodule pin once the sco-core side is in.

## Code style

- C++20 in the style of the surrounding code. Code the DLL compiles stays small: no `std::regex`, iostreams or other heavy headers outside `tools/`.
- Resolvers return `SigFail("<static reason>")` with a reason a maintainer can act on: which check failed, at which offset.
- Comments say why, not what.

## License

GPL-3.0 ([LICENSE](LICENSE)). By opening a pull request you agree your contribution is licensed under GPL-3.0.

**Interface exception:** [`include/sc_ipc.h`](include/sc_ipc.h), the shared-memory wire of `sco.ipc` bridges, is MIT (`SPDX-License-Identifier: MIT` and the MIT text in its header), so a program that isn't GPL, the other side of a bridge, may include it and speak the protocol ([docs/ipc.md](docs/ipc.md)). The same exception covers [`include/sc_net.h`](include/sc_net.h), the datagram wire of `sco.net` sessions (framing, the MAC input, the replay window; [docs/net-wire.md](docs/net-wire.md)), so a program outside sco-core may frame and verify packets. It covers those two files only; contributions to them are licensed under MIT. `include/sco_ipc.h`, `include/sco_net.h`, the services and everything else stay GPL-3.0. Keep both free of any sco-core include, and keep sco-core's own wire code (`include/sco/net/`) on `sc_net.h`'s definitions.
