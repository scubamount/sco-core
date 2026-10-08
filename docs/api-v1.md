# Plugin API v1 reference (`sco_api.h`)

Version 1.0-pre. This is the plain-C interface a plugin uses to talk to its host (sc-offline). It is the only header a plugin includes; the C++ headers in `include/sco/` are internal and are not part of it.

**Status: pre-release.** The header, this page and [`tests/abi_v1.c`](../tests/abi_v1.c) can still change until the `sdk-v1.0.0` tag. No host implements the table yet: sc-offline builds it in step 4 of the SDK plan and loads plugins from step 6. From the tag on, version 1 only grows (see [Compatibility](#compatibility)).

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
- [Threading](#threading)
- [Compatibility](#compatibility)
- [Layout](#layout)

## Rules

- Plain C with `extern "C"`. No C++ types, no exceptions and no varargs cross the boundary; each side formats its own strings.
- Every struct one side hands the other by pointer starts with `uint32_t size`, set to `sizeof` of the struct as that side was built. The host reads only the fields the plugin's `size` covers; a plugin calls only the functions the host's `size` covers.
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
    SCO_CRASHED = 4, SCO_WRONG_THREAD = 5, SCO_TOO_MANY = 6
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
| `SCO_TOO_MANY` | A queue or table is full, or the name is already registered |

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
} sco_api;

typedef void (*sco_task_fn)(void* ctx);
typedef void (*sco_event_fn)(const char* event, const void* data, void* ctx);
```

| Member | Thread | Does |
|---|---|---|
| `size` | | `sizeof(sco_api)` as the host was built. Check it before calling a function added after 1.0 |
| `major`, `minor` | | The host's API version |
| `host_version()` | Any | The host's name and version, `"sc-offline 0.8.0"`. Static string |
| `has(capability)` | Any | 1 if the capability is available on this game build, else 0 (unknown names too). See [Capabilities](#capabilities) |
| `run_on_game_thread(self, fn, ctx)` | Any | Queues `fn(ctx)` to run on the game thread at the next tick, in the order queued. The queue holds 256 tasks; beyond that, `SCO_TOO_MANY` |
| `subscribe(self, event, fn, ctx)` | Any | Calls `fn(event, data, ctx)` for each dispatch of `event`. Applies from the next dispatch |
| `unsubscribe(self, event, fn)` | Any | Removes the subscription for that `event` and `fn`; `SCO_NOT_FOUND` if there is none. Applies from the next dispatch |
| `status(self, message)` | Any | Shows `hello: message` on the status line |
| `log(self, level, message)` | Any | Writes `[hello] message` to `mod.log` |
| `register_command(self, cmd)` | Any | Adds a command; see [Commands](#commands) |
| `invoke(self, name, args, nargs, done, ctx)` | Any | Runs a command; see [Commands](#commands) |
| `list_commands(out, max)` | Any | Writes up to `max` command pointers to `out` and returns the total number registered. Call with `max = 0` to get the count. The pointers stay valid until their owner unloads |

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
    const sco_arg_def* args; uint32_t nargs; uint32_t _pad1;
    sco_command_fn fn; void* ctx;
} sco_command;
```

`sco_arg.type` and `sco_arg_def.type` hold a `sco_arg_type`. Read the union member that matches: `i` for `SCO_ARG_INT` and `SCO_ARG_BOOL` (0 or 1), `f` for `SCO_ARG_FLOAT`, `s` for `SCO_ARG_STRING`. Set `_pad` fields to 0.

| `sco_command` field | Set to |
|---|---|
| `size` | `sizeof(sco_command)` |
| `name` | `<owner>.<action>`: `"spawn.ship"`, `"teleport.save"`, `"hello.wave"`. A plugin may only register names that start with its own `name` and a dot |
| `title` | The menu label, `"Spawn ship"` |
| `help` | One line of help, or NULL |
| `capability` | The `has()` name this command needs, or NULL for always available |
| `args`, `nargs` | The argument definitions, in order |
| `fn`, `ctx` | Called as `fn(args, nargs, ctx, reply, reply_size)` |

Behavior:

- `register_command` copies `name`, `title`, `help` and `capability`. `args`, `fn` and `ctx` must stay valid until the plugin unloads. A duplicate name returns `SCO_TOO_MANY`; a name outside the plugin's prefix returns `SCO_BAD_ARG`.
- Commands run on the game thread. `invoke` from the game thread runs the command at once and calls `done` before returning. From any other thread it queues the command, and `done` runs on the game thread later.
- Before calling `fn` the host checks the argument count and types (`SCO_BAD_ARG`) and the capability (`SCO_UNAVAILABLE`); `done` receives that result and `fn` isn't called.
- `fn` writes a short human message into `reply`, at most `reply_size` bytes including the NUL: `"Spawned Cutlass Black"`. The menu shows it on the status line. `done` gets it as `reply`, valid only during the call.
- `done` may be NULL when the caller doesn't need the result.

Not settled for 1.0: how the menu reads lists it shows today (ship classes, bookmarks, outfits, contract offers). It is decided at step 5 with the real menu; the candidates are commands that return a list in `reply`, or a `query` function added in 1.1.

## Events

| Event | When | `data` |
|---|---|---|
| `game.ready` | After the offline patches and the signature report, before the first tick | `NULL` |
| `tick` | Every main-thread tick (about 10 per second), on the game thread | `const uint32_t*`: milliseconds now |
| `game.exit` | Before the DLL unloads | `NULL` |

`data` is valid only during the callback.

## Capabilities

`has()` answers from sco-core's signature registry and sc-offline's feature readiness: one name per feature that logs `[+] ... ready` today, such as `"teleport"`, `"spawn.ship"`, `"console"`, `"outfits"` and `"contracts"`. Unknown names return 0. In v1 plugins reach features only through commands; `has()` lets a plugin grey out its own UI and say why.

## Threading

- `tick` callbacks, `run_on_game_thread` tasks and commands run on the game's main thread, from sc-offline's `WH_GETMESSAGE` hook.
- `run_on_game_thread`, `subscribe`, `unsubscribe`, `status`, `log`, `register_command`, `invoke` and `list_commands` may be called from any thread.
- A plugin that faults inside a callback is disabled: its subscriptions are dropped, one `[plugin] hello crashed in tick (0xC0000005) and was disabled` line is logged, and the game keeps running. This limits damage; it is not a sandbox, and a fault that corrupts the stack may not be caught.

## Compatibility

Within major version 1:

- New functions go at the end of `sco_api`, new fields at the end of their struct, and `SCO_API_MINOR` goes up by one.
- Nothing is removed, renamed, retyped or reordered. Enum values keep their numbers; new values are added after the last.
- A plugin checks `api->size` before calling a function added after the minor it was built for:

  ```c
  if (api->size > offsetof(sco_api, some_1_1_function) && api->some_1_1_function) { ... }
  ```

- The host reads only the fields the plugin's `size` covers in `sco_plugin_info` and `sco_command`.

[`tests/abi_v1.c`](../tests/abi_v1.c) enforces this. It pins, with static asserts, every struct's size and field offsets, every enum value, both version numbers, and the parameter types of every function and callback. `tools/test.sh` compiles it as C11 and as C++20 with `-Werror`, so CI fails on any change. After `sdk-v1.0.0` the file is append-only: an addition gets new lines, and an existing line never changes.

## Layout

On x64 (all sizes in bytes). `tests/abi_v1.c` is the source of truth.

| Type | Size | Fields (offset) |
|---|---|---|
| `sco_result`, `sco_log_level`, `sco_arg_type` | 4 | |
| `sco_arg` | 16 | `type` 0, `_pad` 4, `v` 8 |
| `sco_arg_def` | 24 | `name` 0, `type` 8, `_pad` 12, `help` 16 |
| `sco_command` | 72 | `size` 0, `_pad0` 4, `name` 8, `title` 16, `help` 24, `capability` 32, `args` 40, `nargs` 48, `_pad1` 52, `fn` 56, `ctx` 64 |
| `sco_api` | 88 | `size` 0, `major` 4, `minor` 6, `host_version` 8, `has` 16, `run_on_game_thread` 24, `subscribe` 32, `unsubscribe` 40, `status` 48, `log` 56, `register_command` 64, `invoke` 72, `list_commands` 80 |
| `sco_plugin_info` | 32 | `size` 0, `api_major` 4, `api_minor` 6, `name` 8, `version` 16, `author` 24 |
