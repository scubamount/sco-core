#!/usr/bin/env bash
# Plugin loader tests as a Windows build, run under Wine: the real LoadLibraryExW / GetProcAddress
# path and the real __try/__except crash guard (tools/test.sh covers the same tests on the host
# with a signal-based stand-in guard).
#   tools/test-win.sh                  needs x86_64-w64-mingw32-g++ and clang (any LLVM with the
#                                      x86_64-w64-mingw32 target); WINE (default: wine) runs it
# GCC has no __try, so src/plugins/guard_win.cpp is built with clang against the mingw headers;
# everything else with mingw g++.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/tests/out/win
GXX=${GXX:-x86_64-w64-mingw32-g++}
GCC=${GCC:-x86_64-w64-mingw32-gcc}
CLANG=${CLANG:-clang}
WINE=${WINE:-wine}
rm -rf "$OUT"
mkdir -p "$OUT"

# The mingw headers clang should use: the last include dir g++ searches (the mingw runtime's),
# not GCC's own intrinsics, which clang can't parse.
MINGW_INC=$(echo | "$GXX" -x c -E -v - 2>&1 | sed -n '/#include <...> search starts/,/End of search/p' | grep '^ ' | tail -1 | sed 's/^ //')
"$CLANG" --target=x86_64-w64-mingw32 -std=c++20 -fms-extensions -O1 -Wall -Wextra -Werror \
  -nostdlibinc -isystem "$MINGW_INC" -c "$ROOT/src/plugins/guard_win.cpp" -o "$OUT/guard_win.o"

PLUG=$OUT/plugins
plugin() {   # plugin <id> <mode>
  mkdir -p "$PLUG/$1"
  printf 'id = %s\nname = Fake %s\nversion = 1.0.0\napi = 1.0\nkind = native\nentry = %s.dll\n' "$1" "$1" "$1" > "$PLUG/$1/plugin.ini"
  "$GCC" -std=c11 -O1 -Wall -Wextra -Werror -I "$ROOT/include" -shared -DFAKE_MODE="$2" -DFAKE_ID="\"$1\"" \
    "$ROOT/tests/fixtures/plugins/native/fake_plugin.c" -o "$PLUG/$1/$1.dll"
}
for m in 0 1 2 3 4 5 6 7 8 9 10; do plugin "m$m" "$m"; done
plugin m11 0
mkdir -p "$PLUG/text"
printf 'id = text\nname = Not a library\nversion = 1\napi = 1.0\nkind = native\nentry = text.dll\n' > "$PLUG/text/plugin.ini"
echo "not a DLL" > "$PLUG/text/text.dll"

"$GXX" -std=c++20 -O1 -g -Wall -Wextra -Werror -I "$ROOT/include" -static \
  "$ROOT/tests/test_plugins.cpp" \
  "$ROOT/src/plugins/manifest.cpp" "$ROOT/src/plugins/discover.cpp" "$ROOT/src/plugins/loader.cpp" \
  "$ROOT/src/plugins/content.cpp" "$OUT/guard_win.o" "$ROOT/src/sco_log_status.cpp" \
  "$ROOT/src/api/sco_tasks.cpp" "$ROOT/src/api/sco_events.cpp" "$ROOT/src/api/sco_commands.cpp" \
  "$ROOT/src/api/sco_services.cpp" \
  -o "$OUT/test_plugins.exe"

# Wine wants Windows paths for arguments; it maps / to Z:.
winpath() { printf 'Z:%s' "$(echo "$1" | tr / '\\')"; }
# The tree test only reads; the limits tests write under <out>, so give it the win dir.
"$WINE" "$OUT/test_plugins.exe" "$(winpath "$ROOT/tests/fixtures/plugins")" "$(winpath "$OUT")"
