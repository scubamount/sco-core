# Lua plugins

A Lua plugin is a folder with `plugin.ini` (`kind = lua`, `entry = main.lua`) and its scripts. sc-offline's bundled `sco-lua` runtime runs the entry script once at load, in a sandbox. The script registers commands and subscribes to events; those callbacks run on the game thread.

> **Status: 1.0-pre.** This page matches the `sco-lua` runtime in sco-core (`plugins/lua/`), whose tests run [`examples/greeter`](../examples/greeter). Until `sdk-v1.0.0` it can still change.

## The sandbox

The script sees only:

- `sco`: the functions below, mirroring [`sco_api.h`](../../docs/api-v1.md),
- `string`, `table`, `math`, `utf8`,
- the safe base functions: `assert`, `error`, `ipairs`, `next`, `pairs`, `pcall`, `select`, `tonumber`, `tostring`, `type`, `xpcall`, `rawequal`, `rawget`, `rawlen`, `rawset`, `setmetatable`, `getmetatable`, and `print` (which logs at `info`).

There is no `io`, `os`, `package`, `require`, `debug`, `dofile`, `loadfile` or FFI, and precompiled (bytecode) chunks are refused. A script can't read or write files or start programs.

`setmetatable` refuses a `__gc` field (finalizers would run outside the limits below).

Limits, per script:

- **Steps**: loading the script, and each event callback, command or `run_on_game_thread` task, may take 1 million steps (a step is about one VM instruction; long `string.rep`, `find`, `gsub`, `table.sort` and `table.concat` calls count per item, and `string.byte`, `string.char`, `table.pack`, `table.unpack`, long `find` needles and long `[...]` classes count by size). Past that the call fails and the script is disabled; `pcall` can't catch it.
- **Memory**: 64 MiB. An allocation past it fails with a `not enough memory` error. A `pcall` in the script can catch that error; when it reaches the callback, command, task or load, the script is disabled.
- **Tasks**: 16 `run_on_game_thread` calls waiting at once. Past that it returns `false, "too_many"`.
- **Subscriptions**: 64 functions per event name, 16 event names. Past that `sco.subscribe` returns `false, "too_many"`.
- **Errors**: an error raised in a callback is caught and logged; a command that raises fails with the error text as its reply. After 3 errors the script is disabled.

A disabled script stays listed but does nothing: its event callbacks stop, its commands answer `unavailable`, and `sco.subscribe`, `sco.register_command` and `sco.run_on_game_thread` return `false, "unavailable"` (a script disabled while it loads is refused). `mod.log` gets `[<id>] error: script disabled: <why>`.

## `sco` functions

| Function | Returns | Does |
|---|---|---|
| `sco.host_version()` | string | `"sc-offline 0.8.0"` |
| `sco.has(name)` | boolean | Whether a capability is available on this game build |
| `sco.status(message)` | | Shows `<id>: message` on the status line |
| `sco.log(level, message)` | | Writes `[<id>] message` to `mod.log`. `level` is `"info"`, `"warn"` or `"error"` |
| `sco.subscribe(event, fn)` | `true`, or `false, err` | Calls `fn(event, data)` on each `event`: `game.ready`, `tick` (`data` = milliseconds), `game.exit` |
| `sco.unsubscribe(event, fn)` | `true`, or `false, err` | Removes that subscription |
| `sco.run_on_game_thread(fn)` | `true`, or `false, err` | Runs `fn()` on the next tick. At most 16 can wait at once (`too_many`) |
| `sco.register_command(t)` | `true`, or `false, err` | Registers a command (below) |
| `sco.invoke(name, ...)` | `true, reply`, or `false, err[, reply]` | Runs any command, sc-offline's or a plugin's, now. Arguments must match the command's types exactly (`int` takes an integer, not `"2"` or `2.0`). When the command itself fails, the third value is its reply |
| `sco.list_commands()` | table | `{ {name, title, help, capability, args = { {name, type, help}, ... } }, ... }` |
| `sco.bind_hotkey(chord, command, ...)` | `true` or `false, why, message` | Binds a key chord (`"ctrl+alt+h"`) to a command with the extra values as its arguments: typed by the command's definition when it is registered, else by their Lua types. `message` names who holds a taken chord. Withdrawn when the script unloads. Needs the host service `sco.ui` (`unavailable` without it). Tabs and overlays from Lua come later |
| `sco.unbind_hotkey(chord)` | `true` or `false, why, message` | Removes the script's own binding |

`err` is the result name from the C API in lower case: `"bad_arg"`, `"unavailable"`, `"not_found"`, `"too_many"`, `"crashed"`.

## Commands

```lua
sco.register_command{
  name  = "greeter.greet",          -- "<id>.<action>", at most 63 bytes
  title = "Greet",                  -- menu label, at most 63 bytes
  help  = "Says hi",                -- optional, at most 255 bytes
  capability = nil,                 -- optional: the has() name the command needs
  args  = {                         -- optional, at most 16
    { name = "name",  type = "string", help = "Who to greet" },
    { name = "shout", type = "bool" },
  },
  fn = function(name, shout) return "Hi, " .. name end,
}
```

`type` is `"int"`, `"float"`, `"string"` or `"bool"`. The host checks the arguments against `args` before calling `fn`, which gets them as plain Lua values in order. `fn` returns the reply shown on the status line (a string, or nothing); `error(...)` makes the command fail.

## Check a script

Without the game, with a stock Lua 5.4:

```sh
lua5.4 tools/lua-check.lua examples/greeter
lua5.4 tools/lua-check.lua examples/greeter --cap teleport --invoke greeter.greet "Pilot One" true
```

`lua-check.lua` runs the script with the sandbox's globals and a stand-in `sco` table that checks the rules above, fires `game.ready`, three ticks and `game.exit`, and runs every command without arguments plus the one named by `--invoke`. It has no instruction budget and no game.
