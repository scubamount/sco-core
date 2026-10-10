# Lua plugins

A Lua plugin is a folder with `plugin.ini` (`kind = lua`, `entry = main.lua`) and its scripts. sc-offline's bundled `sco-lua` runtime runs the entry script once at load, in a sandbox. The script registers commands and subscribes to events; those callbacks run on the game thread.

> **Version 1.1** (`sdk-v1.1.0`). This page matches the `sco-lua` runtime in sco-core (`plugins/lua/`), whose tests run [`examples/greeter`](../examples/greeter) and [`examples/notebook`](../examples/notebook). Version 1 only grows.

## The sandbox

The script sees only:

- `sco`: the functions below, mirroring [`sco_api.h`](../../docs/api-v1.md),
- `string`, `table`, `math`, `utf8`,
- the safe base functions: `assert`, `error`, `ipairs`, `next`, `pairs`, `pcall`, `select`, `tonumber`, `tostring`, `type`, `xpcall`, `rawequal`, `rawget`, `rawlen`, `rawset`, `setmetatable`, `getmetatable`, and `print` (which logs at `info`).

There is no `io`, `os`, `package`, `require`, `debug`, `dofile`, `loadfile` or FFI, and precompiled (bytecode) chunks are refused. A script can't read or write files or start programs; `sco.store` is storage without file names.

`setmetatable` refuses a `__gc` field (finalizers would run outside the limits below).

Limits, per script:

- **Steps**: loading the script, and each event callback, command or `run_on_game_thread` task, may take 1 million steps (a step is about one VM instruction; long `string.rep`, `find`, `gsub`, `table.sort` and `table.concat` calls count per item, and `string.byte`, `string.char`, `table.pack`, `table.unpack`, long `find` needles and long `[...]` classes count by size). Past that the call fails and the script is disabled; `pcall` can't catch it.
- **Memory**: 64 MiB, and 256 MiB for every script together. An allocation past either fails with a `not enough memory` error. A `pcall` in the script can catch that error; when it reaches the callback, command, task or load, the script is disabled.
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

## `sco.store`

The plugin's own storage, kept across launches: a key-value store and SQL on its own SQLite database, which no other plugin can open (the host service [`sco.storage`](../../docs/storage.md)). Keys are strings of 1-255 bytes, values are strings (any bytes) of at most 1 MiB. [`examples/notebook`](../examples/notebook) uses it.

```lua
local st = sco.store
st.put("spots.home", "Lorville")
local home = st.get("spots.home")            -- "Lorville"; nil when there is no such key
for _, key in ipairs(st.keys("spots.")) do print(key) end

assert(st.begin())                            -- many writes: one transaction, much faster
st.exec("CREATE TABLE IF NOT EXISTS visits(place TEXT, at INTEGER)")
st.exec("INSERT INTO visits VALUES (?, ?)", { "Lorville", 1 })
assert(st.commit())

for _, row in ipairs(st.sql("SELECT place, at FROM visits WHERE at > ?", { 0 })) do
  print(row.place, row.at)
end
st.sql("SELECT place FROM visits", nil, function(row) print(row.place) end)   -- row by row
```

| Call | Returns |
|---|---|
| `sco.store.available()` | Whether the host publishes storage |
| `sco.store.get(key)` | The value (a string), or `nil` when there is none |
| `sco.store.put(key, value)` | `true` |
| `sco.store.delete(key)` | `true` when it removed a value, `false` when there was none |
| `sco.store.keys([prefix [, after [, limit]]])` | A list of keys that start with `prefix` and sort after `after`, in byte order; at most `limit` (1-1000, default 1000). For the next page pass the last key as `after` |
| `sco.store.begin()`, `commit()`, `rollback()` | `true`. One transaction at a time, over key-value and SQL alike; until `commit` nothing is kept, and an unload or crash rolls it back |
| `sco.store.exec(sql [, params])` | The number of rows an `INSERT`, `UPDATE` or `DELETE` changed (other statements: not meaningful) |
| `sco.store.sql(sql [, params])` | A list of rows, each `{ column = value, ... }`; at most 1000 rows, more is `too_many` (add a `LIMIT`) |
| `sco.store.sql(sql, params, fn)` | Calls `fn(row)` for each row until it returns `false`, then `true` |

