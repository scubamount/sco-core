#!/usr/bin/env bash
# Build tools/sco-sigcheck and run it against a StarCitizen.exe on disk.
#   tools/sigcheck.sh <StarCitizen.exe> [-v] [--catalog <runtime_catalog.inc>]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/tests/out
mkdir -p "$OUT"
CXX=${CXX:-$(command -v clang++ || command -v g++)}
GAME=()   # the game signature tables, without the CryPak hooks (sco_pak)
for f in "$ROOT"/src/game/*.cpp; do [ "$(basename "$f")" = pak_hooks.cpp ] || GAME+=("$f"); done
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -I "$ROOT/include" "$ROOT/tools/sco-sigcheck.cpp" \
  "$ROOT"/src/sco_scan.cpp "$ROOT"/src/sco_signatures.cpp "$ROOT"/src/sco_log_status.cpp \
  "$ROOT"/src/sco_pe_file.cpp "${GAME[@]}" -o "$OUT/sco-sigcheck"
exec "$OUT/sco-sigcheck" "$@"
