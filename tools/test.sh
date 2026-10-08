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

FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -I "$ROOT/include")
RUNTIME=("$ROOT/src/api/sco_tasks.cpp" "$ROOT/src/api/sco_events.cpp" "$ROOT/src/api/sco_commands.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined \
  "$ROOT/tests/test_core.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_signatures.cpp" \
  "$ROOT/src/sco_log_status.cpp" "$ROOT/src/sco_pe_file.cpp" -o "$OUT/test_core"
"$OUT/test_core"
# The runtime is cross-thread: run its tests under ASan+UBSan and again under ThreadSanitizer.
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "$ROOT/tests/test_runtime.cpp" "${RUNTIME[@]}" -o "$OUT/test_runtime"
"$OUT/test_runtime"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "$ROOT/tests/test_runtime.cpp" "${RUNTIME[@]}" -o "$OUT/test_runtime_tsan"
"$OUT/test_runtime_tsan"

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