Every call fails with `nil, err, message`: `err` is the result name (`"bad_arg"`: a refused or malformed statement, a key too long, a transaction already open; `"failed"`: the statement ran and failed; `"too_many"`: the storage quota is full, or too many rows; `"unavailable"`: the host has no storage), and `message` the host's text when it left one (`"no such table: visits"`). A host without storage (an older one, or a product without a data folder) still has `sco.store`; every call answers `nil, "unavailable"`.

- **SQL**: one statement per call, any SQLite statement on the plugin's own database except those [storage refuses](../../docs/storage.md#sql-what-a-plugin-may-run) (`ATTACH`, `VACUUM`, `BEGIN` / `COMMIT` in SQL: use `begin` / `commit`, most `PRAGMA`s, the `sco_*` tables' schema). The key-value store is the table `sco_kv(key, value)`.
- **Parameters**: a list bound to `?`, `?NNN` or `:name` in order, at most 32: integers, numbers, strings (bound as TEXT), booleans (1 or 0); set `n` to bind trailing `nil`s as NULL (`{ "a", nil, n = 2 }`). Column values come back as integers, numbers and strings (TEXT and BLOB); NULL columns are absent from the row.
- **Steps**: each storage call, and each row or key read, counts 100 steps against the [budget](#the-sandbox), on top of the callback's own code.
- **Cursors** are the runtime's: `sql` closes its cursor on every path, including an error in `fn` or the budget running out. Nothing in `sco.store` names a file or a path.
- A transaction is the plugin's until `commit` or `rollback`: a callback that fails between `begin` and `commit` leaves it open (the next `begin` answers `bad_arg`), so roll back on your error paths.

`lua-check.lua` has an in-memory `sco.store` (key-value and transactions; `exec` and `sql` answer `unavailable`, since a stock Lua has no SQLite).

## `sco.settings`

The values of the `[settings]` your `plugin.ini` declares ([format](plugin-ini.md#settings)), read-only: the player changes them in the game menu (the host service `sco.settings`).

```lua
local speed = sco.settings.get("speed")      -- an integer, 5 until the player changes it
local mode  = sco.settings.get("mode")       -- an enum's choice, as a string
local v, err, msg = sco.settings.get("nope") -- nil, "not_found", "this plugin declares no setting 'nope'"
```

| Call | Returns |
|---|---|
| `sco.settings.get(name)` | A boolean, integer, number or string (`bool`, `int`, `float`, `string` or `enum`), or `nil, err, message`: `"not_found"` (no such setting), `"bad_arg"` (a NUL in the name), `"unavailable"` (a host without the service) |

There is no setter. Read again when you need the value (it follows the player's changes), for example inside a command or a tick handler. A failed read can leave a `last_error` note for the C++ and C# wrappers; ignore it from Lua. `lua-check` doesn't provide `sco.settings` yet.

## `sco.datacore`

Present only when the product publishes the host service [`sco.datacore`](../../docs/datacore.md#the-scodatacore-service) (check `if sco.datacore then`); `lua-check.lua` has no stand-in for it. It queues DataCore overrides, the operations of a data pack's `.toml`, from a script. In sc-offline the game loads DataCore before scripts run, so a committed patch is saved and **applies from the next launch**; overrides that never change belong in a [data pack](data-packs.md#game-data-datacore).

```lua
local eos = "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem"
local p = sco.datacore.begin()            -- or begin({ atomic = false }); nil, err on failure
p:set(eos, "Components[SCItemQuantumDriveParams].params.spoolUpTime", 3.5)
local fast = p:add_instance("SCItemQuantumDriveParams", eos, "Components[SCItemQuantumDriveParams]")
p:set_pointer("EntityClassDefinition.QDRV_WETK_S01_Beacon_SCItem", "Components[SCItemQuantumDriveParams]", fast)
assert(p:commit())
for _, r in ipairs(p:report()) do print(r.op, r.state, r.reason) end
```

| Call | Returns |
|---|---|
| `sco.datacore.state()` | `"open"` (before the load) or `"loaded"` |
| `sco.datacore.begin([{ atomic = false }])` | A patch, or `nil, err` |
| `p:set(record, field, value)`, `p:append(record, field, value)` | `true`, or `false, err` |
| `p:add_instance(type [, clone_record [, clone_field]])` | An instance (use it as a value or with `set_pointer`), or `nil, err` |
| `p:set_pointer(record, field, instance)` | `true`, or `false, err` |
| `p:add_record(type, name, clone_record [, guid [, file_path]])` | A new record cloned from `clone_record` (same struct), returned as an instance (its root; later calls also name the record by `name`, and `{ ref = name }` points a reference field at it), or `nil, err`. No `guid`: a stable one derived from the plugin id and name. `nil, "unavailable"` on a host whose `sco.datacore` is 1.0 |
| `p:commit()`, `p:discard()` | `true`, or `false, err` |
| `p:report()` | A list of `{ state = "queued" \| "applied" \| "skipped" \| "refused", op = n, reason = "..." }`: one per operation (`op` 1, 2, ...), then the patch's (`op` 0) |

Values: integers, numbers, strings (also enum options), booleans, `nil` (a null pointer), an instance from `add_instance`, `{ guid = "..." }`, `{ enum = "Option" }`, `{ ref = "RecordName" }` (a reference field's target). A `record` is a record name or `"guid:..."`. Errors are the same strings as elsewhere (`"bad_arg"`, `"not_found"`, `"unavailable"`, ...). Every call counts against the step budget like other host calls, and the sandbox is unchanged: scripts never touch files; the host writes the saved patch.

## `sco.game.actors`

Present only when the Star Citizen game pack publishes [`game.actors`](../../docs/game-services.md) (check `if sco.game and sco.game.actors then`); `lua-check.lua` has no stand-in for it. Lua gets the game services **read-only**: queries, never a function that changes the game, so a script owns no NPCs.

| Call | Returns |
|---|---|
| `sco.game.actors.local_player()` | Your actor id and entity id (integers; the game's 64-bit ids, which can come back negative), or `nil, err, message` (`"not_found"` before you've spawned, `"unavailable"` when `game.actors.local_player` isn't ready) |

Ids are session handles: use them on the same tick, never store them.

## `sco.game.vehicles`

Present only when the Star Citizen game pack publishes [`game.vehicles`](../../docs/api-v1.md#gamevehicles-10-game-pack) (check `if sco.game and sco.game.vehicles then`); `lua-check.lua` has no stand-in for it. Read-only: scripts can list seats but can't seat, eject or power on (those take `self` and change the game; use C, C++ or C#). Ids are the game's 64-bit entity ids, carried in Lua integers bit for bit: pass them back unchanged and never store one.

| Function | Returns |
|---|---|
| `sco.game.vehicles.player_ship()` | The id of the ship you're aboard |
| `sco.game.vehicles.seats(ship)` | A list of seats, each `{ index, seat_id, occupant_id, priority, name, usable, usable_known, occupied, pilot }` (`index` counts from 0, as the game pack's), and `true` when the ship has more than were listed (256 at most) |
| `sco.game.vehicles.seat_occupant(ship, index)` | The id of the actor in the seat, 0 when it's empty |

On failure each returns `nil, err, message` (`err` as for `sco.store`: `"unavailable"`, `"not_found"`, ...; `message` from the game pack's `last_error`).

## Check a script

Without the game, with a stock Lua 5.4:

```sh
lua5.4 tools/lua-check.lua examples/greeter
lua5.4 tools/lua-check.lua examples/greeter --cap teleport --invoke greeter.greet "Pilot One" true
```

`lua-check.lua` runs the script with the sandbox's globals and a stand-in `sco` table that checks the rules above, fires `game.ready`, three ticks and `game.exit`, and runs every command without arguments plus the one named by `--invoke`. It has no instruction budget and no game.
