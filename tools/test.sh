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

# SDK sources (sdk/): the template and native example compile against sco_api.h alone, so a
# header change that breaks them fails here. The full build from the packaged zip, with MSVC on
# Windows and the plugin checks, is sdk/test-zip.sh (CI: .github/workflows/sdk.yml).
for f in "$ROOT/sdk/template/plugin.c" "$ROOT/sdk/examples/hello/hello.c"; do
  "$CC" -std=c11 "${ABI[@]}" "$f"
done
"$CC" -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I "$ROOT/include" -fsyntax-only "$ROOT/sdk/tools/sco-plugin-check.c"
# The C++20 layer (include/scosdk/): the cpp_hello example and each header on its own.
for f in "$ROOT/sdk/examples/cpp_hello/cpp_hello.cpp" "$ROOT"/include/scosdk/*.hpp; do
  "$CXX" -std=c++20 "${ABI[@]}" -x c++ "$f"
done
echo "sdk: template, hello, cpp_hello, include/scosdk and sco-plugin-check compile against sco_api.h"

FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -I "$ROOT/include")
RUNTIME=("$ROOT/src/api/sco_tasks.cpp" "$ROOT/src/api/sco_events.cpp" "$ROOT/src/api/sco_commands.cpp"
         "$ROOT/src/api/sco_services.cpp" "$ROOT/src/api/sco_raw.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined \
  "$ROOT/tests/test_core.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_signatures.cpp" \
  "$ROOT/src/sco_log_status.cpp" "$ROOT/src/sco_pe_file.cpp" "$ROOT/src/game/"*.cpp -o "$OUT/test_core"
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
"$OUT/test_vfs_tsan"
# The C++20 SDK layer: two SDK plugins over the same host table, under both sanitizer sets.
SDKT=("$ROOT/tests/test_sdk.cpp" "${HOST[@]:1}")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${SDKT[@]}" -o "$OUT/test_sdk"
"$OUT/test_sdk"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${SDKT[@]}" -o "$OUT/test_sdk_tsan"
"$OUT/test_sdk_tsan"

# Plugins: discovery, plugin.ini, the native loader and the content index. The native tests load
# real shared libraries built from tests/fixtures/plugins/native/fake_plugin.c, one per behavior
# (FAKE_MODE), into tests/out/plugins/m<mode>/ with a generated plugin.ini; m11 is a second clean
# plugin and text/ an entry that is not a library.
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
mkdir -p "$PLUG/text"
printf 'id = text\nname = Not a library\nversion = 1\napi = 1.0\nkind = native\nentry = text.so\n' > "$PLUG/text/plugin.ini"
echo "not a shared library" > "$PLUG/text/text.so"
PLUGINS=("$ROOT/src/plugins/manifest.cpp" "$ROOT/src/plugins/discover.cpp" "$ROOT/src/plugins/loader.cpp" \
         "$ROOT/src/plugins/content.cpp" "$ROOT/src/sco_log_status.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_plugins.cpp" "${PLUGINS[@]}" "${RUNTIME[@]}" \
  -ldl -o "$OUT/test_plugins"
"$OUT/test_plugins" "$ROOT/tests/fixtures/plugins" "$OUT"

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
  "${LUA_OBJS[@]}" -ldl -o "$OUT/test_lua"
"$OUT/test_lua" "$ROOT/sdk" "$OUT"

# The host kit (sco/app.h): built-ins, the fake plugins m0 and m11, greeter and travel_pack through
# sco::app (ASan+UBSan). Then sco-host-sim over the SDK examples, laid out like data/plugins with
# hello built here as a shared library (CMake: CTest host_sim_examples).
APP=("$ROOT/src/app/sco_app.cpp" "${PLUGINS[@]}" "${RUNTIME[@]}" "$ROOT/src/api/sco_caps.cpp" "$ROOT/src/host/sco_host.cpp"
     "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/game/"*.cpp)
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined -I "$ROOT/plugins/lua" "$ROOT/tests/test_app.cpp" "${APP[@]}" \
  "${LUA_OBJS[@]}" -ldl -o "$OUT/test_app"
"$OUT/test_app" "$ROOT/sdk" "$OUT"
SIM=$OUT/sim/plugins
rm -rf "$OUT/sim"
mkdir -p "$SIM/hello"
"$CC" -std=c11 -O1 -Wall -Wextra -Werror -I "$ROOT/include" "${SHARED[@]}" "$ROOT/sdk/examples/hello/hello.c" -o "$SIM/hello/hello.so"
sed 's/^entry = hello\.dll/entry = hello.so/' "$ROOT/sdk/examples/hello/plugin.ini" > "$SIM/hello/plugin.ini"
cp -R "$ROOT/sdk/examples/greeter" "$ROOT/sdk/examples/travel_pack" "$SIM/"
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined -I "$ROOT/plugins/lua" "$ROOT/tools/sco-host-sim.cpp" "${APP[@]}" \
  "$ROOT/src/sco_pe_file.cpp" "${LUA_OBJS[@]}" -ldl -o "$OUT/sco-host-sim"
if ! "$OUT/sco-host-sim" "$SIM" --ticks 3 --invoke hello.wave "Pilot One" > "$OUT/sim.log"; then
  cat "$OUT/sim.log"; echo "sco-host-sim: failed"; exit 1
fi
cat "$OUT/sim.log"
grep -qF '[sim] invoke hello.wave -> OK "Hello, Pilot One"' "$OUT/sim.log" || { echo "sco-host-sim: no reply from hello.wave"; exit 1; }
echo "sco-host-sim: SDK examples load, tick, answer and unload"
