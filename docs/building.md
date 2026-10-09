# Building and testing

sco-core has two equivalent builds: the shell scripts in `tools/` (the reference, with every sanitizer run) and a root `CMakeLists.txt` (any CMake toolchain, MSVC included, and the way to consume sco-core from another CMake project). CI runs both.

## Requirements

| For | You need |
|---|---|
| `tools/test.sh` | bash, clang or gcc with C11 and C++20, and the sanitizer runtimes (ASan, UBSan, TSan). `clang` must be on `PATH` for the `x86_64-pc-windows-msvc` ABI pin |
| CMake | CMake 3.20 or newer and a 64-bit C/C++20 toolchain: clang, gcc, or Visual Studio 2019 or newer (MSVC x64) |
| `tools/test-win.sh` | `x86_64-w64-mingw32-g++`, clang with the `x86_64-w64-mingw32` target, and Wine |
| The SDK zip | Python 3, CMake, a C and C++20 compiler; Lua 5.4 for the Lua example check |

Nothing needs the game, and only the Windows build needs Windows.

## `tools/test.sh`

```sh
tools/test.sh                          # picks clang/clang++, else gcc/g++
CC=clang CXX=clang++ tools/test.sh     # what CI runs
```

In order, it:

1. compiles the ABI pins `tests/abi_v1.c` and `tests/abi_storage.c` with `-Werror` as C11 and C++20, again with `-fshort-enums`, and for `x86_64-pc-windows-msvc` (compile-only),
2. compiles the SDK template, the `hello` and `cpp_hello` examples, each `include/scosdk/` header and `sco-plugin-check` against `sco_api.h`,
3. builds and runs `test_core` and `test_hook` (ASan+UBSan), `test_runtime`, `test_host`, `test_spatial` and `test_sdk` (each under ASan+UBSan and again under ThreadSanitizer),
   then builds vendored SQLite once per sanitizer set and runs `test_storage` under both (it also spawns and kills copies of itself for the crash tests),
4. builds the fake plugins from `tests/fixtures/plugins/native/fake_plugin.c` into `tests/out/plugins/` and runs `test_plugins` (ASan+UBSan),
5. builds Lua and sco-lua and runs `test_lua` (ASan+UBSan), which also loads `sdk/examples/greeter` through the real loader,
6. runs `test_app` (ASan+UBSan): the host kit with built-in plugins, two fake plugins, `greeter` and `travel_pack`,
7. builds `hello` as a shared library into `tests/out/sim/plugins/` next to copies of `greeter` and `travel_pack`, builds `sco-host-sim` (ASan+UBSan) and runs `sco-host-sim tests/out/sim/plugins --ticks 3 --invoke hello.wave "Pilot One"`, which must exit 0 and answer `Hello, Pilot One`.

Each test binary ends with `N passed, M failed` and exits non-zero on any failure; the script stops at the first failure. Output goes to `tests/out/` (ignored by git).

## CMake

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows, from a Developer PowerShell for VS (the Visual Studio generator is multi-config, so name the config):

