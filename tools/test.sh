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
# Capabilities and the host's sco_api table, over the same runtime, under both sanitizer sets.
HOST=("$ROOT/tests/test_host.cpp" "$ROOT/src/api/sco_caps.cpp" "$ROOT/src/host/sco_host.cpp" "${RUNTIME[@]}"
      "$ROOT/src/sco_signatures.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_log_status.cpp")
"$CXX" "${FLAGS[@]}" -fsanitize=address,undefined "${HOST[@]}" -o "$OUT/test_host"
"$OUT/test_host"
"$CXX" "${FLAGS[@]}" -fsanitize=thread "${HOST[@]}" -o "$OUT/test_host_tsan"
"$OUT/test_host_tsan"
