# Lua plugins

A Lua plugin is a folder with `plugin.ini` (`kind = lua`, `entry = main.lua`) and its scripts. sc-offline's bundled `sco-lua` runtime runs the entry script once at load, in a sandbox. The script registers commands and subscribes to events; those callbacks run on the game thread.

> **Status: 1.0-pre.** The `sco-lua` runtime is still being built. This page and [`examples/greeter`](../examples/greeter) follow the SDK plan's contract; until `sdk-v1.0.0` the runtime's binding is the reference, and this page changes with it.

## The sandbox

The script sees only:

- `sco`: the functions below, mirroring [`sco_api.h`](../../docs/api-v1.md),
- `string`, `table`, `math`, `utf8`,
- the safe base functions: `assert`, `error`, `ipairs`, `next`, `pairs`, `pcall`, `select`, `tonumber`, `tostring`, `type`, `xpcall`, `rawequal`, `rawget`, `rawlen`, `rawset`, `setmetatable`, `getmetatable`, and `print` (which logs at `info`).

There is no `io`, `os`, `package`, `require`, `debug`, `dofile`, `loadfile` or FFI, and precompiled (bytecode) chunks are refused. A script can't read or write files or start programs.

Each callback has an instruction budget (1 million by default). A callback that goes past it is stopped and the script is disabled, with one line in `mod.log`. An error raised in a callback is caught: a command that raises returns its error as a failed result; an event callback that raises is logged.

## `sco` functions

| Function | Returns | Does |
|---|---|---|
| `sco.host_version()` | string | `"sc-offline 0.8.0"` |
| `sco.has(name)` | boolean | Whether a capability is available on this game build |
| `sco.status(message)` | | Shows `<id>: message` on the status line |
| `sco.log(level, message)` | | Writes `[<id>] message` to `mod.log`. `level` is `"info"`, `"warn"` or `"error"` |
| `sco.subscribe(event, fn)` | `true`, or `false, err` | Calls `fn(event, data)` on each `event`: `game.ready`, `tick` (`data` = milliseconds), `game.exit` |
| `sco.unsubscribe(event, fn)` | `true`, or `false, err` | Removes that subscription |
| `sco.run_on_game_thread(fn)` | `true`, or `false, err` | Runs `fn()` on the next tick |
| `sco.register_command(t)` | `true`, or `false, err` | Registers a command (below) |
| `sco.invoke(name, ...)` | `true, reply`, or `false, err` | Runs any command, sc-offline's or a plugin's |
| `sco.list_commands()` | table | `{ {name = ..., title = ...}, ... }` |

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