```powershell
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

### Options

| Option | Default | Does |
|---|---|---|
| `SCO_BUILD_TESTS` | `ON` when sco-core is the top-level project, else `OFF` | Builds the tests and the fake plugins and registers them with CTest |
| `SCO_WERROR` | `ON` | Warnings in sco-core's own code are errors (`-Wall -Wextra -Werror`; MSVC `/W4 /WX /utf-8`). Vendored Lua and SQLite are always built with warnings off |
| `SCO_SANITIZE` | empty | Sanitizers for sco-core's code and tests, for example `address,undefined` (not MSVC) |

The build is 64-bit only; configuring for 32 bits stops with an error.

### Targets

| Target | Sources | Links |
|---|---|---|
| `sco_core` | Scanners, signatures, log/status, PE file loader, `src/game/*.cpp` except `pak_hooks.cpp`; on Windows also `sco_image_win.cpp` | |
| `sco_pak` | `src/game/pak_hooks.cpp`: the CryPak adapter, `sco/game/pak.h` (its rows are in `sco_core`) | `sco_core`, `sco_hook`, `sco_vfs`, `sco_host` |
| `sco_runtime` | `src/api/sco_tasks.cpp`, `sco_events.cpp`, `sco_commands.cpp`, `sco_services.cpp` | |
| `sco_hook` | `src/hook/sco_hook.cpp`: detours and near-code memory, `sco/hook.h` (x86-64) | |
| `sco_engine` | `src/engine/zone.cpp`: the zone tree, `sco/engine/zone.h` (spatial math `sco/engine/types.h` is header-only) | |
| `sco_host` | `src/api/sco_caps.cpp`, `src/host/sco_host.cpp` | `sco_runtime`, `sco_core` |
| `sco_plugins` | `src/plugins/*.cpp` (`guard_win.cpp` on Windows only) | `sco_runtime`, `sco_core`, `dl` |
| `sco_lua_vendor` | `plugins/lua/third_party/lua/src/*.c` without `lua.c`/`luac.c` | `m` on Unix |
| `sco_lua` | `plugins/lua/sco_lua.c` | `sco_lua_vendor` |
| `sco_sqlite` | `third_party/sqlite/sqlite3.c` ([options](../third_party/sqlite/README.md)), `-std=gnu11`, warnings off | `Threads`, `m` on Unix |
| `sco_storage` | `src/storage/storage.cpp`: the `sco.storage` host service, `sco/storage.h` ([docs](storage.md)) | `sco_host`, `sco_sqlite` |
| `sco_app` | `src/app/sco_app.cpp`: the host kit, `sco/app.h` | `sco_host`, `sco_plugins`, `sco_core`, `sco_storage` |
| `sco-sigcheck` | `tools/sco-sigcheck.cpp` | `sco_core` |
| `sco-host-sim` | `tools/sco-host-sim.cpp` | `sco_app`, `sco_lua` |
| `sco_datacore` | `src/datacore/datacore.cpp`: the DataCore parser, `sco/datacore.h` | |
| `sco-dcb` | `tools/sco-dcb.cpp` ([docs](datacore.md)) | `sco_datacore` |

Every library exposes `include/` as a public include directory.

### Using sco-core from another CMake project

```cmake
add_subdirectory(external/sco-core)            # tests stay off when not top level
target_link_libraries(my_host PRIVATE sco_app sco_lua)   # sco_app brings sco_host, sco_plugins, sco_core
```

What sc-offline does, and why (from its `CMakeLists.txt`):

```cmake
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # before add_subdirectory
set(SCO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SCO_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory(external/sco-core EXCLUDE_FROM_ALL)
```

- **Set `CMAKE_MSVC_RUNTIME_LIBRARY` before `add_subdirectory`.** sco-core's targets take the value in effect when they are created, so a static CRT set afterwards doesn't reach them, and a DLL that links them imports the dynamic CRT after all. sc-offline's CI checks that `dinput8.dll` imports no dynamic CRT.
- **`SCO_BUILD_TESTS OFF`.** It is already off when sco-core isn't the top-level project; forcing it keeps a cached `ON` from an earlier configure out. sco-core's own CI runs the tests.
- **`SCO_WERROR OFF`.** sco-core's CI builds with `/W4 /WX`; in a product build, a warning that only a newer toolset emits must not break the build. Warnings still show.
- **`EXCLUDE_FROM_ALL`** builds only the libraries the product links, not `sco-sigcheck`, `sco-host-sim` or `sco-dcb`.
- The source tree must be checked out: with a submodule, clone with `--recurse-submodules` (CI: `submodules: true`). sc-offline stops at configure with a message when `external/sco-core` is empty.

### Tests

| CTest name | What |
|---|---|
| `abi_v1` | Rebuilds the ABI pin objects (C11, C++20 and, off MSVC, `-fshort-enums`); fails if any static assert breaks |
| `abi_storage` | The same for the `sco.storage` table pin, `tests/abi_storage.c` |
| `test_storage` | Host-owned services and `sco.storage`: key-value and the size handshake, transactions, SQL, isolation (no `ATTACH`, `PRAGMA` or `VACUUM INTO` escape), the quota, unload, threads, and a child process killed mid-transaction |
| `test_core` | Scanners and the signature registry against a synthetic image |
| `test_runtime` | Task queue, event bus, command registry, `Release` |
| `test_host` | Capabilities and the `sco_api` table |
| `test_hook` | Detours over small functions written into executable memory; vtable slot swaps |
| `test_pak` | `sco::game::pak` over a fake `ICryPak` (a real vtable) and a detoured fake loader: the load window, a served mount, passthrough of other files, a second `.dcb` and a second thread, an inert mount read from 0, offsets past 2 GiB, `Disable` |
| `test_spatial` | Vector, quaternion and transform math; the zone tree (chains, round trips at 1e11 m, failures, readers beside a writer) |
| `test_sdk` | The C++20 SDK layer (`include/scosdk/`): two SDK plugins over the real host table |
| `test_plugins` | `plugin.ini`, discovery, the content index and the native loader against the fake plugins in `<build>/tests/out/plugins/` |
| `test_lua` | sco-lua through the real loader and host table, including `sdk/examples/greeter` |
| `test_app` | The host kit (`sco::app`) and built-in plugins: load order, `game.ready`, tick, a faulting built-in contained, `game.exit` before unload, unload order, restart |
| `host_sim_examples` | `sco-host-sim <build>/tests/out/sim/plugins --ticks 3 --invoke hello.wave "Pilot One"` over `hello` (built by CMake), `greeter` and `travel_pack`: exit 0 and the reply (`tests/host_sim.cmake`) |

On Windows `test_plugins` loads real DLLs with `LoadLibraryExW` and runs the real `__try/__except` crash guard. The TSan runs and the `x86_64-pc-windows-msvc` cross-compile of the ABI pin stay in `tools/test.sh`.

## `sco-host-sim`

The real host kit, runtime, loader and sco-lua outside the game, for CI and for trying plugins:

```sh
sco-host-sim <plugin root> [--exe StarCitizen.exe] [--cap NAME]... [--ticks N] [--no-lua] [--invoke NAME [ARG]...]
```

It starts `sco::app` with plugins on and a built-in demo plugin `sim` (command `sim.ping`), runs N ticks (default 1), invokes one command with its arguments parsed against the command's arg defs (int, float, string, bool as `1`/`0`/`true`/`false`), then stops. `--exe` resolves the signature tables against a game executable on disk; `--cap` sets a capability ready; `--no-lua` leaves Lua plugins without a runtime (refused). Log lines go to stdout, as in `mod.log`. Exit 0 when every plugin ended loaded, off or disabled and the invoke answered OK; 1 otherwise; 2 for bad arguments.

## `tools/test-win.sh`

Builds `test_plugins` and the fake plugins as a Windows program with mingw (the crash guard with clang, since GCC has no `__try`) and runs it under Wine. Useful on macOS or Linux when you change the loader; on Windows, the CMake build runs the same test natively.

## The SDK

`sdk/` builds on its own (see the [SDK README](../sdk/README.md)). To build the zip modders download and check it the way CI does:

```sh
python3 sdk/package.py --out dist        # dist/sco-sdk-<version>.zip, reproducible, with SHA256SUMS
sdk/test-zip.sh dist/sco-sdk-*.zip       # builds and checks every example from the unpacked zip alone
```

## CI

| Workflow | Runs on | Does |
|---|---|---|
| [`test`](../.github/workflows/test.yml) | ubuntu-24.04 | `CC=clang CXX=clang++ tools/test.sh`, and checks that `sco-sigcheck` builds |
| [`cmake`](../.github/workflows/cmake.yml) | ubuntu-24.04 (clang, ASan+UBSan), windows-2025 (MSVC x64) | Configure, build and `ctest` |
| [`sdk`](../.github/workflows/sdk.yml) | ubuntu-24.04, then windows-2025 (MSVC x64) | Packages the SDK zip, builds and checks every example from it, keeps the zip as an artifact |
| [`discord`](../.github/workflows/discord.yml) | ubuntu | Posts repository activity to the sc-offline Discord channel |

Every workflow has `contents: read` permissions and pins its actions to commit SHAs. A pull request needs `test`, `cmake` and `sdk` green.

## Adding a source file

The two builds list sources separately. A new `.cpp` under `src/game/` is picked up by both builds' globs (`pak_hooks.cpp` is the one exclusion, for `sco_pak` and `test_pak`). Any other new `.cpp` goes into both `tools/test.sh` (`APP` for anything `sco_app` links) (and `tools/test-win.sh` if `test_plugins` links it) and the matching library in `CMakeLists.txt`; a new fake-plugin mode goes into the `for m in ...` loops of both scripts and `SCO_FAKE_PLUGIN_MODES`.
