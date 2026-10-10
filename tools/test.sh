#!/usr/bin/env bash
# Host unit tests for sco-core (macOS / Linux, no game, no Windows).
#   tools/test.sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/tests/out
mkdir -p "$OUT"
CXX=${CXX:-$(command -v clang++ || command -v g++)}
CC=${CC:-$(command -v clang || command -v gcc)}

# Plugin ABI pin (tests/abi_v1.c): compile-only. As C and C++ for this host, for x64 Windows
# (the real target; -ffreestanding so no Windows SDK is needed), and with -fshort-enums to prove
# the enums keep 4 bytes whatever the compiler's enum setting.
ABI=(-Wall -Wextra -Wpedantic -Werror -I "$ROOT/include" -fsyntax-only)
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_v1.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_v1.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_v1.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_v1.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_v1.c"
echo "abi_v1: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The sco.storage table (tests/abi_storage.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_storage.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_storage.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_storage.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_storage.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_storage.c"
echo "abi_storage: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The sco.ui table (tests/abi_ui.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_ui.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_ui.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_ui.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_ui.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_ui.c"
echo "abi_ui: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The sco.settings table (tests/abi_settings.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_settings.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_settings.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_settings.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_settings.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_settings.c"
echo "abi_settings: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The sco.datacore table (tests/abi_datacore.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_datacore.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_datacore.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_datacore.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_datacore.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_datacore.c"
echo "abi_datacore: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The sco.ipc table (tests/abi_ipc.c), the same five ways; and the MIT wire include/sc_ipc.h on its
# own, as the other side of a bridge includes it (no sco-core header), as C11 and C++20.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_ipc.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_ipc.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_ipc.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_ipc.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_ipc.c"
printf '#include "sc_ipc.h"\n' | "$CC"  -std=c11   -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I "$ROOT/include" -x c   -
printf '#include "sc_ipc.h"\n' | "$CXX" -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I "$ROOT/include" -x c++ -
echo "abi_ipc: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc); sc_ipc.h stands alone"
# The MIT wire of sco.net packets (tests/abi_sc_net.c), the same five ways; and sc_net.h on its own, as a
# program outside sco-core includes it, as C11 and C++20.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_sc_net.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_sc_net.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_sc_net.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_sc_net.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_sc_net.c"
printf '#include "sc_net.h"\n' | "$CC"  -std=c11   -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I "$ROOT/include" -x c   -
printf '#include "sc_net.h"\n' | "$CXX" -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I "$ROOT/include" -x c++ -
echo "abi_sc_net: wire pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc); sc_net.h stands alone"
# The sco.net table (tests/abi_net.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_net.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_net.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_net.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_net.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_net.c"
echo "abi_net: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The MIT bridge layouts (tests/abi_titanlink.c, tests/abi_voxel_bridge.c), the same five ways; and each
# header on its own, as the other side of a bridge includes it from the SDK, as C11 and C++20.
for b in titanlink voxel_bridge; do
  "$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_$b.c"
  "$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_$b.c"
  "$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_$b.c"
  clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_$b.c"
  clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_$b.c"
  printf '#include "sc_%s.h"\n' "$b" | "$CC"  -std=c11   -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I "$ROOT/include" -x c   -
  printf '#include "sc_%s.h"\n' "$b" | "$CXX" -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I "$ROOT/include" -x c++ -
  echo "abi_$b: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc); sc_$b.h stands alone"
done
# The teleport.spatial table sc-offline provides (tests/abi_spatial.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_spatial.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_spatial.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_spatial.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_spatial.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_spatial.c"
echo "abi_spatial: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The spawn.entities table sc-offline provides (tests/abi_spawn.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_spawn.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_spawn.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_spawn.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_spawn.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_spawn.c"
echo "abi_spawn: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The game.actors table the game pack provides (tests/abi_game_actors.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_actors.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_actors.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_game_actors.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_actors.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_actors.c"
echo "abi_game_actors: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The game pack's game.vehicles table (tests/abi_game_vehicles.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_vehicles.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_vehicles.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_game_vehicles.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_vehicles.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_vehicles.c"
echo "abi_game_vehicles: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"

