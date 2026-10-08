#!/usr/bin/env bash
# Host unit tests for sco-core (macOS / Linux, no game, no Windows).
#   tools/test.sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/tests/out
mkdir -p "$OUT"
CXX=${CXX:-$(command -v clang++ || command -v g++)}
CC=${CC:-$(command -v clang || command -v gcc)}

# Plugin ABI pin (tests/abi_v1.c): compile-only, as C and as C++.
"$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -I "$ROOT/include" -c "$ROOT/tests/abi_v1.c" -o "$OUT/abi_v1_c.o"
"$CXX" -std=c++20 -Wall -Wextra -Wpedantic -Werror -I "$ROOT/include" -x c++ -c "$ROOT/tests/abi_v1.c" -o "$OUT/abi_v1_cpp.o"
echo "abi_v1: layout pinned (C11, C++20)"

"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$ROOT/include" \
  "$ROOT/tests/test_core.cpp" "$ROOT/src/sco_scan.cpp" "$ROOT/src/sco_signatures.cpp" \
  "$ROOT/src/sco_log_status.cpp" "$ROOT/src/sco_pe_file.cpp" -o "$OUT/test_core"
"$OUT/test_core"
