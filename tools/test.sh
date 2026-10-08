#!/usr/bin/env bash
# Host unit tests for sco-core (macOS / Linux, no game, no Windows).
#   tools/test.sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/tests/out
mkdir -p "$OUT"
CXX=${CXX:-$(command -v clang++ || command -v g++)}
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
