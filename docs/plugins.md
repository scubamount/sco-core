# Plugins: discovery, loading and data packs

`sco/plugins.h`, `src/plugins/`. Internal C++ used by the host (sc-offline); plugins themselves only see [`sco_api.h`](api-v1.md).

## The folder

Every plugin is a folder `data/plugins/<id>/` with a `plugin.ini`:

```ini
id = hello              ; [a-z0-9_], 1-31 chars, same as the folder name; also the command prefix
name = Hello            ; shown in the menu and status
version = 1.0.0
author = you            ; optional
api = 1.0               ; sco_api major.minor it needs
kind = native           ; native | lua | data
entry = hello.dll       ; native: the DLL; lua: the main script; data: none
requires = teleport, spawn.ship   ; optional capabilities
```

- `;` or `#` starts a comment at the start of a line or after whitespace. CRLF and a UTF-8 BOM are fine. Unknown keys are ignored, so a later minor can add some.
- Reserved ids: `sco`, `host`, `menu`, `game`.
- `entry` is a bare file name inside the plugin folder: no `/`, `\`, `:` or `..`.
- `plugin.ini` is at most 16 KiB.

## Discovery

`Discover(root, options)` lists every subfolder of `root` that holds a `plugin.ini`, in byte order of the folder name. It never runs plugin code and never opens the entry file. Symlinked folders are skipped. Each plugin ends in one state:

| State | When |
|---|---|
| `off` | `plugins = off` in `sc-offline.ini` (the default). Listed so `status` can show it |
| `disabled` | `data/plugins/<id>/disabled` exists (a file or a folder) |
| `refused: <reason>` | `plugin.ini: <parse error>`, `id 'x' does not match folder 'y'`, `the id belongs to a built-in plugin` (a folder named like one of the host's built-ins, whatever its kind), `built for api M.m` (major differs or minor newer than the host), `entry 'x' not found`, `missing capability 'x'`, `too many plugins` (over 128) |
| `ready` | Passed; the loader, the Lua runtime or the content index takes it |

## Native plugins

`LoadNative(plugin, api, self, options)`, on the game thread:

1. `LoadLibraryExW(entry, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)`: the plugin's own dependencies come from its folder or System32, never the game folder or `PATH`.
2. Resolves `sco_plugin_query`, `sco_plugin_load` and `sco_plugin_unload`; any missing refuses the plugin.
3. `sco_plugin_query()` must return info whose `size` covers `author`, with `api_major` equal to the host's, `api_minor` not newer, and `name` equal to the id.
4. `sco_plugin_load(api, self)`; anything but `SCO_OK` refuses the plugin and releases what it registered.

`self` is the host's owner handle for the plugin; the host never reuses one. On refusal the DLL is closed.

On Windows `LoadLibraryExW` runs under `SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX)`: a file that isn't a valid 64-bit PE (a broken download, a 32-bit DLL) is refused with a reason instead of raising Windows' modal "Bad Image" box over the game. Loading never shows UI.

### Crash containment

Every call into plugin code goes through `Guarded()`: on Windows a `__try/__except` that catches any SEH exception. `query`, `load` and `unload` are guarded by the loader itself. Everything a plugin registers through `sco_api` (event callbacks, `run_on_game_thread` tasks, command functions and `invoke` `done` callbacks) runs from the runtime, so the host installs a runtime callout guard once, right after `Discover`:

```cpp
auto list = sco::plugins::Discover(root, opts);
sco::plugins::ContainCallouts(&list);   // every plugin callout -> CallPlugin(plugin, where, ...)
```

`ContainCallouts` (over `sco::SetCalloutGuard`) finds the plugin whose `self` owns the callout and runs it through `CallPlugin(plugin, where, thunk, ctx)`, where `where` is the event name (`tick`), the command name, `task` or `invoke done`. Host features' own callouts, and calls a plugin makes into itself while its `sco_plugin_load` is still running (already inside the load guard), are called directly. A fault:

- `sco::Release(self)`: its subscriptions, commands, queued tasks and queued invokes are gone;
- the plugin is `crashed` and never called again;
- a command that faulted answers `SCO_CRASHED` (with an empty reply) to whoever invoked it;
- one `[plugin] hello crashed in tick (0xC0000005) and was disabled` line in `mod.log` and a status message;
- the DLL stays mapped, because its code may still be on a stack.

`DllMain` is not guarded: it runs under the OS loader lock, and unwinding out of it would leave the lock held. Keep `DllMain` empty and do the work in `sco_plugin_load`.

This limits damage. It is not a sandbox: stack corruption, `__fastfail` and `/GS` failures end the process, and a native plugin runs with the game's full rights.

### Unloading

`UnloadAll(list, ops, &runtime)` right after `game.exit` (`sco::app::Stop`): plugins last loaded first, [built-ins](#built-in-plugins) after every other plugin. For a native plugin: `sco_plugin_unload()` (guarded; a fault marks the plugin crashed and keeps the DLL), `Release(self)`, `FreeLibrary`. If `Release` fails (out of memory, or `UnloadAll` called off the game thread) the runtime may still hold the plugin's callbacks, so the DLL is never unmapped: the plugin becomes `crashed: release failed: <RESULT>` instead.

`game.exit` is best effort. The host sends it from the game's own quit path (the game's Quit never reaches the message loop as `WM_QUIT`; see [`sco/app.h`](api.md#scoapph-the-host-kit)), but a crash or a killed process never sends it, and nothing unloads then. A plugin must not rely on `game.exit` or `sco_plugin_unload` for durability: save as it goes.

`LoadNative` passes the plugin path to `LoadLibraryExW` as an absolute path, so the host may discover from a relative root such as `data/plugins`.

## Lua plugins

`LoadScript(plugin, api, self, runtime)`, on the game thread, with the script runtime the host links in (sc-offline: [sco-lua](../plugins/lua/README.md)):

1. Reads the entry script (1 MiB at most).
2. Calls the runtime's `load` (guarded like a native call). It runs the script once in a sandbox; an error refuses the plugin with the script's message (`main.lua:3: ...`) and releases what it registered.

A script talks to the host only through the `sco_api` table, like a native plugin, and has no file, OS or network access. It runs under a step budget and a 64 MiB memory cap; past either, or after 3 errors, the runtime disables it (`[<id>] error: script disabled: <why>`). `UnloadAll(list, ops, &runtime)` releases each script, then frees it. What scripts can call: [`sdk/docs/lua.md`](../sdk/docs/lua.md).

## Data packs

`kind = data` packs carry content and never run code. `ContentIndex::Build(list)` indexes every ready pack and marks it `loaded`:

| Kind | Files |
|---|---|
| `mission` | `missions/*.cwmission` |
| `rules` | `rules/*.rules` |
| `script` | `scripts/**.xml` (any depth) |
| `list` | `lists/*.txt` |

Extensions match in any case. Anything else in the folder is ignored, and symlinks are skipped so a pack can't reach outside itself. `scripts/` is read at most 16 folders deep. A pack with more than 4096 matching files is refused (`too many files`), and so is one whose content folders can't be read to the end (`cannot read scripts: ...`), so a pack never loads with only part of its files. `Build` can run again at any time: it re-reads every ready or loaded pack. Features query the index with `Items(kind)`, `Find(kind, "missions/a.cwmission")` (one entry per pack that ships that name, in plugin order) or `FromPlugin(id)`, and read the files themselves.

## Built-in plugins

A feature compiled into the host can be a plugin too: the same three functions a plugin DLL exports, listed in a table instead of loaded from a folder.

```cpp
static const sco::plugins::Builtin kBuiltins[] = {
    { "teleport", TeleportQuery, TeleportLoad, TeleportUnload },   // id, query, load, unload
};
auto p = sco::plugins::FromBuiltin(kBuiltins[0]);                  // kind builtin, state ready
sco::plugins::LoadBuiltin(p, api, sco::host::NewPlugin("teleport"), opts);
```

- No `plugin.ini`: the manifest (name, version, author, api) comes from `sco_plugin_query`. `FromBuiltin` refuses an id that isn't `[a-z0-9_]`, 1-31 characters and unreserved, or a null function.
- `LoadBuiltin` runs `LoadNative`'s checks without a module: `size` covers `author`, the api major matches and the minor isn't newer, `name` equals the id; then `sco_plugin_load(api, self)`. Refusals read `built-in built for api 2.0` and `built-in name 'x' does not match id 'y'`.
- Crash containment is the same: `query`, `load` and `unload` are guarded, and `ContainCallouts` guards every callout it owns, so a built-in that faults is `crashed` and the rest keep running.
- `UnloadAll` unloads built-ins after every other plugin: they are the product's own features, which other plugins may still call while they unload.
- A built-in talks to other plugins only through `sco_api`. Unlike an external plugin it may also read sco-core's C++ headers (signature rows, scanners), since it ships and is tested with the core.
- The report lists it as `builtin`: `teleport 1.0.0 builtin loaded`. Hosts normally don't call these directly: `sco::app::Start` ([API](api.md#scoapph-the-host-kit)) loads built-ins before any discovered plugin.

## Status

`Describe(plugin)` gives one line, `hello 1.0.0 native loaded` or `pack 1.0.0 data refused: built for api 2.0`; `LogReport(list, enabled)` writes `[plugin] N found, L loaded (plugins = on)` and one line per plugin.

## Tests

- `tools/test.sh` runs `tests/test_plugins.cpp` on the host under ASan+UBSan: manifest rules, discovery over `tests/fixtures/plugins/tree/`, the content index, and the loader against real shared libraries built from `tests/fixtures/plugins/native/fake_plugin.c`, one per behavior. Off Windows the crash guard is a signal handler the test installs.
- `tools/test.sh` also runs `tests/test_app.cpp`: built-in plugins through the loader (the same checks, crash containment, built-ins unloaded last) and through `sco::app` with the fake plugins, `greeter` and `travel_pack`; then `sco-host-sim` over the SDK examples.
- `tools/test-win.sh` builds the same tests and plugins for Windows with mingw (the guard with clang, since GCC has no `__try`) and runs them under Wine: the real `LoadLibraryExW` and SEH path.
