# Plugin API v1 reference (`sco_api.h`)

Version 1.1, released in `sdk-v1.1.0`. This is the plain-C interface a plugin uses to talk to its host (sc-offline). It is the only header a plugin includes; the C++ headers in `include/sco/` are internal and are not part of it.

**Status: stable.** Since `sdk-v1.1.0` the header, this page and [`tests/abi_v1.c`](../tests/abi_v1.c) only grow: version 1 adds fields and functions at the end of structs and never changes what was released (see [Compatibility](#compatibility)). sco-core implements the table ([`sco::host::BuildApi`](api.md#scohosth-the-hosts-sco_api-table)) and the loader ([Plugins](plugins.md)); sc-offline hosts it.

Plugins are native DLLs and run with the game's full rights. Only install plugins you trust. Plugins are GPL-3.0, like sco-core; there is no linking exception.

## Contents

- [Rules](#rules)
- [Versions and export macro](#versions-and-export-macro)
- [Results and log levels](#results-and-log-levels)
- [The plugin's exports](#the-plugins-exports)
- [The host's table: `sco_api`](#the-hosts-table-sco_api)
- [Commands](#commands)
- [Events](#events)
- [Capabilities](#capabilities)
- [Services (1.1)](#services-11)
- [Host-owned services](#host-owned-services)
- [Built-in services](#built-in-services)
- [Raw handlers (1.1)](#raw-handlers-11)
- [Threading](#threading)
- [Compatibility](#compatibility)
- [Layout](#layout)

## Rules

- Plain C with `extern "C"`. No C++ types, no exceptions and no varargs cross the boundary; each side formats its own strings.
- `sco_api`, `sco_command` and `sco_plugin_info` start with `uint32_t size`, set to `sizeof` of the struct as that side was built. The host reads only the fields the plugin's `size` covers; a plugin calls only the functions the host's `size` covers.
- `sco_arg_def` arrays are read with the stride in `sco_command.arg_def_size`, so `sco_arg_def` can grow in a later minor. `sco_arg` is frozen for major 1: its 8-byte union already holds every argument type.
- Every enum is 4 bytes. Each has a `_FORCE32` member that pins the size; never pass it.
- Pointers handed to a plugin are valid for the duration of the call unless this page says otherwise.
- Strings are UTF-8 and NUL-terminated.
- Calls that can fail return `sco_result`. Nothing throws. A fault in game code reached from a call comes back as `SCO_CRASHED`.
- 64-bit only. The target is x64 Windows (MSVC or clang-cl); the host tests build the same layout on 64-bit macOS and Linux.

## Versions and export macro

| Name | Value | Meaning |
|---|---|---|
| `SCO_API_MAJOR` | `1` | Changes only for a break. The host refuses a plugin with a different major |
| `SCO_API_MINOR` | `0` | Grows when functions or fields are added at the end. The host refuses a plugin built for a newer minor than its own |
| `SCO_EXPORT` | `__declspec(dllexport)` on Windows, default visibility on gcc/clang | Put on the three plugin exports |

## Results and log levels

```c
typedef enum sco_result {
    SCO_OK = 0, SCO_UNAVAILABLE = 1, SCO_NOT_FOUND = 2, SCO_BAD_ARG = 3,
    SCO_CRASHED = 4, SCO_WRONG_THREAD = 5, SCO_TOO_MANY = 6,
    SCO_FAILED = 7   /* 1.1 */
} sco_result;

typedef enum sco_log_level { SCO_LOG_INFO = 0, SCO_LOG_WARN = 1, SCO_LOG_ERROR = 2 } sco_log_level;
```

| Result | Means |
|---|---|
| `SCO_OK` | Done |
| `SCO_UNAVAILABLE` | The capability the call needs is missing on this game build |
| `SCO_NOT_FOUND` | Unknown command, or nothing to unsubscribe |
| `SCO_BAD_ARG` | A NULL where a value is needed, a bad name, or arguments that don't match the command's definition |
| `SCO_CRASHED` | The call faulted inside game or plugin code; the host survived |
| `SCO_WRONG_THREAD` | Called from a thread the function doesn't allow |
| `SCO_TOO_MANY` | A queue or table is full |
| `SCO_FAILED` | 1.1. The command or service ran but couldn't do its job (no saved spot, a file it couldn't write); the reply says why. A command built for 1.0 answers `SCO_UNAVAILABLE` for this, so treat both as "didn't happen" |

## The plugin's exports

```c
typedef struct sco_plugin_info {
    uint32_t size;
    uint16_t api_major, api_minor;
    const char* name;
    const char* version;
    const char* author;
} sco_plugin_info;

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void);
SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self);
SCO_EXPORT void       sco_plugin_unload(void);
```

| Field | Set to |
|---|---|
| `size` | `sizeof(sco_plugin_info)` |
| `api_major`, `api_minor` | `SCO_API_MAJOR`, `SCO_API_MINOR` (what the plugin was built against) |
| `name` | Short lowercase name, `"hello"`. It prefixes the plugin's log lines, status messages and command names |
| `version` | `"1.0.0"` |
| `author` | Free text |

| Export | Called | Does |
|---|---|---|
| `sco_plugin_query` | First, before anything else | Returns the plugin's info. The pointer must stay valid until the DLL unloads; use a `static` |
| `sco_plugin_load` | Once, after the version check, on the game thread | `api` and `self` stay valid until `sco_plugin_unload`; keep both. Return `SCO_OK` to stay loaded; anything else unloads the plugin |
| `sco_plugin_unload` | Once, as the game shuts down | Last call into the plugin |

`sco_plugin` is opaque: the host's handle for one plugin. Pass `self` back to every call that takes it.

The header also declares `sco_plugin_query_fn`, `sco_plugin_load_fn` and `sco_plugin_unload_fn`, the pointer types a host loader casts `GetProcAddress` results to.

A minimal plugin:

```c
#include "sco_api.h"

static const sco_api* api;
static sco_plugin*    me;

static const sco_plugin_info info = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "hello", "1.0.0", "you"
};

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) { return &info; }

SCO_EXPORT sco_result sco_plugin_load(const sco_api* a, sco_plugin* self) {
    api = a; me = self;
    api->status(me, "Hello from a plugin");
    return SCO_OK;
}

SCO_EXPORT void sco_plugin_unload(void) {}
```

## The host's table: `sco_api`

```c
typedef struct sco_api {
    uint32_t size;
    uint16_t major, minor;
    const char* (*host_version)(void);
    int        (*has)(const char* capability);
    sco_result (*run_on_game_thread)(sco_plugin* self, sco_task_fn fn, void* ctx);
    sco_result (*subscribe)(sco_plugin* self, const char* event, sco_event_fn fn, void* ctx);
    sco_result (*unsubscribe)(sco_plugin* self, const char* event, sco_event_fn fn);
    void       (*status)(sco_plugin* self, const char* message);
    void       (*log)(sco_plugin* self, sco_log_level level, const char* message);
    sco_result (*register_command)(sco_plugin* self, const sco_command* cmd);
    sco_result (*invoke)(sco_plugin* self, const char* name, const sco_arg* args, uint32_t nargs,
                         sco_invoke_done done, void* ctx);
    uint32_t   (*list_commands)(const sco_command** out, uint32_t max);
    /* 1.1 */
    sco_result (*provide_service)(sco_plugin* self, const char* name, uint32_t version, const void* vtable);
    sco_result (*query_service)(const char* name, uint32_t min_version, const void** out_vtable);
    sco_result (*release_service)(sco_plugin* self, const char* name);
    sco_result (*invoke_raw)(sco_plugin* self, const char* name, const void* in_bytes, uint32_t in_size,
                             void* out_bytes, uint32_t* inout_out_size);
    sco_result (*register_raw)(sco_plugin* self, const char* name, const char* capability,
                               sco_raw_fn fn, void* ctx);
} sco_api;

typedef sco_result (*sco_raw_fn)(const void* in, uint32_t in_size, void* out,
                                 uint32_t* inout_out_size, void* ctx);   /* 1.1 */

typedef void (*sco_task_fn)(void* ctx);
typedef void (*sco_event_fn)(const char* event, const void* data, void* ctx);
```

| Member | Thread | Does |
|---|---|---|
| `size` | | `sizeof(sco_api)` as the host was built. Check it before calling a function added after 1.0 |
| `major`, `minor` | | The host's API version |
| `host_version()` | Any | The host's name and version, `"sc-offline 0.8.0"`. Static string |
| `has(capability)` | Any | 1 if the capability is available on this game build, else 0 (unknown names too). See [Capabilities](#capabilities) |
| `run_on_game_thread(self, fn, ctx)` | Any | Queues `fn(ctx)` to run on the game thread at the next tick, in the order queued. Posting never fails for lack of room in a normal burst: the first 256 waiting tasks go into a fixed ring (no allocation), the rest into an overflow that keeps the same order. `SCO_TOO_MANY` only when 65,536 tasks are already waiting (a cap so a plugin posting in a loop can't use up the game's memory) or memory runs out; nothing is queued then |
| `subscribe(self, event, fn, ctx)` | Any | Calls `fn(event, data, ctx)` for each dispatch of `event`. Applies from the next dispatch |
| `unsubscribe(self, event, fn)` | Any | Removes the subscription for that `event` and `fn`; `SCO_NOT_FOUND` if there is none. Applies at once: a dispatch in progress won't call `fn` again. See [Freeing ctx](#freeing-ctx) |
| `status(self, message)` | Any | Shows `hello: message` on the status line |
| `log(self, level, message)` | Any | Writes `[hello] message` to `mod.log` |
| `register_command(self, cmd)` | Any | Adds a command; see [Commands](#commands) |
| `invoke(self, name, args, nargs, done, ctx)` | Any | Runs a command; see [Commands](#commands) |
| `provide_service(self, name, version, vtable)` | Any | 1.1. Publishes a function table for other plugins; see [Services](#services-11) |
| `query_service(name, min_version, out_vtable)` | Any | 1.1. Finds a published table; see [Services](#services-11) |
| `release_service(self, name)` | Any | 1.1. Withdraws one of this plugin's services |
| `register_raw(self, name, capability, fn, ctx)` | Any | 1.1. Registers a raw handler: bytes in, bytes out; see [Raw handlers](#raw-handlers-11) |
| `invoke_raw(self, name, in, in_size, out, inout_out_size)` | Game | 1.1. Calls a raw handler now; see [Raw handlers](#raw-handlers-11) |
| `list_commands(out, max)` | Any | Writes up to `max` command pointers to `out` and returns the number of live commands. Call with `max = 0` to get the count. The pointers stay valid until their owner unloads |

## Commands

Features expose actions as named commands, sc-offline's own and plugins' alike. The menu draws a control per argument and calls `invoke`; a plugin calls the same function. Three functions cover every feature.

```c
typedef enum sco_arg_type { SCO_ARG_INT = 0, SCO_ARG_FLOAT = 1, SCO_ARG_STRING = 2, SCO_ARG_BOOL = 3 } sco_arg_type;

typedef struct sco_arg {
    uint32_t type; uint32_t _pad;
    union { int64_t i; double f; const char* s; } v;
} sco_arg;

typedef struct sco_arg_def { const char* name; uint32_t type; uint32_t _pad; const char* help; } sco_arg_def;

typedef sco_result (*sco_command_fn)(const sco_arg* args, uint32_t nargs, void* ctx,
                                     char* reply, uint32_t reply_size);
typedef void       (*sco_invoke_done)(sco_result r, const char* reply, void* ctx);

typedef struct sco_command {
    uint32_t size; uint32_t _pad0;
    const char* name;
    const char* title;
    const char* help;
    const char* capability;
    const sco_arg_def* args; uint32_t nargs; uint32_t arg_def_size;
    sco_command_fn fn; void* ctx;
} sco_command;
```

`sco_arg.type` and `sco_arg_def.type` hold a `sco_arg_type`. Read the union member that matches: `i` for `SCO_ARG_INT` and `SCO_ARG_BOOL` (0 or 1; anything else is `SCO_BAD_ARG`), `f` for `SCO_ARG_FLOAT`, `s` for `SCO_ARG_STRING`. Set `_pad` fields to 0.

| `sco_command` field | Set to |
|---|---|
| `size` | `sizeof(sco_command)` |
| `name` | `<owner>.<action>`: `"spawn.ship"`, `"teleport.save"`, `"hello.wave"`. A plugin may only register names that start with its own `name` and a dot |
| `title` | The menu label, `"Spawn ship"` |
| `help` | One line of help, or NULL |
| `capability` | The `has()` name this command needs, or NULL for always available |
| `args`, `nargs` | The argument definitions, in order (at most 16) |
| `arg_def_size` | `sizeof(sco_arg_def)` |
| `fn`, `ctx` | Called as `fn(args, nargs, ctx, reply, reply_size)` |

Behavior:

- `register_command` copies `name` (63 bytes at most), `title` (63), `help` (255), `capability` (63) and the arg defs with their `name` (31) and `help` (127). Only `fn` and `ctx` must stay valid until the plugin unloads. `SCO_BAD_ARG` for a longer string, a duplicate name, a name outside the plugin's prefix, a reserved prefix (`sco`, `host`, `menu`, `game`) or a prefix another plugin or a host feature already uses. The prefix is the plugin's `name`, which may not contain a dot.
- Commands run on the game thread. `invoke` from the game thread runs the command at once, calls `done` once before returning and returns the same result.
- From any other thread `invoke` copies the name and arguments, queues the call and returns `SCO_OK`; `done` then runs exactly once, on the game thread. Any other return (`SCO_BAD_ARG`, or `SCO_TOO_MANY` when the queue is full or memory runs out) means nothing was queued and `done` is never called. A call still queued when the calling plugin unloads is dropped, and `done` isn't called.
- Before calling `fn` the host checks the argument count and types (`SCO_BAD_ARG`) and the capability (`SCO_UNAVAILABLE`); `done` receives that result and `fn` isn't called.
- `fn` writes a short human message into `reply`, at most `reply_size` bytes including the NUL: `"Spawned Cutlass Black"`. The menu shows it on the status line. `done` gets it as `reply`, valid only during the call.
- `done` may be NULL when the caller doesn't need the result.

Not settled for 1.0: how the menu reads lists it shows today (ship classes, bookmarks, outfits, contract offers). It is decided at step 5 with the real menu; the candidates are commands that return a list in `reply`, or a `query` function added in 1.1.

## Events

| Event | When | `data` |
|---|---|---|
| `game.ready` | After the offline patches and the signature report, before the first tick | `NULL` |
| `tick` | Every main-thread tick (about 10 per second), on the game thread | `const uint32_t*`: milliseconds now |
| `game.exit` | When the game quits, before plugins unload. Best effort: a crash or a killed process never sends it, so don't rely on it to save data | `NULL` |

`data` is valid only during the callback.

### Freeing ctx

After `unsubscribe` the host never calls `fn(…, ctx)` again, but a call may already be running:

- On the game thread (in a tick callback, a task or a command, or inside `fn` itself), nothing else is running. Free `ctx` as soon as `unsubscribe` returns.
- On another thread, the game thread may be inside `fn(ctx)` right now. Call `run_on_game_thread` after `unsubscribe` returns and free `ctx` in that task. Tasks never run while a dispatch is in progress, so by then `fn` has returned.

## Capabilities

`has()` answers from sco-core's signature registry and sc-offline's feature readiness: one name per feature that logs `[+] ... ready` today, such as `"teleport"`, `"spawn.ship"`, `"console"`, `"outfits"` and `"contracts"`. Unknown names return 0. In v1 plugins reach features only through commands; `has()` lets a plugin grey out its own UI and say why.

## Services (1.1)

A service is a C function table one plugin publishes for others to call directly, without
parsing a reply string. The table's layout is the provider's contract; start it with a
`uint32_t size` like every struct here, so it can grow.

```c
static const my_spatial_v1 kTable = { sizeof(my_spatial_v1), get_position, set_position };
api->provide_service(self, "nav.spatial", 0x00010000, &kTable);

const my_spatial_v1* s = NULL;   /* in another plugin */
if (api->size > offsetof(sco_api, query_service) &&
    api->query_service("nav.spatial", 0x00010000, (const void**)&s) == SCO_OK) { ... }
```

- **Names** are `[a-z0-9_.]`, 1-63 characters, with no leading, trailing or doubled `.`, and must be the plugin's id or start with `<id>.`, like command names. A name already published is `SCO_BAD_ARG`.
- **Versions** are `(major << 16) | minor`. `query_service` answers `SCO_OK` when the service has the same major as `min_version` and is at least as new; `SCO_UNAVAILABLE` when the major differs or it is older; `SCO_NOT_FOUND` when nothing is published under that name. `out_vtable` is `NULL` unless `SCO_OK`.
- **Withdrawing:** `release_service(self, name)` withdraws one of your services (`SCO_NOT_FOUND` if it isn't yours).
- **Lifetime:** the host keeps the name, version and pointer, never the table's contents, and never calls into it. When the provider unloads or crashes its services are withdrawn. A table from a built-in plugin stays valid for as long as any other plugin is loaded (built-ins unload last); a table from another plugin may go away when that plugin does, so query it when you need it rather than keeping it across ticks.
- **Faults:** a call into another plugin's table runs under the caller's crash guard, so a fault in the provider's code marks the caller crashed.
- Threads: both functions work from any thread; what thread the table's own functions may be called from is part of the provider's contract.
- **Entity ids, never pointers.** A service that deals with game objects takes and returns entity and zone ids: opaque `uint64_t` values, as the game's own are. It resolves the id on every call, on the game thread, and answers `SCO_NOT_FOUND` (or `SCO_UNAVAILABLE`) when the entity has streamed out. It never hands out a pointer into the game: Star Citizen streams objects in and out (object container streaming), so a pointer a caller stores dangles once its object goes, and the next call through it crashes the caller, not the provider. Treat an id as a value to pass back, not a number to decode: the player's id in the sc-offline spawn test is `0xCAE11A7400000000`, a tagged value, not a small index. See [Framework § Lessons](framework.md#6-services-hand-out-ids-never-pointers).

## Host-owned services

Some services are published by the host itself rather than by a plugin. They live under the reserved plugin id `sco` ([design decision 10](design/vfs-datacore.md#decisions-maintainer-2026-10-09)), so their names are `sco.<name>`, and plugins find them with the same `query_service`; `sco_api.h` doesn't change for them. No plugin can publish under `sco`: the id is reserved (`plugin.ini` and the host refuse it, and discovery refuses a folder named `sco`), and `provide_service` only takes names under the caller's own id.

- **Lifetime:** host services are published before any plugin loads and withdrawn at host shutdown, after every plugin has unloaded. So unlike another plugin's table, a host service's table may be kept for the plugin's whole life.
- **Optional:** a host or a game build that doesn't offer one doesn't publish it, and `query_service` answers `SCO_NOT_FOUND`.
- **Versioned on their own**, `(major << 16) | minor` like any service, each with its own plain-C header beside `sco_api.h` and its own layout pin.

| Service | Version | Header | What |
|---|---|---|---|
| `sco.storage` | 1.0 | [`sco_storage.h`](../include/sco_storage.h) | Per-plugin persistent storage: key-value and SQL over SQLite, in the plugin's own database. [Storage](storage.md) |
| `sco.datacore` | 1.0 | [`sco_datacore.h`](../include/sco_datacore.h) | DataCore overrides from code: the operations of a data pack's `.toml` files, queued call by call; after the game's DataCore load they are saved and apply from the next launch. Published only when the product enables it (`sco::app::Platform::dataCore`; sc-offline will in design plan PR 8). [The sco.datacore service](datacore.md#the-scodatacore-service) |
| `sco.ipc` | 1.0 | [`sco_ipc.h`](../include/sco_ipc.h) | Local shared-memory channels to another program on the same PC (a bridge): `Local\SCO_<plugin id>.<name>`, the current user only, rings and seqlock blocks laid out with the MIT wire [`sc_ipc.h`](../include/sc_ipc.h), which the other program includes alone. [IPC](ipc.md) |
| `sco.net` | 1.0 | [`sco_net.h`](../include/sco_net.h) | Typed message channels between the players of a private session the product opened (`<plugin id>.<name>`, unreliable or reliable, per-plugin send quotas); callbacks on the game thread, `net.state` / `net.peer` events. Published with the capability `sco.net` (`requires = sco.net`); plugins can't open or join sessions. [Multiplayer messages](net.md) |
| `sco.ui` | 1.0 | [`sco_ui.h`](../include/sco_ui.h) | Tabs, overlays and badges in the product's menu, drawn by the product through the plugin's draw function, and hotkeys: key chords bound to commands. [UI](ui.md) |

The host side is `sco::host::ProvideHostService` ([API: sco/host.h](api.md#scohosth-the-hosts-sco_api-table)).

## Built-in services

Services a product's built-in plugins publish, under the built-in's id. Their headers ship with the SDK; the product, not sco-core, implements them, so on another host `query_service` answers `SCO_NOT_FOUND`.

### `teleport.spatial` 1.0 (sc-offline)

[`sc_spatial.h`](../include/sc_spatial.h), pinned by [`tests/abi_spatial.c`](../tests/abi_spatial.c): where you are, and positions converted between the game's zones. sc-offline's `teleport` built-in publishes it at load on every game build; whether it can answer there is the capability `"teleport"` (`has("teleport")`, the same gate as `teleport.save` / `teleport.go`). Without it every function returns 0. So check both:

```c
const sc_spatial_v1* sp = NULL;
if (api->size > offsetof(sco_api, query_service) &&
    api->query_service(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION, (const void**)&sp) == SCO_OK &&
    api->has("teleport")) { ... }
```

Every function returns 1 when it answered and 0 when it can't (wrong thread, not spawned, an id that isn't streamed in, a null pointer, teleport unavailable); outputs are written only on 1. Game thread only. Positions are metres as doubles; zone id 0 is the world frame.

| Function | What |
|---|---|
| `player_pose(pos[3], rot_xyzw[4], &zone_id)` | Your position and orientation (unit quaternion, x y z w) in the zone you're in, and that zone's id |
| `zone_of_entity(entity_id, &zone_id)` | The zone an entity is in |
| `local_to_world(zone_id, local[3], world[3])` | A position in a zone to the world frame |
| `world_to_local(zone_id, world[3], local[3])` | A world position to a zone's frame |
| `zone_to_zone(from, to, in[3], out[3])` | A position in one zone to another, through their common ancestor (more precise than going through the world) |
| `zone_name(zone_id, out, cap)` | The zone's name (`"OOC_Stanton_2b_Daymar"`), NUL-terminated and cut to fit `cap` |

Zone ids are volatile streaming handles: to save or send a place, keep the zone name and the double coordinates, never the id ([C++ SDK § Services](sdk-cpp.md#services)).

### `spawn.entities` 1.2 (sc-offline)

[`sc_spawn.h`](../include/sc_spawn.h), pinned by [`tests/abi_spawn.c`](../tests/abi_spawn.c): spawn entities near you, look up your entity and ship ids, and move the entities you spawned. sc-offline's `spawn` built-in publishes it at load; whether it can answer is the capability `"spawn.ship"`. Frames, units and ids are `teleport.spatial`'s.

```c
const sc_spawn_service_v1* sp = NULL;
if (api->size > offsetof(sco_api, query_service) &&
    api->query_service(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION, (const void**)&sp) == SCO_OK &&
    api->has("spawn.ship")) { ... }
```

To also run on an older sc-offline, ask for `0x00010000` and check `sp->size > offsetof(sc_spawn_service_v1, <function>)` before calling a function a later minor added (a 1.0 table is 40 bytes, 1.1 is 48, 1.2 is 64). Game thread only.

| Function | Since | What |
|---|---|---|
| `spawn_near_player(class, offset[3], &id)` | 1.0 | Spawns an entity class at `offset` metres from you in your zone's frame; `NULL` and the id, or the reason. The entity belongs to no plugin |
| `class_exists(class)` | 1.0 | 1 if the class is spawnable on this game build |
| `local_player_id()`, `player_ship_id()` | 1.0 | Your entity id, the ship you're aboard; 0 when there is none |
| `entity_alive(id)` | 1.1 | 1 while the id resolves in the game |
| `set_entity_transform(self, id, zone_id, pos[3], rot[4])` | 1.2 | Moves and turns an entity to `pos` / `rot` (unit quaternion, x y z w) in zone `zone_id`'s frame, 0 = the world; the entity stays in its zone. 1 on success, 0 on failure |
| `spawn_as(self, class, offset[3], &id)` | 1.2 | `spawn_near_player`, recorded as the calling plugin's (`self` as `sco_plugin_load` received it) |

`set_entity_transform` moves only an entity spawned through `spawn_as` with the same `self` while that plugin is loaded (unloading forgets them; `spawn_near_player` and the `spawn.ship` command count for nobody), or the player's own vehicle once sc-offline has registered it as retrieved or delivered by ATC (no build does yet). Anything else answers 0. A spawn's id is final at once, but the entity streams in seconds later (up to a minute for a big ship), and until then `set_entity_transform` answers 0: check `entity_alive(id)` first, and try again on a later tick. sc-offline logs why a call answered 0 to `mod.log`, once per id and reason.

## Raw handlers (1.1)

A raw handler is a call that takes and returns bytes: for data that doesn't fit a command's typed arguments and 256-byte reply (a list of entities, a transform, a buffer). The bytes' layout is the handler's contract, as a service table's is; put a size or version field first.

```c
static sco_result get_pose(const void* in, uint32_t in_size, void* out, uint32_t* out_size, void* ctx) {
    if (*out_size < sizeof(my_pose)) { *out_size = sizeof(my_pose); return SCO_TOO_MANY; }
    fill_pose((my_pose*)out);
    *out_size = sizeof(my_pose);
    return SCO_OK;
}
api->register_raw(self, "nav.pose", "teleport", get_pose, NULL);

my_pose pose; uint32_t size = sizeof(pose);   /* in another plugin, on the game thread */
if (api->invoke_raw(self, "nav.pose", NULL, 0, &pose, &size) == SCO_OK) { ... }
```

- **Names** follow the command rule (`<plugin id>.<name>`) in their own namespace; a raw handler and a command may share a name.
- **Output:** `*inout_out_size` is the buffer's capacity on the way in and the bytes written on the way out. A handler that needs more sets it to the size it needs and answers `SCO_TOO_MANY`; pass `out = NULL` with `*inout_out_size = 0` to ask the size first. Pass `inout_out_size = NULL` when you want no output.
- **Calls** run now, on the game thread only (`SCO_WRONG_THREAD` elsewhere), after the capability check (`SCO_UNAVAILABLE`), under the handler's crash guard: a fault marks the handler's plugin crashed and the call answers `SCO_CRASHED` with no output.
- **Lifetime:** a plugin's handlers go when it unloads or crashes. The host keeps fn and ctx, never the bytes.

## Threading

- `tick` callbacks, `run_on_game_thread` tasks and commands run on the game's main thread, from sc-offline's `WH_GETMESSAGE` hook.
- `run_on_game_thread`, `subscribe`, `unsubscribe`, `status`, `log`, `register_command`, `invoke` and `list_commands` may be called from any thread.
- When a plugin unloads, fails to load or is disabled, the host removes everything it registered: subscriptions, commands, queued tasks and queued `invoke` calls. From then on every `sco_api` call with its `self` returns `SCO_BAD_ARG`, so a plugin thread still running can't add anything back.
- A plugin that faults inside a callback is disabled: everything it registered is removed, one `[plugin] hello crashed in tick (0xC0000005) and was disabled` line is logged, and the game keeps running. This limits damage; it is not a sandbox, and a fault that corrupts the stack may not be caught.

## Compatibility

Within major version 1:

- New functions go at the end of `sco_api`, new fields at the end of their struct, and `SCO_API_MINOR` goes up by one.
- Nothing is removed, renamed, retyped or reordered. Enum values keep their numbers; new values are added after the last.
- A plugin checks `api->size` before calling a function added after the minor it was built for:

  ```c
  if (api->size > offsetof(sco_api, some_1_1_function) && api->some_1_1_function) { ... }
  ```

- The host reads only the fields the plugin's `size` covers in `sco_plugin_info` and `sco_command`, and reads `sco_arg_def` arrays with the plugin's `arg_def_size`.

[`tests/abi_v1.c`](../tests/abi_v1.c) enforces this. It pins, with static asserts, every struct's size and field offsets, every enum value and size, both version numbers, and the parameter types of every function and callback. `tools/test.sh` compiles it with `-Werror` as C11 and C++20 for the host, again with `-fshort-enums`, and for `x86_64-pc-windows-msvc`, so CI fails on any change. Since `sdk-v1.1.0` the file is append-only: an addition gets new lines, and an existing line never changes.

## Layout

On x64 (all sizes in bytes). `tests/abi_v1.c` is the source of truth.

| Type | Size | Fields (offset) |
|---|---|---|
| `sco_result`, `sco_log_level`, `sco_arg_type` | 4 | |
| `sco_arg` | 16 | `type` 0, `_pad` 4, `v` 8 |
| `sco_arg_def` | 24 | `name` 0, `type` 8, `_pad` 12, `help` 16 |
| `sco_command` | 72 | `size` 0, `_pad0` 4, `name` 8, `title` 16, `help` 24, `capability` 32, `args` 40, `nargs` 48, `arg_def_size` 52, `fn` 56, `ctx` 64 |
| `sco_api` | 128 | `size` 0, `major` 4, `minor` 6, `host_version` 8, `has` 16, `run_on_game_thread` 24, `subscribe` 32, `unsubscribe` 40, `status` 48, `log` 56, `register_command` 64, `invoke` 72, `list_commands` 80, `provide_service` 88, `query_service` 96, `release_service` 104, `invoke_raw` 112, `register_raw` 120 (1.0 hosts: 88) |
| `sco_plugin_info` | 32 | `size` 0, `api_major` 4, `api_minor` 6, `name` 8, `version` 16, `author` 24 |
