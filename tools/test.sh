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
# The ABI freeze (tools/abi_snapshot.py): compiles a probe against include/sco_api.h and compares
# its sizeof / _Alignof / offsetof / enum / macro output with tests/abi/sco_api-v1.1.golden, then
# checks that include/sco_api.h as released at tag sdk-v1.1.0 hashes to the sha256 the golden
# records (comment-only changes since then are expected, so the pin is the semantic surface, not
# the bytes). There is no auto-update: a difference prints the diff and fails; a deliberate 1.x
# addition edits the golden by hand in the same PR.
PYTHON=${PYTHON:-python3}
"$PYTHON" "$ROOT/tools/abi_snapshot.py" check --cc "$CC"
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
# The renderer-neutral widget table and payloads, all five ABI modes.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_widgets.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_widgets.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_widgets.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_widgets.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_widgets.c"
echo "abi_widgets: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The sco.ui.surfaces table and payloads (tests/abi_ui_surfaces.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_ui_surfaces.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_ui_surfaces.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_ui_surfaces.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_ui_surfaces.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_ui_surfaces.c"
echo "abi_ui_surfaces: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
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
# The game pack's game.world table (tests/abi_game_world.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_world.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_world.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_game_world.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_world.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_world.c"
echo "abi_game_world: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The game pack's game.* event payloads (tests/abi_game_events.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_events.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_events.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_game_events.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_events.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_events.c"
echo "abi_game_events: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The game pack's game.entities table (tests/abi_game_entities.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_entities.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_entities.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_game_entities.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_entities.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_entities.c"
echo "abi_game_entities: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"
# The game pack's game.creative table (tests/abi_game_creative.c), the same five ways.
"$CC"  -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_creative.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_creative.c"
"$CC"  -std=c11   "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_game_creative.c"
clang   --target=x86_64-pc-windows-msvc -ffreestanding -std=c11   "${ABI[@]}" "$ROOT/tests/abi_game_creative.c"
clang++ --target=x86_64-pc-windows-msvc -ffreestanding -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_creative.c"
echo "abi_game_creative: layout pinned (C11, C++20, -fshort-enums, x86_64-pc-windows-msvc)"

# SDK sources (sdk/): the template and native example compile against sco_api.h alone, so a
# header change that breaks them fails here. The full build from the packaged zip, with MSVC on
# Windows and the plugin checks, is sdk/test-zip.sh (CI: .github/workflows/sdk.yml).
for f in "$ROOT/sdk/template/plugin.c" "$ROOT/sdk/examples/hello/hello.c"; do
  "$CC" -std=c11 "${ABI[@]}" "$f"
