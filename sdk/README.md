# sco SDK

Build plugins for [sc-offline](https://github.com/scubamount/sc-offline), the Star Citizen offline mod. A plugin is one folder that sc-offline loads from `data\plugins\<id>\`. There are three kinds:

| Kind | What it is | Example |
|---|---|---|
| `native` | A 64-bit Windows DLL written in C against `sco_api.h`, in C++20 with the `include/scosdk/` headers, or in C# with `csharp/Sco.Sdk` (NativeAOT) | [`examples/hello`](examples/hello), [`examples/cpp_hello`](examples/cpp_hello), [`examples/cs_hello`](examples/cs_hello) |
| `data` | Files only: missions, rules, scripts, lists and game-data overrides (`datacore\*.toml`). Runs no code | [`examples/travel_pack`](examples/travel_pack), [`examples/quantum_pack`](examples/quantum_pack) |
| `lua` | A Lua 5.4 script, run in a sandbox by sc-offline's bundled Lua runtime | [`examples/greeter`](examples/greeter) |

> **Status: 1.0-pre.** The API can still change until the `sdk-v1.0.0` tag. Plugin loading is in sc-offline's `main` branch, switched off by default (`plugins = on` in `sc-offline.ini` turns it on), but not yet in a tagged sc-offline release.

**Native plugins run with the game's full rights.** Only install plugins you trust; sc-offline doesn't review them. Plugins must stay offline and single-player: see [Plugin rules](docs/plugin-rules.md).

## What's in the SDK

| Path | What |
|---|---|
| `include/sco_api.h` | The only header a native C plugin includes |
| `include/scosdk/` | The C++20 layer over `sco_api.h`, header-only: [C++ plugins](#c-plugins) |
| `csharp/` | The C# layer (`Sco.Sdk`, .NET 8, NativeAOT) and its layout test: [C# plugins](#c-plugins-1) |
| `cmake/sco-plugin.cmake` | `sco_add_plugin()` and `sco_add_pack()`: build a plugin and lay it out |
| `template/` | A native plugin to copy and rename |
| `examples/` | `hello` (native C), `cpp_hello` (native C++20), `cs_hello` (native C#, NativeAOT), `travel_pack` (data), `quantum_pack` (data: DataCore overrides), `greeter` (Lua) |
| `tools/sco-plugin-check.c` | Checks a built plugin folder on your machine, without the game |
| `tools/lua-check.lua` | Runs a Lua plugin against a stand-in `sco` table, without the game |
| `docs/` | [plugin.ini](docs/plugin-ini.md), [data packs](docs/data-packs.md), [Lua](docs/lua.md), [plugin rules](docs/plugin-rules.md), [API reference](../docs/api-v1.md), [C++ SDK](../docs/sdk-cpp.md), [C# SDK](../docs/sdk-csharp.md) |
| `SHA256SUMS` | The SHA-256 of every other file (in the zip only) |

## Build the examples

You need CMake 3.20 or newer (Visual Studio 2019 ships 3.20) and a C and C++20 compiler (Visual Studio 2019 16.11 or newer, clang 14, gcc 11). For plugins the game can load, that's Visual Studio 2019 or newer with the "Desktop development with C++" workload, on Windows. On macOS and Linux the same commands build plugins you can check with `sco-plugin-check`; the game needs the Windows DLL.

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

`out/data/plugins/` now holds `hello/`, `cpp_hello/`, `travel_pack/`, `quantum_pack/`, `greeter/` and `my_plugin/` (the template), laid out the way sc-offline reads them, and `out/bin/` holds `sco-plugin-check`.

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

## C++ plugins

`include/scosdk/` is a header-only C++20 layer over `sco_api.h`, with nothing beyond the standard library. Derive from `sco::sdk::Plugin`, override `OnLoad`, and `SCO_PLUGIN` writes the three exports:

```cpp
#include "scosdk/scosdk.hpp"

class MyPlugin : public sco::sdk::Plugin {
public:
    sco_result OnLoad() override {
        return sco::sdk::CommandBuilder(*this, "my_plugin.ping")
            .Title("Ping")
            .Handle([](const sco::sdk::Args&, sco::sdk::Reply& reply) { reply.Set("pong"); return SCO_OK; })
            .Register();
    }
};

SCO_PLUGIN(MyPlugin, "my_plugin", "1.0.0", "you");
```

- Commands with typed arguments (`CommandBuilder`, `Args`, `Reply`), event subscriptions as handles that unsubscribe themselves (`Subscribe`), tasks as `std::function` (`RunOnGameThread`), services (`Provide`, `ServiceRef<T>`) and raw handlers with the size handshake done for you (`RegisterRaw<In, Out>`, `InvokeRaw`).
- The SDK owns your handlers and strings for the plugin's life and releases them at unload; nothing it hands the host can be called after it was freed.
- No exception crosses into the host: one that escapes `OnLoad` or a handler is caught, logged, and answered with `SCO_FAILED`.

Build a C++ plugin with the same `sco_add_plugin`, in a project that enables `CXX`; [`examples/cpp_hello`](examples/cpp_hello/cpp_hello.cpp) shows the whole layer in one file. The [C++ SDK reference](../docs/sdk-cpp.md) covers lifetimes, the exception boundary and threads.

## C# plugins

`csharp/Sco.Sdk` is a .NET 8 library over `sco_api.h` and the host services, with no package references. A plugin derives from `Sco.Sdk.Plugin`, declares the three exports as `[UnmanagedCallersOnly(EntryPoint = "sco_plugin_query")]` (and `_load`, `_unload`) methods that forward to `PluginExports`, and is published with NativeAOT into one native DLL: no .NET runtime is needed next to the game.

```powershell
cd examples\cs_hello
dotnet publish -c Release -r win-x64 -o out\cs_hello
copy plugin.ini out\cs_hello\
..\..\out\bin\sco-plugin-check out\cs_hello --invoke cs_hello.wave "Pilot One"
```

- You need the .NET 8 SDK and, on Windows, Visual Studio 2019 or newer with "Desktop development with C++" (NativeAOT links with MSVC). CMake doesn't build C# plugins; `dotnet publish` does.
- Commands (`CommandBuilder`, `Args`, `Reply`), `Subscribe` handles, `RunOnGameThread`, `Invoke` / `InvokeAsync`, services (`Provide`, `Query` with `ServiceRef<T>.Covers`), raw handlers with the size handshake, and `Storage`, `DataCore` and `Ui` for the host services.
- Every callback's `ctx` is an id into a registry swept at unload, never a `GCHandle`; no exception crosses into the host (logged, and `SCO_FAILED` where there is a result).
- `dotnet run -c Release --project csharp/Sco.Sdk.Tests` checks every C# struct against the pinned C layout.

[`examples/cs_hello`](examples/cs_hello/CsHello.cs) shows the whole layer in one file. The [C# SDK reference](../docs/sdk-csharp.md) covers setup, lifetimes, threads, the exception boundary and the NativeAOT rules.

## License

GPL-3.0 ([LICENSE](../LICENSE)), like sco-core and sc-offline. Plugins built against `sco_api.h` are GPL-3.0 too; there is no linking exception.
