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
| `disabled` | `data/plugins/<id>/disabled` exists |
| `refused: <reason>` | `plugin.ini: <parse error>`, `id 'x' does not match folder 'y'`, `built for api M.m` (major differs or minor newer than the host), `entry 'x' not found`, `missing capability 'x'`, `too many plugins` (over 128) |
| `ready` | Passed; the loader, the Lua runtime or the content index takes it |

## Native plugins

`LoadNative(plugin, api, self, options)`, on the game thread:

1. `LoadLibraryExW(entry, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)`: the plugin's own dependencies come from its folder or System32, never the game folder or `PATH`.
2. Resolves `sco_plugin_query`, `sco_plugin_load` and `sco_plugin_unload`; any missing refuses the plugin.
3. `sco_plugin_query()` must return info whose `size` covers `author`, with `api_major` equal to the host's, `api_minor` not newer, and `name` equal to the id.
4. `sco_plugin_load(api, self)`; anything but `SCO_OK` refuses the plugin and releases what it registered.

`self` is the host's owner handle for the plugin; the host never reuses one. On refusal the DLL is closed.

### Crash containment

Every call into plugin code goes through `Guarded()`: on Windows a `__try/__except` that catches any SEH exception. Host trampolines for event callbacks, commands and tasks call `CallPlugin(plugin, where, thunk, ctx)`. A fault:

- `sco::Release(self)`: its subscriptions, commands, queued tasks and queued invokes are gone;
- the plugin is `crashed` and never called again;
- one `[plugin] hello crashed in tick (0xC0000005) and was disabled` line in `mod.log` and a status message;
- the DLL stays mapped, because its code may still be on a stack.

`DllMain` is not guarded: it runs under the OS loader lock, and unwinding out of it would leave the lock held. Keep `DllMain` empty and do the work in `sco_plugin_load`.

This limits damage. It is not a sandbox: stack corruption, `__fastfail` and `/GS` failures end the process, and a native plugin runs with the game's full rights.

### Unloading

`UnloadAll(list)` at `game.exit`: last loaded first, `sco_plugin_unload()` (guarded; a fault marks the plugin crashed and keeps the DLL), `Release(self)`, `FreeLibrary`.

## Data packs

`kind = data` packs carry content and never run code. `ContentIndex::Build(list)` indexes every ready pack and marks it `loaded`:

| Kind | Files |
|---|---|
| `mission` | `missions/*.cwmission` |
| `rules` | `rules/*.rules` |
| `script` | `scripts/**.xml` (any depth) |
| `list` | `lists/*.txt` |

Extensions match in any case. Anything else in the folder is ignored, and symlinks are skipped so a pack can't reach outside itself. A pack with more than 4096 matching files is refused (`too many files`). Features query the index with `Items(kind)`, `Find(kind, "missions/a.cwmission")` (one entry per pack that ships that name, in plugin order) or `FromPlugin(id)`, and read the files themselves.

## Status

`Describe(plugin)` gives one line, `hello 1.0.0 native loaded` or `pack 1.0.0 data refused: built for api 2.0`; `LogReport(list, enabled)` writes `[plugin] N found, L loaded (plugins = on)` and one line per plugin.

## Tests

- `tools/test.sh` runs `tests/test_plugins.cpp` on the host under ASan+UBSan: manifest rules, discovery over `tests/fixtures/plugins/tree/`, the content index, and the loader against real shared libraries built from `tests/fixtures/plugins/native/fake_plugin.c`, one per behavior. Off Windows the crash guard is a signal handler the test installs.
- `tools/test-win.sh` builds the same tests and plugins for Windows with mingw (the guard with clang, since GCC has no `__try`) and runs them under Wine: the real `LoadLibraryExW` and SEH path.
