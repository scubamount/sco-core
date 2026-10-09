# sco SDK

Build plugins for [sc-offline](https://github.com/scubamount/sc-offline), the Star Citizen offline mod. A plugin is one folder that sc-offline loads from `data\plugins\<id>\`. There are three kinds:

| Kind | What it is | Example |
|---|---|---|
| `native` | A 64-bit Windows DLL written in C (or C++ with `extern "C"`) against `sco_api.h` | [`examples/hello`](examples/hello) |
| `data` | Files only: missions, rules, scripts and lists. Runs no code | [`examples/travel_pack`](examples/travel_pack) |
| `lua` | A Lua 5.4 script, run in a sandbox by sc-offline's bundled Lua runtime | [`examples/greeter`](examples/greeter) |

> **Status: 1.0-pre.** The API can still change until the `sdk-v1.0.0` tag. Plugin loading is in sc-offline's `main` branch, switched off by default (`plugins = on` in `sc-offline.ini` turns it on), but not yet in a tagged sc-offline release.

**Native plugins run with the game's full rights.** Only install plugins you trust; sc-offline doesn't review them. Plugins must stay offline and single-player: see [Plugin rules](docs/plugin-rules.md).

## What's in the SDK

| Path | What |
|---|---|
| `include/sco_api.h` | The only header a native plugin includes |
| `cmake/sco-plugin.cmake` | `sco_add_plugin()` and `sco_add_pack()`: build a plugin and lay it out |
| `template/` | A native plugin to copy and rename |
| `examples/` | `hello` (native), `travel_pack` (data), `greeter` (Lua) |
| `tools/sco-plugin-check.c` | Checks a built plugin folder on your machine, without the game |
| `tools/lua-check.lua` | Runs a Lua plugin against a stand-in `sco` table, without the game |
| `docs/` | [plugin.ini](docs/plugin-ini.md), [data packs](docs/data-packs.md), [Lua](docs/lua.md), [plugin rules](docs/plugin-rules.md), [API reference](../docs/api-v1.md) |
| `SHA256SUMS` | The SHA-256 of every other file (in the zip only) |

## Build the examples

You need CMake 3.20 or newer (Visual Studio 2019 ships 3.20) and a C compiler. For plugins the game can load, that's Visual Studio 2019 or newer with the "Desktop development with C++" workload, on Windows. On macOS and Linux the same commands build plugins you can check with `sco-plugin-check`; the game needs the Windows DLL.

Windows (Developer PowerShell for VS):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix out
```

macOS or Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix out
```

`out/data/plugins/` now holds `hello/`, `travel_pack/`, `greeter/` and `my_plugin/` (the template), laid out the way sc-offline reads them, and `out/bin/` holds `sco-plugin-check`.

## Check a plugin

```sh
out/bin/sco-plugin-check out/data/plugins/hello
out/bin/sco-plugin-check out/data/plugins/hello --cap teleport --invoke hello.wave "Pilot One"
lua5.4 tools/lua-check.lua examples/greeter --invoke greeter.greet "Pilot One" true
```

`sco-plugin-check` reads `plugin.ini` with the host's rules. For a native plugin it then loads the DLL, calls `sco_plugin_query` and `sco_plugin_load` against a stand-in host that enforces the documented rules, fires `game.ready` and a few `tick`s, runs every command that takes no arguments (and the one you name with `--invoke`), and unloads. For a data pack it lists the files the host would index. `--cap NAME` makes `has(NAME)` answer 1; by default every capability is missing, as on a game build the mod doesn't support yet. Exit code 0 means every check passed.

The stand-in host is not the game: it has no game thread, no crash guard and no game features. A plugin that passes still needs a run in the game.

## Install a plugin

1. Copy the plugin's folder, for example `out\data\plugins\hello`, into sc-offline's `data\plugins\` folder, so you have `data\plugins\hello\plugin.ini`.
2. In `sc-offline.ini` set `plugins = on`.
3. Play. `data\mod.log` lists every plugin with its state: `loaded`, `off`, `disabled`, `crashed` or `refused: <reason>`. `sc-offline.exe status` shows the same list.

To switch one plugin off, create an empty file named `disabled` in its folder.

## Start your own native plugin

1. Copy `template\` to a new folder outside the SDK.
2. Pick an id: 1-31 characters from `a-z`, `0-9` and `_`, not `sco`, `host`, `menu` or `game`. It is the folder name, the `id` in `plugin.ini`, the `name` in `sco_plugin_info` and the prefix of every command (`<id>.<action>`).
3. Replace every `my_plugin` in `plugin.c`, `plugin.ini` and `CMakeLists.txt` with it.
4. Build, pointing at the unpacked SDK:

   ```sh
   cmake -S . -B build -DSCO_SDK=/path/to/sco-sdk-1.0.0-pre
   ```

The template registers one command, `my_plugin.ping`. [`examples/hello/hello.c`](examples/hello/hello.c) adds `has()`, events and a command with an argument. The [API reference](../docs/api-v1.md) covers every function.

## License

GPL-3.0 ([LICENSE](../LICENSE)), like sco-core and sc-offline. Plugins built against `sco_api.h` are GPL-3.0 too; there is no linking exception.