done
"$CC" -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I "$ROOT/include" -fsyntax-only "$ROOT/sdk/tools/sco-plugin-check.c"
# The C++20 layer (include/scosdk/): the cpp_hello example and each header on its own.
"$CXX" -std=c++20 "${ABI[@]}" "$ROOT/sdk/examples/cpp_hello/cpp_hello.cpp"
for f in "$ROOT"/include/scosdk/*.hpp "$ROOT"/include/scosdk/game/*.hpp; do
  # A header's self-containment is checked as an include, not as the main source.
  printf '#include "%s"\n' "${f#"$ROOT/include/"}" | "$CXX" -std=c++20 "${ABI[@]}" -x c++ -
done
echo "sdk: template, hello, cpp_hello, include/scosdk and sco-plugin-check compile against sco_api.h"

# Vendored toml++ (third_party/tomlplusplus/README.md) is used as released: check both files first.
( cd "$ROOT/third_party/tomlplusplus" && printf '%s\n' \
  "6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e  toml.hpp" \
  "529bc3900a9571e49db285b0df432397e70b881cc3bf48de6667ae74ff4b06d8  LICENSE" | \
  if sha256sum --version 2>/dev/null | grep -q GNU; then sha256sum -c --quiet; else shasum -a 256 -c --quiet; fi )
echo "toml++: 3.4.0, sha256 of toml.hpp and LICENSE match third_party/tomlplusplus/README.md"

FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -I "$ROOT/include" -isystem "$ROOT/third_party/sqlite"
       -isystem "$ROOT/third_party/tomlplusplus")

# Shared object cache for ASan and TSan compilations to eliminate repeated translation-unit compiles.
OBJ_ASAN=$OUT/obj_asan
OBJ_TSAN=$OUT/obj_tsan
# Objects are shared only within one invocation of this script: start empty so an edited source or
# header can never reuse an object built by an earlier run (no stale-cache reuse).
rm -rf "$OBJ_ASAN" "$OBJ_TSAN"
mkdir -p "$OBJ_ASAN" "$OBJ_TSAN"

obj() {
  local mode="$1"; shift
  local out=()
  for src in "$@"; do
    case "$src" in
      *.o) out+=("$src") ;;
      *.cpp|*.c)
        local base="$(echo "${src#"$ROOT/"}" | tr '/.' '__')"
        if [ "$mode" = "asan" ]; then
          out+=("$OBJ_ASAN/${base}.o")
        else
          out+=("$OBJ_TSAN/${base}.o")
        fi
        ;;
      *) out+=("$src") ;;
    esac
  done
  echo "${out[@]}"
}

compile_obj() {
  local mode="$1"; shift
  local src="$1"; shift
  # Any extra argument (macro variant) gets its own object name, so variants never share an object.
  local suffix=""
  if [ "$#" -gt 0 ]; then suffix="_$(printf '%s' "$*" | tr -c 'A-Za-z0-9\n' '_')"; fi
  local base="$(echo "${src#"$ROOT/"}" | tr '/.' '__')${suffix}"
  local out
  local san
  if [ "$mode" = "asan" ]; then
    out="$OBJ_ASAN/${base}.o"
    san=(-fsanitize=address,undefined)
  else
    out="$OBJ_TSAN/${base}.o"
    san=(-fsanitize=thread)
  fi
  if [ ! -f "$out" ]; then
    # Compile to a temporary name and rename only on success: a failed compile leaves no object.
    local tmp
    tmp="$(mktemp "$out.XXXXXX")" || return 1
    if "$CXX" "${FLAGS[@]}" "${san[@]}" "$@" -c "$src" -o "$tmp"; then
      mv -f "$tmp" "$out"
    else
      rm -f "$tmp"
      return 1
    fi
  fi
  echo "$out"
}

NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

parallel_compile() {
  local mode="$1"; shift
  local pids=() fail=0 p
  for f in "$@"; do
    compile_obj "$mode" "$f" >/dev/null &
    pids+=("$!")
    if [ "${#pids[@]}" -ge "$NPROC" ]; then
      wait "${pids[0]}" || fail=1
      pids=(${pids[@]+"${pids[@]:1}"})
    fi
  done
  for p in ${pids[@]+"${pids[@]}"}; do wait "$p" || fail=1; done
  if [ "$fail" != 0 ]; then echo "parallel_compile: at least one compile failed ($mode)" >&2; exit 1; fi
}

# Signature registration and tables only (the same closure as CMake's sco_game_sc).
# The CryPak adapter, game-host composition and game diagnostics have separate dependencies;
# do not pull them into scanner/signature tests merely because they live in src/game/.
GAME=("$ROOT/src/game/signatures.cpp" "$ROOT"/src/game/*_sigs.cpp)
RUNTIME=("$ROOT/src/api/sco_tasks.cpp" "$ROOT/src/api/sco_events.cpp" "$ROOT/src/api/sco_commands.cpp"
         "$ROOT/src/api/sco_services.cpp" "$ROOT/src/api/sco_raw.cpp")
HOST_IMPL=("$ROOT/src/api/sco_caps.cpp" "$ROOT/src/host/sco_host.cpp"
           "$ROOT/src/host/sco_diagnostics.cpp" "$ROOT/src/host/sco_diagnostics_publisher.cpp")
DATACORE=("$ROOT/src/datacore/datacore.cpp" "$ROOT/src/datacore/patch.cpp" "$ROOT/src/datacore/pack.cpp"
          "$ROOT/src/vfs/compose.cpp" "$ROOT/src/vfs/table.cpp")
PLUGINS=("$ROOT/src/plugins/manifest.cpp" "$ROOT/src/plugins/settings_ini.cpp" "$ROOT/src/plugins/discover.cpp"
         "$ROOT/src/plugins/loader.cpp" "$ROOT/src/plugins/content.cpp" "$ROOT/src/sco_log_status.cpp")
DIAGNOSTICS_IPC=("$ROOT/src/ipc/diagnostics_ipc.cpp" "$ROOT/src/ipc/diagnostics_json.cpp"
                 "$ROOT/src/ipc/diagnostics_audit.cpp" "$ROOT/src/ipc/diagnostics_platform_posix.cpp")
NETSVC_SRC=("$ROOT/src/net/service.cpp" "$ROOT/src/net/scope.cpp" "$ROOT/src/net/control.cpp"
            "$ROOT/src/net/udp_posix.cpp" "$ROOT/src/net/wire.cpp" "$ROOT/src/net/reliable.cpp" "$ROOT/src/net/core.cpp")

SHARED_ASAN_SOURCES=(
  "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_log_status.cpp" "$ROOT/src/sco_pe_file.cpp"
  "${GAME[@]}" "${RUNTIME[@]}" "${HOST_IMPL[@]}"
  "$ROOT/src/game/services/input.cpp" "$ROOT/src/game/services/gamerules.cpp"
  "$ROOT/src/hook/sco_hook.cpp" "$ROOT/src/engine/zone.cpp" "$ROOT/src/vfs/compose.cpp" "$ROOT/src/vfs/table.cpp"
  "$ROOT/src/game/pak_hooks.cpp" "${DATACORE[@]}" "${PLUGINS[@]}"
  "$ROOT/src/ui/ui.cpp" "$ROOT/src/ui/surfaces.cpp" "$ROOT/src/widgets/widgets.cpp"
  "$ROOT/src/settings/settings.cpp" "$ROOT/src/storage/storage.cpp"
  "$ROOT/src/ipc/ipc.cpp" "$ROOT/src/ipc/shm_posix.cpp"
  "$ROOT/src/net/wire.cpp" "$ROOT/src/net/reliable.cpp" "$ROOT/src/net/core.cpp"
  "$ROOT/src/net/service.cpp" "$ROOT/src/net/scope.cpp" "$ROOT/src/net/control.cpp" "$ROOT/src/net/udp_posix.cpp"
  "$ROOT/src/datacore/service.cpp" "$ROOT/src/content/content.cpp" "$ROOT/src/app/sco_app.cpp"
  "$ROOT/src/game/diagnostics.cpp" "$ROOT/src/game/private_session.cpp"
  "${DIAGNOSTICS_IPC[@]}"
)

SHARED_TSAN_SOURCES=(
  "${RUNTIME[@]}" "${HOST_IMPL[@]}"
  "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_log_status.cpp"
  "$ROOT/src/engine/zone.cpp" "$ROOT/src/vfs/compose.cpp" "$ROOT/src/vfs/table.cpp"
  "$ROOT/src/game/pak_hooks.cpp" "$ROOT/src/hook/sco_hook.cpp" "${GAME[@]}"
  "$ROOT/src/storage/storage.cpp" "$ROOT/src/ipc/ipc.cpp" "$ROOT/src/ipc/shm_posix.cpp"
  "$ROOT/src/net/wire.cpp" "$ROOT/src/net/reliable.cpp" "$ROOT/src/net/core.cpp"
  "$ROOT/src/net/service.cpp" "$ROOT/src/net/scope.cpp" "$ROOT/src/net/control.cpp" "$ROOT/src/net/udp_posix.cpp"
  "${PLUGINS[@]}"
  "$ROOT/src/ui/ui.cpp" "$ROOT/src/ui/surfaces.cpp" "$ROOT/src/widgets/widgets.cpp" "$ROOT/src/settings/settings.cpp"
)

parallel_compile asan "${SHARED_ASAN_SOURCES[@]}"
parallel_compile tsan "${SHARED_TSAN_SOURCES[@]}"

"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined \
  "$ROOT/tests/test_core.cpp" $(obj asan "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_signatures.cpp" \
  "$ROOT/src/sco_log_status.cpp" "$ROOT/src/sco_pe_file.cpp" "${GAME[@]}") -o "$OUT/test_core"
"$OUT/test_core"

# The runtime is cross-thread: run its tests under ASan+UBSan and again under ThreadSanitizer.
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_runtime.cpp" $(obj asan "${RUNTIME[@]}") -o "$OUT/test_runtime"
"$OUT/test_runtime"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_runtime.cpp" $(obj tsan "${RUNTIME[@]}") -o "$OUT/test_runtime_tsan"
"$OUT/test_runtime_tsan"

# Capabilities and the host's sco_api table, over the same runtime, under both sanitizer sets.
HOST_ASAN=($(obj asan "${HOST_IMPL[@]}" "${RUNTIME[@]}" "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_log_status.cpp"))
HOST_TSAN=($(obj tsan "${HOST_IMPL[@]}" "${RUNTIME[@]}" "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_log_status.cpp"))

"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_input.cpp" \
  $(obj asan "$ROOT/src/game/services/input.cpp") "${HOST_ASAN[@]}" -o "$OUT/test_input"
"$OUT/test_input"
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_gamerules_params.cpp" \
  $(obj asan "$ROOT/src/game/services/gamerules.cpp") "${HOST_ASAN[@]}" -o "$OUT/test_gamerules_params"
"$OUT/test_gamerules_params"
"$CC" -std=c11 "${ABI[@]}" "$ROOT/tests/abi_game_input.c"
"$CXX" -std=c++20 "${ABI[@]}" -x c++ "$ROOT/tests/abi_game_input.c"
"$CC" -std=c11 "${ABI[@]}" -fshort-enums "$ROOT/tests/abi_game_input.c"

"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_host.cpp" "${HOST_ASAN[@]}" -o "$OUT/test_host"
"$OUT/test_host"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_host.cpp" "${HOST_TSAN[@]}" -o "$OUT/test_host_tsan"

# The death hook's one-owner rule (game.actors.died for every death, game.player.died only for your
# player's), over the real bus with real subscribers. The decision is src/game/services/death_events.h,
# which is portable on purpose: the hook itself is Windows-only (it reads game memory under SEH), so
# this is the seam that exercises the rule without a game.
# ASan+UBSan only, and deliberately single-threaded: the dispatch runs on one (game) thread, which is
# how the game calls it, so a thread sanitizer has nothing to find here that the bus's own
# cross-thread tests (test_runtime, test_host) don't already cover under both sets.
"$CXX" "${FLAGS[@]}" -I "$ROOT/src" -fsanitize=address,undefined "$ROOT/tests/test_death_events.cpp" "${HOST_ASAN[@]}" -o "$OUT/test_death_events"
"$OUT/test_death_events"

# Detours over small functions written into executable memory (x86-64 only; skipped elsewhere).
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_hook.cpp" $(obj asan "$ROOT/src/hook/sco_hook.cpp") -o "$OUT/test_hook"
"$OUT/test_hook"
"$OUT/test_host_tsan"

# Spatial math and the zone tree: pure math, but readers run beside a writer, so both sanitizer sets.
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_spatial.cpp" $(obj asan "$ROOT/src/engine/zone.cpp") -o "$OUT/test_spatial"
"$OUT/test_spatial"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_spatial.cpp" $(obj tsan "$ROOT/src/engine/zone.cpp") -o "$OUT/test_spatial_tsan"
"$OUT/test_spatial_tsan"

# sco::vfs: the read/seek arithmetic and the mount table, with readers beside table swaps, so both sets.
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_vfs.cpp" $(obj asan "$ROOT/src/vfs/compose.cpp" "$ROOT/src/vfs/table.cpp") -o "$OUT/test_vfs"
"$OUT/test_vfs"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_vfs.cpp" $(obj tsan "$ROOT/src/vfs/compose.cpp" "$ROOT/src/vfs/table.cpp") -o "$OUT/test_vfs_tsan"

# sco::game::pak: the CryPak adapter over a fake ICryPak and a fake loader, with a second thread on
# the engine's functions during the load window, so both sanitizer sets.
PAK_SRC=("$ROOT/src/game/pak_hooks.cpp" "${GAME[@]}" "$ROOT/src/hook/sco_hook.cpp" "$ROOT/src/vfs/compose.cpp" "$ROOT/src/vfs/table.cpp" \
         "$ROOT/src/api/sco_caps.cpp" "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_log_status.cpp" "${RUNTIME[@]}")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_pak.cpp" $(obj asan "${PAK_SRC[@]}") -o "$OUT/test_pak"
"$OUT/test_pak"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_pak.cpp" $(obj tsan "${PAK_SRC[@]}") -o "$OUT/test_pak_tsan"
"$OUT/test_pak_tsan"
"$OUT/test_vfs_tsan"

# The C++20 SDK layer: two SDK plugins over the same host table, under both sanitizer sets.
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_sdk.cpp" "${HOST_ASAN[@]}" -o "$OUT/test_sdk"
"$OUT/test_sdk"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_sdk.cpp" "${HOST_TSAN[@]}" -o "$OUT/test_sdk_tsan"
"$OUT/test_sdk_tsan"

# Direct C workflow recipes and C++ actor/entity complete-pointer and null-method guards.
"$CC" -std=c11 -O1 -g -Wall -Wextra -Werror -I "$ROOT/include" -fsanitize=address,undefined \
  "$ROOT/tests/test_language_routes.c" -o "$OUT/test_language_routes_c"
"$OUT/test_language_routes_c"
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_language_routes.cpp" -o "$OUT/test_language_routes"
"$OUT/test_language_routes"

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
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_storage.cpp" $(obj asan "$ROOT/src/storage/storage.cpp") "${HOST_ASAN[@]}" "$SQLITE_ASAN" -lm -o "$OUT/test_storage"
"$OUT/test_storage" "$OUT"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_storage.cpp" $(obj tsan "$ROOT/src/storage/storage.cpp") "${HOST_TSAN[@]}" "$SQLITE_TSAN" -lm -o "$OUT/test_storage_tsan"
"$OUT/test_storage_tsan" "$OUT"

# sc_ipc.h and sco.ipc (sco/ipc.h): the wire over plain memory, the service over shm_open, a peer on
# threads and in a second process (test_ipc spawns itself), so both sanitizer sets.
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_ipc.cpp" $(obj asan "$ROOT/src/ipc/ipc.cpp" "$ROOT/src/ipc/shm_posix.cpp") "${HOST_ASAN[@]}" -o "$OUT/test_ipc"
"$OUT/test_ipc"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_ipc.cpp" $(obj tsan "$ROOT/src/ipc/ipc.cpp" "$ROOT/src/ipc/shm_posix.cpp") "${HOST_TSAN[@]}" -o "$OUT/test_ipc_tsan"
"$OUT/test_ipc_tsan"

# sco.net's core (sco/net/*.h): sha2.c built as C once per sanitizer set with -Werror like the rest;
# sessions of several Cores over the seeded in-memory network, so both sanitizer sets.
NET_C=(-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -I "$ROOT/include")
"$CC" "${NET_C[@]}" -fsanitize=address,undefined -c "$ROOT/src/net/sha2.c" -o "$OUT/sha2_asan.o"
"$CC" "${NET_C[@]}" -fsanitize=thread -c "$ROOT/src/net/sha2.c" -o "$OUT/sha2_tsan.o"
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_net.cpp" $(obj asan "$ROOT/src/net/wire.cpp" "$ROOT/src/net/reliable.cpp" "$ROOT/src/net/core.cpp") "$OUT/sha2_asan.o" -o "$OUT/test_net"
"$OUT/test_net"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_net.cpp" $(obj tsan "$ROOT/src/net/wire.cpp" "$ROOT/src/net/reliable.cpp" "$ROOT/src/net/core.cpp") "$OUT/sha2_tsan.o" -o "$OUT/test_net_tsan"
"$OUT/test_net_tsan"

# The sco.net service (sco/net/session.h) over the host table: the in-memory network with no thread,
# two Cores over UDP loopback, and the service's network thread over UDP loopback beside table calls
# from 8 threads, so both sanitizer sets.
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_net_service.cpp" $(obj asan "${NETSVC_SRC[@]}") "${HOST_ASAN[@]}" "$OUT/sha2_asan.o" -o "$OUT/test_net_service"
"$OUT/test_net_service"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_net_service.cpp" $(obj tsan "${NETSVC_SRC[@]}") "${HOST_TSAN[@]}" "$OUT/sha2_tsan.o" -o "$OUT/test_net_service_tsan"
"$OUT/test_net_service_tsan"

# The DataCore parser (sco/datacore.h) over tests/dcb_builder.h fixtures, including truncated and
# corrupted files. A pure function over bytes with no shared state, so ASan+UBSan only. Then sco-dcb
# over the fixtures test_datacore writes, info, records and patch (CMake: CTest dcb_tool).
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_datacore.cpp" $(obj asan "$ROOT/src/datacore/datacore.cpp") -o "$OUT/test_datacore"
"$OUT/test_datacore" "$OUT"
# The patcher over the same fixtures, its splices applied through sco::vfs (ASan+UBSan).
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_datacore_patch.cpp" \
  $(obj asan "$ROOT/src/datacore/datacore.cpp" "$ROOT/src/datacore/patch.cpp" "$ROOT/src/vfs/compose.cpp" "$ROOT/src/vfs/table.cpp") \
  -o "$OUT/test_datacore_patch"
"$OUT/test_datacore_patch"
# Data packs (sco/datacore_pack.h): golden parses, ordering, conflicts, atomicity, every refusal; it
# writes the fixture the sample pack is checked against (ASan+UBSan).
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_datacore_pack.cpp" $(obj asan "${DATACORE[@]}") -o "$OUT/test_datacore_pack"
"$OUT/test_datacore_pack" "$ROOT" "$OUT"
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tools/sco-dcb.cpp" $(obj asan "${DATACORE[@]}") -o "$OUT/sco-dcb"
# The sco.datacore service over the real host table: a simulated launch sequence (load, a commit after
# it saved to data/datacore/pending/, the next launch applying it), release, atomicity, reports,
# datacore.applied and scosdk/datacore.hpp (ASan+UBSan).
SERVICE_ASAN=($(obj asan "$ROOT/src/datacore/service.cpp" "${DATACORE[@]}" "${HOST_IMPL[@]}" "${RUNTIME[@]}" \
  "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_log_status.cpp" \
  "$ROOT/src/plugins/manifest.cpp" "$ROOT/src/plugins/settings_ini.cpp" "$ROOT/src/plugins/discover.cpp" \
  "$ROOT/src/plugins/loader.cpp" "$ROOT/src/plugins/content.cpp"))
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_datacore_service.cpp" "${SERVICE_ASAN[@]}" -ldl -o "$OUT/test_datacore_service"
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
# The ABI freeze's old binary: tests/fixtures/plugins/native/frozen_v11_plugin.c compiled against
# include/sco_api.h exactly as released at sdk-v1.1.0 (the tag's header, extracted by
# tools/abi_snapshot.py; nothing binary is committed), loaded by test_plugins.cpp. Keep in step
# with CMakeLists.txt and tools/test-win.sh.
FROZEN=$OUT/frozen_v11/v11
mkdir -p "$FROZEN" "$OUT/frozen_v11_include"
"$PYTHON" "$ROOT/tools/abi_snapshot.py" released-header "$OUT/frozen_v11_include/sco_api.h"
printf 'id = v11\nname = Frozen 1.1\nversion = 1.1.0\napi = 1.1\nkind = native\nentry = v11.so\n' > "$FROZEN/plugin.ini"
"$CC" -std=c11 -O1 -Wall -Wextra -Werror -I "$OUT/frozen_v11_include" "${SHARED[@]}" \
  "$ROOT/tests/fixtures/plugins/native/frozen_v11_plugin.c" -o "$FROZEN/v11.so"
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_manifest.cpp" $(obj asan "${PLUGINS[@]}") \
  -ldl -o "$OUT/test_manifest"
"$OUT/test_manifest"

"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_plugins.cpp" $(obj asan "${PLUGINS[@]}" "${RUNTIME[@]}") \
  -ldl -o "$OUT/test_plugins"
"$OUT/test_plugins" "$ROOT/tests/fixtures/plugins" "$OUT"

# Independent C SDK workflow consumers on the neutral host: public game contract tables,
# real loader/runtime, synthetic engine responses. CMake: sdk_conformance.
CONF=$OUT/sdk_conformance/plugins
for id in conformance conformance_peer; do
  mkdir -p "$CONF/$id"
  printf 'id = %s\nname = SDK conformance\nversion = 1.0.0\napi = 1.1\nkind = native\nentry = %s.so\n' "$id" "$id" > "$CONF/$id/plugin.ini"
  "$CC" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$ROOT/include" \
    "${SHARED[@]}" -DCONSUMER_ID="\"$id\"" "$ROOT/tests/game_contracts/conformance/consumer.c" -o "$CONF/$id/$id.so"
done
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/game_contracts/conformance/host.cpp" \
  $(obj asan "${PLUGINS[@]:0:5}") "${HOST_ASAN[@]}" -ldl -o "$OUT/test_sdk_conformance"
"$OUT/test_sdk_conformance" "$CONF" "$PLUG"

# sco.ui (sco/ui.h, sco_ui.h, scosdk/ui.hpp): built-in plugins under the loader's crash guard (a
# signal-based stand-in here), registration from threads beside a frame, so both sanitizer sets.
UI_SRC=("$ROOT/src/ui/ui.cpp" "$ROOT/src/ui/surfaces.cpp" "${PLUGINS[@]}" "${RUNTIME[@]}" "${HOST_IMPL[@]}" \
        "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_ui.cpp" $(obj asan "${UI_SRC[@]}") -ldl -o "$OUT/test_ui"
"$OUT/test_ui"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_ui.cpp" $(obj tsan "${UI_SRC[@]}") -ldl -o "$OUT/test_ui_tsan"
"$OUT/test_ui_tsan"
# sco.ui.surfaces (sco/surfaces.h, sco_ui_surfaces.h, scosdk/ui_surfaces.hpp): registrations from
# threads beside a renderer's snapshots, so both sanitizer sets.
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_surfaces.cpp" $(obj asan "${UI_SRC[@]}") -ldl -o "$OUT/test_surfaces"
"$OUT/test_surfaces"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_surfaces.cpp" $(obj tsan "${UI_SRC[@]}") -ldl -o "$OUT/test_surfaces_tsan"
"$OUT/test_surfaces_tsan"

# Retained widgets over the neutral real host, and the host-only ImGui API shim.
WIDGETS_SRC=("$ROOT/src/widgets/widgets.cpp" "$ROOT/src/sco_log_status.cpp" "${RUNTIME[@]}" "${HOST_IMPL[@]}" \
             "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_widgets.cpp" $(obj asan "${WIDGETS_SRC[@]}") -o "$OUT/test_widgets"
"$OUT/test_widgets"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_widgets.cpp" $(obj tsan "${WIDGETS_SRC[@]}") -o "$OUT/test_widgets_tsan"
"$OUT/test_widgets_tsan"
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined -DSCO_TEST_IMGUI_ADAPTER \
  -I "$ROOT/sdk/third_party/imgui" "$ROOT/tests/test_widgets_sdk.cpp" "$ROOT/src/widgets/imgui_adapter.cpp" -o "$OUT/test_widgets_sdk"
"$OUT/test_widgets_sdk"

# sco.settings (sco/settings.h, sco_settings.h, scosdk/settings.hpp): the declarations' values, the service
# over the real host table and storage, persistence across a restart, settings.changed; readers on
# threads beside the game thread's sets, so both sanitizer sets.
SETTINGS_SRC=("$ROOT/src/settings/settings.cpp" "$ROOT/src/ui/ui.cpp" "$ROOT/src/ui/surfaces.cpp" "$ROOT/src/storage/storage.cpp" \
              "${PLUGINS[@]}" "${RUNTIME[@]}" "${HOST_IMPL[@]}" "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_settings.cpp" $(obj asan "${SETTINGS_SRC[@]}") "$SQLITE_ASAN" -ldl -lm -o "$OUT/test_settings"
"$OUT/test_settings" "$OUT"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_settings.cpp" $(obj tsan "${SETTINGS_SRC[@]}") "$SQLITE_TSAN" -ldl -lm -o "$OUT/test_settings_tsan"
"$OUT/test_settings_tsan" "$OUT"

# sco-lua (plugins/lua): the sandboxed Lua runtime, loaded through the real loader and host table.
# Vendored Lua is built as C with its own warnings off; sco_lua.c with -Werror like the rest.
LUA_SRC=$ROOT/plugins/lua/third_party/lua/src
mkdir -p "$OUT/lua"
LUA_OBJS=()
for f in "$LUA_SRC"/*.c "$ROOT/plugins/lua/sco_lua.c" "$ROOT/plugins/lua/sco_lua_game.c"; do
  o="$OUT/lua/$(basename "$f" .c).o"
  case "$f" in "$ROOT"/plugins/lua/sco_lua*.c) W=(-Wall -Wextra -Wpedantic -Werror) ;; *) W=(-w) ;; esac
  "$CC" -std=c11 -O1 -g "${W[@]}" -fsanitize=address,undefined -I "$ROOT/include" -I "$LUA_SRC" -c "$f" -o "$o"
  LUA_OBJS+=("$o")
done
LUA_TEST_SRC=("${PLUGINS[@]}" "${RUNTIME[@]}" "${HOST_IMPL[@]}" "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" \
              "$ROOT/src/ui/ui.cpp" "$ROOT/src/ui/surfaces.cpp" "$ROOT/src/settings/settings.cpp" "$ROOT/src/datacore/service.cpp" \
              "${DATACORE[@]}" "$ROOT/src/storage/storage.cpp" "$ROOT/src/widgets/widgets.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined -isystem "$LUA_SRC" "$ROOT/tests/test_lua.cpp" $(obj asan "${LUA_TEST_SRC[@]}") \
  "$SQLITE_ASAN" "${LUA_OBJS[@]}" -ldl -lm -o "$OUT/test_lua"
"$OUT/test_lua" "$ROOT/sdk" "$OUT"
"$OUT/test_lua" "$ROOT/sdk" "$OUT/widgets" widgets

# The host kit (sco/app.h): built-ins, the fake plugins m0 and m11, greeter and travel_pack through
# sco::app (ASan+UBSan). Then sco-host-sim over the SDK examples, laid out like data/plugins with
# hello built here as a shared library (CMake: CTest host_sim_examples).
# The app owns diagnostic transport and advisory sources, unlike the signature-only tests.
APP_SRC=("$ROOT/src/content/content.cpp" "$ROOT/src/app/sco_app.cpp" "$ROOT/src/widgets/widgets.cpp" "${PLUGINS[@]}" "${RUNTIME[@]}" "${HOST_IMPL[@]}" \
         "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "${GAME[@]}" "$ROOT/src/game/diagnostics.cpp" "$ROOT/src/game/private_session.cpp" \
         "$ROOT/src/storage/storage.cpp" "$ROOT/src/ui/ui.cpp" "$ROOT/src/ui/surfaces.cpp" "$ROOT/src/settings/settings.cpp" \
         "$ROOT/src/ipc/ipc.cpp" "$ROOT/src/ipc/shm_posix.cpp" "${DIAGNOSTICS_IPC[@]}" "${NETSVC_SRC[@]}" \
         "$ROOT/src/datacore/service.cpp" "${DATACORE[@]}")

# Note: sco_services.cpp and diagnostics_ipc.cpp need -DSCO_RUNTIME_DIAGNOSTICS_FIXTURE=1 for the fixture test
OBJ_SERVICES_FIXTURE="$(compile_obj asan "$ROOT/src/api/sco_services.cpp" -DSCO_RUNTIME_DIAGNOSTICS_FIXTURE=1)"
OBJ_DIAG_IPC_FIXTURE="$(compile_obj asan "$ROOT/src/ipc/diagnostics_ipc.cpp" -DSCO_RUNTIME_DIAGNOSTICS_FIXTURE=1)"

APP_ASAN=($(obj asan "${APP_SRC[@]}"))
APP_FIXTURE_ASAN=()
for obj_path in "${APP_ASAN[@]}"; do
  case "$obj_path" in
    *sco_services_cpp.o) APP_FIXTURE_ASAN+=("$OBJ_SERVICES_FIXTURE") ;;
    *diagnostics_ipc_cpp.o) APP_FIXTURE_ASAN+=("$OBJ_DIAG_IPC_FIXTURE") ;;
    *) APP_FIXTURE_ASAN+=("$obj_path") ;;
  esac
done

"$CXX" "${FLAGS[@]}" -DSCO_RUNTIME_DIAGNOSTICS_FIXTURE=1 -fsanitize=address,undefined \
  "$ROOT/tests/test_runtime_diagnostics_snapshot.cpp" "$ROOT/tests/test_runtime_diagnostics_services.cpp" \
  "${APP_FIXTURE_ASAN[@]}" "$SQLITE_ASAN" "$OUT/sha2_asan.o" -ldl -lm -o "$OUT/test_runtime_diagnostics_snapshot"
"$OUT/test_runtime_diagnostics_snapshot"

"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined -I "$ROOT/plugins/lua" -isystem "$LUA_SRC" "$ROOT/tests/test_app.cpp" \
  "${APP_ASAN[@]}" "$SQLITE_ASAN" "$OUT/sha2_asan.o" "${LUA_OBJS[@]}" -ldl -o "$OUT/test_app"
"$OUT/test_app" "$ROOT/sdk" "$OUT"
"$OUT/test_app" "$ROOT/sdk" "$OUT" shutdown-only

"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_widgets_app.cpp" \
  "${APP_ASAN[@]}" "$SQLITE_ASAN" "$OUT/sha2_asan.o" -ldl -o "$OUT/test_widgets_app"
"$OUT/test_widgets_app"

SIM=$OUT/sim/plugins
rm -rf "$OUT/sim"
mkdir -p "$SIM/hello"
"$CC" -std=c11 -O1 -Wall -Wextra -Werror -I "$ROOT/include" "${SHARED[@]}" "$ROOT/sdk/examples/hello/hello.c" -o "$SIM/hello/hello.so"
sed 's/^entry = hello\.dll/entry = hello.so/' "$ROOT/sdk/examples/hello/plugin.ini" > "$SIM/hello/plugin.ini"
cp -R "$ROOT/sdk/examples/greeter" "$ROOT/sdk/examples/travel_pack" "$ROOT/sdk/examples/quantum_pack" "$SIM/"

"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined -I "$ROOT/plugins/lua" -isystem "$LUA_SRC" "$ROOT/tools/sco-host-sim.cpp" \
  "${APP_ASAN[@]}" $(obj asan "$ROOT/src/sco_pe_file.cpp") "$SQLITE_ASAN" "$OUT/sha2_asan.o" "${LUA_OBJS[@]}" -ldl -o "$OUT/sco-host-sim"
if ! "$OUT/sco-host-sim" "$SIM" --ticks 3 --invoke hello.wave "Pilot One" > "$OUT/sim.log"; then
  cat "$OUT/sim.log"; echo "sco-host-sim: failed"; exit 1
fi
cat "$OUT/sim.log"
grep -qF '[sim] invoke hello.wave -> OK "Hello, Pilot One"' "$OUT/sim.log" || { echo "sco-host-sim: no reply from hello.wave"; exit 1; }
grep -qF '[sim] datacore quantum_pack datacore/eos.toml: OK' "$OUT/sim.log" || { echo "sco-host-sim: the sample datacore pack wasn't linted"; exit 1; }
echo "sco-host-sim: SDK examples load, tick, answer and unload"
