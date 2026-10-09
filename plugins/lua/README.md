# sco-lua

The Lua runtime for `kind = lua` plugins: Lua 5.4.8 in a sandbox, talking to the host only
through the plain-C plugin API ([`include/sco_api.h`](../../include/sco_api.h)). It includes no
other sco-core header, so it is also the proof that a plugin can do everything through that table.

What a script sees and how to write one: [`sdk/docs/lua.md`](../../sdk/docs/lua.md).

## Files

| Path | What |
|---|---|
| `sco_lua.h` / `sco_lua.c` | Load and unload a script, the `sco` table, the sandbox and the limits |
| `sco_lua_user.h` | `SCO_LUA_STEP`, included by the vendored `luaconf.h` |
| `third_party/lua/` | Lua 5.4.8 sources (MIT, [`LICENSE`](third_party/lua/LICENSE), [`VERSION`](third_party/lua/VERSION)); `lua.c`, `luac.c` and the Makefile left out |

## Host side

The host links `sco_lua.c` and the Lua sources in and gives the loader the two entry points:

```cpp
static const sco::plugins::ScriptRuntime kLua{ sco_lua_load, sco_lua_unload };
sco::plugins::LoadScript(plugin, api, sco::host::NewPlugin(id), kLua);   // game thread
...
sco::plugins::UnloadAll(list, sco::plugins::PlatformModuleOps(), &kLua);
```

Everything runs on the game thread. `LoadScript` reads the entry file (1 MiB at most) and calls
`sco_lua_load`, guarded like a native plugin call. On unload the host releases the handle first,
so nothing can call into the script, then `sco_lua_unload` frees the Lua state.

## Sandbox

- Libraries: the base functions listed in `sdk/docs/lua.md`, `string` (without `string.dump`),
  `table`, `math`, `utf8`. No `io`, `os`, `package`, `require`, `debug`, `coroutine`, `load`,
  `dofile`, `loadfile` or `collectgarbage`.
- Text chunks only: the entry script is loaded with mode `"t"`, so bytecode is refused.
- `setmetatable` refuses `__gc`: Lua runs finalizers with hooks off, outside the step budget.

## Limits

| Limit | Value | Past it |
|---|---|---|
| Steps per entry (load, one event, one command, one task) | 1,000,000 | The entry fails, the script is disabled |
| Memory per script | 64 MiB | The allocation fails (`not enough memory`). A `pcall` in the script can catch that; when it ends the entry, the script is disabled |
| Errors in callbacks | 3 | The script is disabled |
| Functions subscribed to one event | 64 | `sco.subscribe` returns `false, "too_many"` |
| Event names subscribed to | 16 | `sco.subscribe` returns `false, "too_many"` |
| `run_on_game_thread` tasks waiting | 16 | `sco.run_on_game_thread` returns `false, "too_many"` |
| Entry script | 1 MiB | Refused |
| Scripts | 64 | Refused |
| Nested entries (a command invoking a command ...) | 8 | `SCO_TOO_MANY` |

A step is one VM instruction, or one iteration of a C loop in the string and table libraries that
the VM's count hook can't see. Those loops are patched to call `SCO_LUA_STEP`; every patched line
is marked `/* sco-lua */`. Calls that move many values at once are charged in proportion to the
work, 1 step plus 1 per 64 values or bytes (1 per 16 bytes of a `[...]` class):

| File | Loops |
|---|---|
| `lstrlib.c` | `string.rep`, pattern matching (`match`, `max_expand`, `%b`), plain `find` (per candidate, plus the needle length), `gsub`, `string.byte` and `string.char` (by count), `[...]` classes (per test, by class length) |
| `ltablib.c` | `table.insert`, `table.remove`, `table.move`, `table.concat`, `table.sort`, `table.pack` and `table.unpack` (by count) |
| `luaconf.h` | Includes `sco_lua_user.h` |

Once a script is past its budget, every further instruction raises, so a `pcall` in the script
can't keep it running. A disabled script stays loaded but does nothing: its subscriptions are
dropped, its commands answer `SCO_UNAVAILABLE` until the plugin unloads, and `sco.subscribe`,
`sco.register_command` and `sco.run_on_game_thread` return `false, "unavailable"` (an outer frame
of the script can still be running). A script disabled while it loads fails the load.

To update Lua: replace `third_party/lua/src` with the new release's `src/` (minus `lua.c`,
`luac.c`, `Makefile`), re-apply the `/* sco-lua */` lines, update `VERSION` with the new
tarball's sha256 from <https://www.lua.org/ftp/>, and run `tools/test.sh`.