# SDK sources (sdk/): the template and native example compile against sco_api.h alone, so a
# header change that breaks them fails here. The full build from the packaged zip, with MSVC on
# Windows and the plugin checks, is sdk/test-zip.sh (CI: .github/workflows/sdk.yml).
for f in "$ROOT/sdk/template/plugin.c" "$ROOT/sdk/examples/hello/hello.c"; do
  "$CC" -std=c11 "${ABI[@]}" "$f"
done
"$CC" -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I "$ROOT/include" -fsyntax-only "$ROOT/sdk/tools/sco-plugin-check.c"
# The C++20 layer (include/scosdk/): the cpp_hello example and each header on its own.
for f in "$ROOT/sdk/examples/cpp_hello/cpp_hello.cpp" "$ROOT"/include/scosdk/*.hpp "$ROOT"/include/scosdk/game/*.hpp; do
  "$CXX" -std=c++20 "${ABI[@]}" -x c++ "$f"
done
echo "sdk: template, hello, cpp_hello, include/scosdk and sco-plugin-check compile against sco_api.h"

# Vendored toml++ (third_party/tomlplusplus/README.md) is used as released: check both files first.
( cd "$ROOT/third_party/tomlplusplus" && printf '%s\n' \
  "6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e  toml.hpp" \
  "529bc3900a9571e49db285b0df432397e70b881cc3bf48de6667ae74ff4b06d8  LICENSE" | \
  if command -v sha256sum >/dev/null; then sha256sum -c --quiet; else shasum -a 256 -c --quiet; fi )
echo "toml++: 3.4.0, sha256 of toml.hpp and LICENSE match third_party/tomlplusplus/README.md"

FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -I "$ROOT/include" -isystem "$ROOT/third_party/sqlite"
       -isystem "$ROOT/third_party/tomlplusplus")
# The game signature tables; the CryPak hooks (pak_hooks.cpp) are built only into test_pak.
GAME=()
for f in "$ROOT"/src/game/*.cpp; do [ "$(basename "$f")" = pak_hooks.cpp ] || GAME+=("$f"); done
RUNTIME=("$ROOT/src/api/sco_tasks.cpp" "$ROOT/src/api/sco_events.cpp" "$ROOT/src/api/sco_commands.cpp"
         "$ROOT/src/api/sco_services.cpp" "$ROOT/src/api/sco_raw.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined \
  "$ROOT/tests/test_core.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_signatures.cpp" \
  "$ROOT/src/sco_log_status.cpp" "$ROOT/src/sco_pe_file.cpp" "${GAME[@]}" -o "$OUT/test_core"
"$OUT/test_core"
# The runtime is cross-thread: run its tests under ASan+UBSan and again under ThreadSanitizer.
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_runtime.cpp" "${RUNTIME[@]}" -o "$OUT/test_runtime"
"$OUT/test_runtime"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_runtime.cpp" "${RUNTIME[@]}" -o "$OUT/test_runtime_tsan"
"$OUT/test_runtime_tsan"
# Capabilities and the host's sco_api table, over the same runtime, under both sanitizer sets.
HOST=("$ROOT/tests/test_host.cpp" "$ROOT/src/api/sco_caps.cpp" "$ROOT/src/host/sco_host.cpp" "${RUNTIME[@]}"
      "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_log_status.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${HOST[@]}" -o "$OUT/test_host"
"$OUT/test_host"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${HOST[@]}" -o "$OUT/test_host_tsan"
# Detours over small functions written into executable memory (x86-64 only; skipped elsewhere).
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_hook.cpp" "$ROOT/src/hook/sco_hook.cpp" -o "$OUT/test_hook"
"$OUT/test_hook"
"$OUT/test_host_tsan"
# Spatial math and the zone tree: pure math, but readers run beside a writer, so both sanitizer sets.
SPATIAL=("$ROOT/tests/test_spatial.cpp" "$ROOT/src/engine/zone.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${SPATIAL[@]}" -o "$OUT/test_spatial"
"$OUT/test_spatial"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${SPATIAL[@]}" -o "$OUT/test_spatial_tsan"
"$OUT/test_spatial_tsan"
# sco::vfs: the read/seek arithmetic and the mount table, with readers beside table swaps, so both sets.
VFS=("$ROOT/tests/test_vfs.cpp" "$ROOT/src/vfs/compose.cpp" "$ROOT/src/vfs/table.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${VFS[@]}" -o "$OUT/test_vfs"
"$OUT/test_vfs"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${VFS[@]}" -o "$OUT/test_vfs_tsan"
# sco::game::pak: the CryPak adapter over a fake ICryPak and a fake loader, with a second thread on
# the engine's functions during the load window, so both sanitizer sets.
PAK=("$ROOT/tests/test_pak.cpp" "$ROOT/src/game/pak_hooks.cpp" "${GAME[@]}" "$ROOT/src/hook/sco_hook.cpp" "${VFS[@]:1}"
     "$ROOT/src/api/sco_caps.cpp" "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_log_status.cpp" "${RUNTIME[@]}")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${PAK[@]}" -o "$OUT/test_pak"
"$OUT/test_pak"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${PAK[@]}" -o "$OUT/test_pak_tsan"
"$OUT/test_pak_tsan"
"$OUT/test_vfs_tsan"
# The C++20 SDK layer: two SDK plugins over the same host table, under both sanitizer sets.
SDKT=("$ROOT/tests/test_sdk.cpp" "${HOST[@]:1}")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${SDKT[@]}" -o "$OUT/test_sdk"
"$OUT/test_sdk"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${SDKT[@]}" -o "$OUT/test_sdk_tsan"
"$OUT/test_sdk_tsan"
# sco.storage (sco/storage.h) over vendored SQLite, built once per sanitizer set as C with its own
# warnings off and the options of CMakeLists.txt (keep in step). test_storage spawns itself to test
# crash safety and runs threads, so both sanitizer sets.
SQLITE_DEFS=(-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0
             -DSQLITE_TEMP_STORE=2 -DSQLITE_TRUSTED_SCHEMA=0 -DSQLITE_USE_URI=0 -DSQLITE_OMIT_SHARED_CACHE
             -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_ENABLE_API_ARMOR)
# SQLite calls through function pointers cast to a common type (its destructors); clang's
# -fsanitize=function would flag every one of them in its own code.
NOFN=(); "$CC" --version 2>/dev/null | grep -q clang && NOFN=(-fno-sanitize=function)
SQLITE_ASAN=$OUT/sqlite3_asan.o
SQLITE_TSAN=$OUT/sqlite3_tsan.o
"$CC" -std=gnu11 -O1 -g0 -w -fsanitize=address,undefined "${NOFN[@]}" "${SQLITE_DEFS[@]}" -c "$ROOT/third_party/sqlite/sqlite3.c" -o "$SQLITE_ASAN"
"$CC" -std=gnu11 -O1 -g0 -w -fsanitize=thread "${SQLITE_DEFS[@]}" -c "$ROOT/third_party/sqlite/sqlite3.c" -o "$SQLITE_TSAN"
STORAGE=("$ROOT/tests/test_storage.cpp" "$ROOT/src/storage/storage.cpp" "${HOST[@]:1}")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${STORAGE[@]}" "$SQLITE_ASAN" -lm -o "$OUT/test_storage"
"$OUT/test_storage" "$OUT"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${STORAGE[@]}" "$SQLITE_TSAN" -lm -o "$OUT/test_storage_tsan"
"$OUT/test_storage_tsan" "$OUT"
# sc_ipc.h and sco.ipc (sco/ipc.h): the wire over plain memory, the service over shm_open, a peer on
# threads and in a second process (test_ipc spawns itself), so both sanitizer sets.
IPC=("$ROOT/tests/test_ipc.cpp" "$ROOT/src/ipc/ipc.cpp" "$ROOT/src/ipc/shm_posix.cpp" "${HOST[@]:1}")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${IPC[@]}" -o "$OUT/test_ipc"
"$OUT/test_ipc"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${IPC[@]}" -o "$OUT/test_ipc_tsan"
"$OUT/test_ipc_tsan"
# sco.net's core (sco/net/*.h): sha2.c built as C once per sanitizer set with -Werror like the rest;
# sessions of several Cores over the seeded in-memory network, so both sanitizer sets.
NET_C=(-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -I "$ROOT/include")
"$CC" "${NET_C[@]}" -fsanitize=address,undefined -c "$ROOT/src/net/sha2.c" -o "$OUT/sha2_asan.o"
"$CC" "${NET_C[@]}" -fsanitize=thread -c "$ROOT/src/net/sha2.c" -o "$OUT/sha2_tsan.o"
NET=("$ROOT/tests/test_net.cpp" "$ROOT/src/net/wire.cpp" "$ROOT/src/net/reliable.cpp" "$ROOT/src/net/core.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${NET[@]}" "$OUT/sha2_asan.o" -o "$OUT/test_net"
"$OUT/test_net"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${NET[@]}" "$OUT/sha2_tsan.o" -o "$OUT/test_net_tsan"
"$OUT/test_net_tsan"
# The sco.net service (sco/net/session.h) over the host table: the in-memory network with no thread,
# two Cores over UDP loopback, and the service's network thread over UDP loopback beside table calls
# from 8 threads, so both sanitizer sets.
NETSVC_SRC=("$ROOT/src/net/service.cpp" "$ROOT/src/net/scope.cpp" "$ROOT/src/net/udp_posix.cpp" "${NET[@]:1}")
NETSVC=("$ROOT/tests/test_net_service.cpp" "${NETSVC_SRC[@]}" "${HOST[@]:1}")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${NETSVC[@]}" "$OUT/sha2_asan.o" -o "$OUT/test_net_service"
"$OUT/test_net_service"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${NETSVC[@]}" "$OUT/sha2_tsan.o" -o "$OUT/test_net_service_tsan"
"$OUT/test_net_service_tsan"

# The DataCore parser (sco/datacore.h) over tests/dcb_builder.h fixtures, including truncated and
# corrupted files. A pure function over bytes with no shared state, so ASan+UBSan only. Then sco-dcb
# over the fixtures test_datacore writes, info, records and patch (CMake: CTest dcb_tool).
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_datacore.cpp" "$ROOT/src/datacore/datacore.cpp" \
  -o "$OUT/test_datacore"
"$OUT/test_datacore" "$OUT"
# The patcher over the same fixtures, its splices applied through sco::vfs (ASan+UBSan).
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_datacore_patch.cpp" "$ROOT/src/datacore/datacore.cpp" \
  "$ROOT/src/datacore/patch.cpp" "${VFS[@]:1}" -o "$OUT/test_datacore_patch"
"$OUT/test_datacore_patch"
# Data packs (sco/datacore_pack.h): golden parses, ordering, conflicts, atomicity, every refusal; it
# writes the fixture the sample pack is checked against (ASan+UBSan).
DATACORE=("$ROOT/src/datacore/datacore.cpp" "$ROOT/src/datacore/patch.cpp" "$ROOT/src/datacore/pack.cpp" "${VFS[@]:1}")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_datacore_pack.cpp" "${DATACORE[@]}" -o "$OUT/test_datacore_pack"
"$OUT/test_datacore_pack" "$ROOT" "$OUT"
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tools/sco-dcb.cpp" "${DATACORE[@]}" -o "$OUT/sco-dcb"
# The sco.datacore service over the real host table: a simulated launch sequence (load, a commit after
# it saved to data/datacore/pending/, the next launch applying it), release, atomicity, reports,
# datacore.applied and scosdk/datacore.hpp (ASan+UBSan).
SERVICE=("$ROOT/src/datacore/service.cpp" "${DATACORE[@]}" "$ROOT/src/api/sco_caps.cpp" "$ROOT/src/host/sco_host.cpp" "${RUNTIME[@]}"
         "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_log_status.cpp"
         "$ROOT/src/plugins/manifest.cpp" "$ROOT/src/plugins/settings_ini.cpp" "$ROOT/src/plugins/discover.cpp" "$ROOT/src/plugins/loader.cpp" "$ROOT/src/plugins/content.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_datacore_service.cpp" "${SERVICE[@]}" -ldl -o "$OUT/test_datacore_service"
"$OUT/test_datacore_service" "$OUT"
"$OUT/sco-dcb" info "$OUT/datacore_36.dcb" | grep -q '^layout: OK$' || { echo "sco-dcb: valid fixture not OK"; exit 1; }
"$OUT/sco-dcb" info "$OUT/datacore_32.dcb" | grep -q '^record size: 32 bytes' || { echo "sco-dcb: 32-byte records not derived"; exit 1; }
set +e
"$OUT/sco-dcb" info "$OUT/datacore_bad.dcb" > /dev/null; bad=$?
"$OUT/sco-dcb" info "$OUT/datacore_missing.dcb" 2> /dev/null; missing=$?
set -e
[ $bad -eq 1 ] && [ $missing -eq 2 ] || { echo "sco-dcb: exit codes $bad/$missing, expected 1/2"; exit 1; }
"$OUT/sco-dcb" records "$OUT/datacore_36.dcb" | grep -q "$(printf '\tShipA\tShip\t0x[0-9a-f]*\t0\t139\tlibs/foundry/records/test/ships.xml\tShips$')" || { echo "sco-dcb: records"; exit 1; }
# patch: a float override, AddRecord and a reference to the new record; the output re-parses and lists it.
"$OUT/sco-dcb" patch "$OUT/datacore_36.dcb" "$OUT/datacore_36_patched.dcb" --seed 1 set ShipA speed 2.5 \
  add-record Ship ShipC ShipA - set ShipB maker record:ShipC | grep -q '^Emit: OK' || { echo "sco-dcb: patch"; exit 1; }
"$OUT/sco-dcb" info "$OUT/datacore_36_patched.dcb" | grep -q '^layout: OK$' || { echo "sco-dcb: patched file not OK"; exit 1; }
"$OUT/sco-dcb" records "$OUT/datacore_36_patched.dcb" | grep -q "$(printf '^5\t.*\tShipC\tShip\t0x[0-9a-f]*\t2\t139\tlibs/foundry/records/sco/sco-dcb/ShipC.xml\tShips$')" \
  || { echo "sco-dcb: added record not listed"; exit 1; }
set +e
"$OUT/sco-dcb" patch "$OUT/datacore_36.dcb" "$OUT/datacore_36_refused.dcb" set ShipA speedX 1 | grep -q 'REFUSED (field not found)'; refused=$?
"$OUT/sco-dcb" patch "$OUT/datacore_36.dcb" "$OUT/datacore_36_refused.dcb" set ShipA speedX 1 > /dev/null; refusedRc=$?
set -e
[ $refused -eq 0 ] && [ $refusedRc -eq 1 ] && [ ! -e "$OUT/datacore_36_refused.dcb" ] || { echo "sco-dcb: patch refusal"; exit 1; }
echo "sco-dcb: info, records and patch over the fixtures, exit codes 0/1/2"
# lint, check, show and diff over the golden files and the sample pack (CMake: CTest dcb_pack).
cmake -DDCB="$OUT/sco-dcb" -DROOT="$ROOT" -DDIR="$OUT" -P "$ROOT/tests/dcb_pack.cmake" > "$OUT/dcb_pack.log" 2>&1 \
  || { cat "$OUT/dcb_pack.log"; echo "sco-dcb: pack commands failed"; exit 1; }
grep -F 'dcb_pack: OK' "$OUT/dcb_pack.log"

# Plugins: discovery, plugin.ini, the native loader and the content index. The native tests load
# real shared libraries built from tests/fixtures/plugins/native/fake_plugin.c, one per behavior
# (FAKE_MODE), into tests/out/plugins/m<mode>/ with a generated plugin.ini; m11 is a second clean
# plugin, svc_provider/svc_prov_b/c/d provide a service (mode 11), svc_caller (mode 12) has commands
# that call svc_provider's, and text/ is an entry that is not a library.
# Keep in step with CMakeLists.txt and tools/test-win.sh.
PLUG=$OUT/plugins
rm -rf "$PLUG"
SHARED=(-shared -fPIC); [ "$(uname)" = Darwin ] && SHARED=(-dynamiclib)
plugin() {   # plugin <id> <mode>
  mkdir -p "$PLUG/$1"
  printf 'id = %s\nname = Fake %s\nversion = 1.0.0\napi = 1.0\nkind = native\nentry = %s.so\n' "$1" "$1" "$1" > "$PLUG/$1/plugin.ini"
  "$CC" -std=c11 -O1 -Wall -Wextra -Werror -I "$ROOT/include" "${SHARED[@]}" -DFAKE_MODE="$2" -DFAKE_ID="\"$1\"" \
    "$ROOT/tests/fixtures/plugins/native/fake_plugin.c" -o "$PLUG/$1/$1.so"
}
for m in 0 1 2 3 4 5 6 7 8 9 10; do plugin "m$m" "$m"; done
plugin m11 0
for p in svc_provider svc_prov_b svc_prov_c svc_prov_d; do plugin "$p" 11; done
plugin svc_caller 12
mkdir -p "$PLUG/text"
printf 'id = text\nname = Not a library\nversion = 1\napi = 1.0\nkind = native\nentry = text.so\n' > "$PLUG/text/plugin.ini"
echo "not a shared library" > "$PLUG/text/text.so"
PLUGINS=("$ROOT/src/plugins/manifest.cpp" "$ROOT/src/plugins/settings_ini.cpp" "$ROOT/src/plugins/discover.cpp" "$ROOT/src/plugins/loader.cpp" \
         "$ROOT/src/plugins/content.cpp" "$ROOT/src/sco_log_status.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_plugins.cpp" "${PLUGINS[@]}" "${RUNTIME[@]}" \
  -ldl -o "$OUT/test_plugins"
"$OUT/test_plugins" "$ROOT/tests/fixtures/plugins" "$OUT"

# sco.ui (sco/ui.h, sco_ui.h, scosdk/ui.hpp): built-in plugins under the loader's crash guard (a
# signal-based stand-in here), registration from threads beside a frame, so both sanitizer sets.
UI=("$ROOT/tests/test_ui.cpp" "$ROOT/src/ui/ui.cpp" "${PLUGINS[@]}" "${RUNTIME[@]}" "$ROOT/src/api/sco_caps.cpp"
    "$ROOT/src/host/sco_host.cpp" "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${UI[@]}" -ldl -o "$OUT/test_ui"
"$OUT/test_ui"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${UI[@]}" -ldl -o "$OUT/test_ui_tsan"
"$OUT/test_ui_tsan"

# sco.settings (sco/settings.h, sco_settings.h, scosdk/settings.hpp): the declarations' values, the service
# over the real host table and storage, persistence across a restart, settings.changed; readers on
# threads beside the game thread's sets, so both sanitizer sets.
SETTINGS=("$ROOT/tests/test_settings.cpp" "$ROOT/src/settings/settings.cpp" "$ROOT/src/ui/ui.cpp" "$ROOT/src/storage/storage.cpp"
          "${PLUGINS[@]}" "${RUNTIME[@]}" "$ROOT/src/api/sco_caps.cpp" "$ROOT/src/host/sco_host.cpp"
          "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${SETTINGS[@]}" "$SQLITE_ASAN" -ldl -lm -o "$OUT/test_settings"
"$OUT/test_settings" "$OUT"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${SETTINGS[@]}" "$SQLITE_TSAN" -ldl -lm -o "$OUT/test_settings_tsan"
"$OUT/test_settings_tsan" "$OUT"

# sco-lua (plugins/lua): the sandboxed Lua runtime, loaded through the real loader and host table.
# Vendored Lua is built as C with its own warnings off; sco_lua.c with -Werror like the rest.
LUA_SRC=$ROOT/plugins/lua/third_party/lua/src
mkdir -p "$OUT/lua"
LUA_OBJS=()
for f in "$LUA_SRC"/*.c "$ROOT/plugins/lua/sco_lua.c"; do
  o="$OUT/lua/$(basename "$f" .c).o"
  if [ "$f" = "$ROOT/plugins/lua/sco_lua.c" ]; then W=(-Wall -Wextra -Wpedantic -Werror); else W=(-w); fi
  "$CC" -std=c11 -O1 -g "${W[@]}" -fsanitize=address,undefined -I "$ROOT/include" -I "$LUA_SRC" -c "$f" -o "$o"
  LUA_OBJS+=("$o")
done
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_lua.cpp" "${PLUGINS[@]}" "${RUNTIME[@]}" \
  "$ROOT/src/api/sco_caps.cpp" "$ROOT/src/host/sco_host.cpp" "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" \
  "$ROOT/src/ui/ui.cpp" "$ROOT/src/settings/settings.cpp" "$ROOT/src/datacore/service.cpp" "${DATACORE[@]}" "$ROOT/src/storage/storage.cpp" "$SQLITE_ASAN" \
  "${LUA_OBJS[@]}" -ldl -lm -o "$OUT/test_lua"
"$OUT/test_lua" "$ROOT/sdk" "$OUT"

# The host kit (sco/app.h): built-ins, the fake plugins m0 and m11, greeter and travel_pack through
# sco::app (ASan+UBSan). Then sco-host-sim over the SDK examples, laid out like data/plugins with
# hello built here as a shared library (CMake: CTest host_sim_examples).
APP=("$ROOT/src/app/sco_app.cpp" "${PLUGINS[@]}" "${RUNTIME[@]}" "$ROOT/src/api/sco_caps.cpp" "$ROOT/src/host/sco_host.cpp"
     "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "${GAME[@]}" "$ROOT/src/storage/storage.cpp" "$ROOT/src/ui/ui.cpp" "$ROOT/src/settings/settings.cpp" "$SQLITE_ASAN"
     "$ROOT/src/ipc/ipc.cpp" "$ROOT/src/ipc/shm_posix.cpp" "${NETSVC_SRC[@]}" "$OUT/sha2_asan.o"
     "$ROOT/src/datacore/service.cpp" "${DATACORE[@]}")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined -I "$ROOT/plugins/lua" "$ROOT/tests/test_app.cpp" "${APP[@]}" \
  "${LUA_OBJS[@]}" -ldl -o "$OUT/test_app"
"$OUT/test_app" "$ROOT/sdk" "$OUT"
SIM=$OUT/sim/plugins
rm -rf "$OUT/sim"
mkdir -p "$SIM/hello"
"$CC" -std=c11 -O1 -Wall -Wextra -Werror -I "$ROOT/include" "${SHARED[@]}" "$ROOT/sdk/examples/hello/hello.c" -o "$SIM/hello/hello.so"
sed 's/^entry = hello\.dll/entry = hello.so/' "$ROOT/sdk/examples/hello/plugin.ini" > "$SIM/hello/plugin.ini"
cp -R "$ROOT/sdk/examples/greeter" "$ROOT/sdk/examples/travel_pack" "$ROOT/sdk/examples/quantum_pack" "$SIM/"
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined -I "$ROOT/plugins/lua" "$ROOT/tools/sco-host-sim.cpp" "${APP[@]}" \
  "$ROOT/src/sco_pe_file.cpp" "${LUA_OBJS[@]}" -ldl -o "$OUT/sco-host-sim"
if ! "$OUT/sco-host-sim" "$SIM" --ticks 3 --invoke hello.wave "Pilot One" > "$OUT/sim.log"; then
  cat "$OUT/sim.log"; echo "sco-host-sim: failed"; exit 1
fi
cat "$OUT/sim.log"
grep -qF '[sim] invoke hello.wave -> OK "Hello, Pilot One"' "$OUT/sim.log" || { echo "sco-host-sim: no reply from hello.wave"; exit 1; }
grep -qF '[sim] datacore quantum_pack datacore/eos.toml: OK' "$OUT/sim.log" || { echo "sco-host-sim: the sample datacore pack wasn't linted"; exit 1; }
echo "sco-host-sim: SDK examples load, tick, answer and unload"
