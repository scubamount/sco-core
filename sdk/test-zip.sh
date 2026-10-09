#!/usr/bin/env bash
# Unpacks the SDK zip into an empty folder and builds and checks every example from it alone:
# no file from the repository is used after packaging. CI runs this (.github/workflows/sdk.yml)
# on Linux and Windows (Git Bash + MSVC); it also runs on macOS.
#
#   sdk/test-zip.sh                 # packages first (sdk/package.py), then tests that zip
#   sdk/test-zip.sh <sco-sdk.zip>   # tests a zip built elsewhere, e.g. CI's artifact
#
# Needs python3, cmake, a C and C++20 compiler and Lua 5.4 (lua5.4). SCO_SDK_NO_LUA=1 skips the Lua
# example's run (the Windows CI job, which has no Lua); the job that packages runs it.
#
# Exit 0 = the zip is complete and every example builds and passes its check.
set -euo pipefail
SDK=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# Git Bash on Windows: hand native tools (cmake, the checker) C:/ paths, not /tmp ones.
if command -v cygpath >/dev/null; then WORK=$(cygpath -m "$WORK"); fi

PY=${PYTHON:-$(command -v python3 || command -v python)}
if [ $# -ge 1 ]; then
  ZIP=$1
else
  "$PY" "$SDK/package.py" --out "$WORK/dist"
  ZIP=$(ls "$WORK"/dist/sco-sdk-*.zip)
fi
mkdir "$WORK/unpacked"
"$PY" -m zipfile -e "$ZIP" "$WORK/unpacked"
ROOT=$(ls -d "$WORK"/unpacked/sco-sdk-*)

# The zip's own manifest must match its files, and list every file.
( cd "$ROOT" && if command -v sha256sum >/dev/null; then sha256sum -c --quiet SHA256SUMS; else shasum -a 256 -c --quiet SHA256SUMS; fi )
listed=$(wc -l < "$ROOT/SHA256SUMS")
actual=$(cd "$ROOT" && find . -type f ! -name SHA256SUMS | wc -l)
[ "$listed" -eq "$actual" ] || { echo "SHA256SUMS lists $listed files, zip has $actual"; exit 1; }
echo "zip: $actual files, SHA256SUMS OK"

# Build everything from the unpacked SDK, as a modder would.
cmake -S "$ROOT" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release
# --config is for multi-config generators (Visual Studio); single-config ones ignore it.
cmake --build "$WORK/build" --config Release
cmake --install "$WORK/build" --config Release --prefix "$WORK/out"
CHECK="$WORK/out/bin/sco-plugin-check"
P="$WORK/out/data/plugins"

"$CHECK" "$P/hello"
"$CHECK" "$P/hello" --cap teleport --invoke hello.wave "Pilot One" | tee "$WORK/hello.txt"
grep -q 'invoke  hello.wave -> ok "Hello, Pilot One"' "$WORK/hello.txt"
# The C++20 example (include/scosdk/): a command with an argument, one without, and its service.
"$CHECK" "$P/cpp_hello" --invoke cpp_hello.wave "Pilot One" | tee "$WORK/cpp_hello.txt"
grep -q 'invoke  cpp_hello.wave -> ok "Hello, Pilot One"' "$WORK/cpp_hello.txt"
grep -q 'invoke  cpp_hello.ticks -> ok "3 ticks"' "$WORK/cpp_hello.txt"
grep -q 'service cpp_hello.greeter 1.0' "$WORK/cpp_hello.txt"
"$CHECK" "$P/my_plugin" | tee "$WORK/template.txt"
grep -q 'invoke  my_plugin.ping -> ok "pong"' "$WORK/template.txt"
"$CHECK" "$P/travel_pack"
"$CHECK" "$P/greeter"

# sco-plugin-check reads plugin.ini with the host's rules (src/plugins/manifest.cpp): it refuses
# what the game refuses and accepts what the game accepts.
CASES="$WORK/cases"
case_ini() {   # case_ini [key=value]...: a lua plugin.ini with these keys changed, in $CASES/<id>
  local id=bad name=Case version=1.0 author=Someone api=1.0 kind=lua entry=main.lua requires=teleport extra= kv
  for kv in "$@"; do local "$kv"; done
  mkdir -p "$CASES/$id"
  : > "$CASES/$id/main.lua"
  printf 'id = %s\nname = %s\nversion = %s\nauthor = %s\napi = %s\nkind = %s\nentry = %s\nrequires = %s\n%s\n' \
    "$id" "$name" "$version" "$author" "$api" "$kind" "$entry" "$requires" "$extra" > "$CASES/$id/plugin.ini"
  echo "$CASES/$id"
}
refused() {    # refused <what> [key=value]...: the checker must fail on that plugin.ini
  local what=$1 dir code
  shift
  dir=$(case_ini "$@")
  set +e; "$CHECK" "$dir" > "$WORK/case.txt"; code=$?; set -e
  { [ $code -eq 1 ] && grep -q '^FAIL plugin.ini' "$WORK/case.txt"; } || { cat "$WORK/case.txt"; echo "sco-plugin-check accepted $what"; exit 1; }
}
refused "requires = a..b" "requires=a..b"
refused "a capability listed twice" "requires=teleport, teleport"
refused "'*' in entry" "entry=main*.lua"
refused "a control character in entry" "entry=main$(printf '\001').lua"
refused "an empty author" "author="
refused "a control character in name" "name=Case$(printf '\177')"
refused "api = 1.0x" "api=1.0x"
refused "api = +1.0" "api=+1.0"
refused "api = -0.0" "api=-0.0"
refused "a plugin.ini over 16 KiB" "extra=;$(head -c 17000 /dev/zero | tr '\0' x)"
good=$(case_ini id=good "extra=homepage = a
homepage = b
notes = $(head -c 3000 /dev/zero | tr '\0' x)")
"$CHECK" "$good" > "$WORK/case.txt" || { cat "$WORK/case.txt"; echo "sco-plugin-check refused unknown keys or a long line"; exit 1; }
echo "sco-plugin-check: 10 bad plugin.ini refused, unknown keys and a 3000-byte line accepted"

if [ "${SCO_SDK_NO_LUA:-0}" = 1 ]; then
  echo "lua-check: skipped (SCO_SDK_NO_LUA=1)"
else
  LUA=$(command -v lua5.4 || command -v lua-5.4 || command -v lua || true)
  { [ -n "$LUA" ] && "$LUA" -v 2>&1 | grep -q 'Lua 5\.4'; } || { echo "need Lua 5.4 (lua5.4) for the Lua example"; exit 1; }
  "$LUA" "$ROOT/tools/lua-check.lua" "$P/greeter" --invoke greeter.greet "Pilot One" true | tee "$WORK/greeter.txt"
  grep -q 'invoke  greeter.greet -> ok "HI, PILOT ONE!"' "$WORK/greeter.txt"
  # lua-check's sandbox and sco table behave like the runtime's (plugins/lua/sco_lua.c).
  mkdir -p "$CASES/parity"
  printf 'id = parity\nname = Parity\nversion = 1.0\napi = 1.0\nkind = lua\nentry = main.lua\n' > "$CASES/parity/plugin.ini"
  cat > "$CASES/parity/main.lua" <<'EOF'
assert(string.dump == nil, "string.dump")
assert(not pcall(setmetatable, {}, { __gc = print }), "__gc")
assert(sco.subscribe("tick", print))
assert(select(2, sco.subscribe("tick", print)) == "bad_arg", "same function twice")
for i = 1, 63 do assert(sco.subscribe("tick", function() return i end)) end
assert(select(2, sco.subscribe("tick", function() end)) == "too_many", "65th function on one event")
sco.register_command{ name = "parity.add", title = "Add", args = {{ name = "a", type = "int" }},
                      fn = function(a) return tostring(a + 1) end }
assert(select(2, sco.invoke("parity.add", 2.0)) == "bad_arg", "int takes no float")
assert(select(2, sco.invoke("parity.add")) == "bad_arg", "argument count")
local ok, reply = sco.invoke("parity.add", 2)
assert(ok and reply == "3", "invoke")
print("parity", true, nil)
EOF
  "$LUA" "$ROOT/tools/lua-check.lua" "$CASES/parity" | tee "$WORK/parity.txt"
  grep -q "\[parity\] info: parity$(printf '\t')true$(printf '\t')nil" "$WORK/parity.txt"
fi

# A copied template must build outside the SDK folder with -DSCO_SDK.
cp -R "$ROOT/template" "$WORK/mine"
cmake -S "$WORK/mine" -B "$WORK/mine/build" -DSCO_SDK="$ROOT" -DCMAKE_BUILD_TYPE=Release
cmake --build "$WORK/mine/build" --config Release

# The Lua and data-pack examples configure and install on their own too: project(... NONE), so
# no compiler is enabled and the 64-bit check must not fire. greeter builds in its own folder,
# under a name other than build or out (those are always left out): that build folder must not be
# installed with the pack, however the platform spells the temp path.
for ex in greeter travel_pack; do
  if [ $ex = greeter ]; then B="$ROOT/examples/$ex/my-build"; else B="$WORK/$ex-build"; fi
  cmake -S "$ROOT/examples/$ex" -B "$B"
  cmake --install "$B" --config Release --prefix "$WORK/$ex-out"
  "$CHECK" "$WORK/$ex-out/data/plugins/$ex"
  for f in my-build CMakeLists.txt; do
    [ ! -e "$WORK/$ex-out/data/plugins/$ex/$f" ] || { echo "sco_add_pack($ex) installed $f"; exit 1; }
  done
done
echo "sdk zip: every example built and checked from $(basename "$ZIP")"
