# Changelog

sco-core has no releases yet. sc-offline pins a commit; this file lists what each change means for sc-offline.

## Unreleased

- **Plugins** (`sco/plugins.h`, `src/plugins/`, [docs](docs/plugins.md)): `data/plugins/<id>/plugin.ini` discovery and parsing (off/disabled/refused/ready with a reason), the native loader (`LoadLibraryExW` from the plugin folder and System32 only, query/version/name checks, `__try` guard on every call into plugin code; a fault releases the plugin and keeps its DLL mapped), and the data-pack content index (missions, rules, scripts, lists). sc-offline doesn't call it yet. `tools/test.sh` runs `tests/test_plugins.cpp` against real fake plugins; `tools/test-win.sh` runs the same tests as a Windows build under Wine.
- **Runtime** (`sco/runtime.h`, `src/api/`): game-thread task queue (256, in order, `TooMany` when full), event bus (subscribe/unsubscribe from any thread, changes apply from the next dispatch) and command registry (`RegisterCommand`, `ListCommands`, `Invoke` with argument, capability and owner-prefix checks). `GameThreadTick()` drains the queue then dispatches `tick`. sc-offline doesn't call it yet; wiring `OnMainThreadTick` to it is the next sc-offline change.
- `tools/test.sh` also builds and runs `tests/test_runtime.cpp` under ASan+UBSan and ThreadSanitizer.
- **Runtime and ABI review fixes**: a task that ticks or drains no longer corrupts the queue (nested drains are refused); `Release(owner)` removes an owner's subscriptions, commands, queued tasks and queued invokes, for plugin unload; `Unsubscribe` applies at once and the docs give the safe point for freeing `ctx`; `RegisterCommand` copies every string and arg def, takes an owner, and refuses reserved or foreign prefixes; `Bool` args must be 0 or 1; nothing throws. `sco_api.h`: 4-byte enums (`_FORCE32`), `sco_command.arg_def_size` so `sco_arg_def` can grow, `done` and `list_commands` rules spelled out. `tools/test.sh` pins the ABI for `x86_64-pc-windows-msvc` and with `-fshort-enums`, and runs real cross-thread overlap tests under ThreadSanitizer.
- **Plugin ABI 1.0-pre**: `include/sco_api.h`, the plain-C header plugins will include ([reference](docs/api-v1.md)). Declarations only; nothing implements or loads it yet. `tests/abi_v1.c` pins every v1 size, offset, enum value and signature; `tools/test.sh` compiles it as C11 and C++20 (CI now passes `CC=clang`).
- **Docs**: README, [How it works](docs/architecture.md), [Adding a signature](docs/adding-signatures.md), [Checking a game build](docs/sigcheck.md), [API reference](docs/api.md), CONTRIBUTING.md and this changelog.

## 2026-10-08: first commit (`01d579c`)

- Scanners (`FindPattern`, `FindUniquePattern`, `BytesMatch`, `FindCString`, `FindRipLea`, `Rel32`, `RipTarget`) moved from sc-offline's `src/common.cpp`. One behavior change: a pattern longer than 96 bytes now matches nothing instead of matching only its first 96 bytes.
- Signature registry: named rows, dependency order, `Missing` / `Ambiguous` / `Failed` / `Blocked` results, and the `[core] signatures: N/M OK` report.
- Status channel: `sco::Status()` and `sco::GetStatus()` replace sc-offline's `SetMenuStatus`.
- Teleport's four addresses as `teleport.*` rows, moved byte for byte from sc-offline's `src/teleport.cpp`.
- `tools/sco-sigcheck`: runs every table against a `StarCitizen.exe` on disk.
- `tools/test.sh`: 42 unit checks with AddressSanitizer and UBSan.
