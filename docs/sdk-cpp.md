# C++ SDK (`include/scosdk/`)

The C++20 layer of the sco plugin SDK: header-only, over [`sco_api.h`](api-v1.md), with nothing beyond the C++ standard library. It writes the three exports for you, keeps your handlers' strings and callables alive, and makes sure no C++ exception crosses the C ABI. A plugin built with it is an ordinary `kind = native` plugin; the host can't tell it from a C one.

`sco_api.h` stays the contract. Everything here is a wrapper, so the [API v1 reference](api-v1.md) rules (threads, the reply buffer, `done` callbacks, services, raw handlers) still apply; this page covers what the C++ layer adds.

## Contents

- [Headers](#headers)
- [A plugin](#a-plugin)
- [Plugin](#plugin)
- [Commands](#commands)
- [Services](#services)
- [Raw handlers](#raw-handlers)
- [Storage](#storage)
- [Lifetimes](#lifetimes)
- [The exception boundary](#the-exception-boundary)
- [Threads](#threads)
- [Building](#building)

## Headers

| Header | What |
|---|---|
| [`scosdk/plugin.hpp`](../include/scosdk/plugin.hpp) | `sco::sdk::Plugin`, `Subscription`, `MakeArg`, `LoadPlugin` / `UnloadPlugin`, `SCO_PLUGIN` |
| [`scosdk/command.hpp`](../include/scosdk/command.hpp) | `CommandBuilder`, `Args`, `Reply` |
| [`scosdk/service.hpp`](../include/scosdk/service.hpp) | `Provide`, `Release`, `ServiceRef<T>`, `HasMember`, `ServiceVersion` |
| [`scosdk/raw.hpp`](../include/scosdk/raw.hpp) | `RegisterRaw<In, Out>`, `InvokeRaw`, `RegisterRawBytes`, `InvokeRawBytes` |
| [`scosdk/storage.hpp`](../include/scosdk/storage.hpp) | `Storage`, `StorageCursor`, `StorageTransaction`, `SqlInt` / `SqlFloat` / `SqlText` / `SqlBlob` / `SqlNull` (the host service `sco.storage`, [`sco_storage.h`](../include/sco_storage.h)); not in `scosdk.hpp`, include it when you use storage |
| [`scosdk/scosdk.hpp`](../include/scosdk/scosdk.hpp) | All of the above |

Put the SDK's `include/` folder on the include path; the headers find `sco_api.h` there.

## A plugin

```cpp
#include "scosdk/scosdk.hpp"

class Hello : public sco::sdk::Plugin {
public:
    sco_result OnLoad() override {
        using namespace sco::sdk;
        sco_result r = CommandBuilder(*this, "hello.wave")
                           .Title("Wave")
                           .Arg<const char*>("name", "Who to wave at")
                           .Handle([](const Args& args, Reply& reply) {
                               reply.Printf("Hello, %s", args.String(0));
                               return SCO_OK;
                           })
                           .Register();
        if (r != SCO_OK) return r;
        tick_ = Subscribe("tick", [this](const void*) { ++ticks_; });
        Info("loaded on %s", Api()->host_version());
        return tick_ ? SCO_OK : tick_.Result();
    }

private:
    sco::sdk::Subscription tick_;
    unsigned ticks_ = 0;
};

SCO_PLUGIN(Hello, "hello", "1.0.0", "you");
```

[`sdk/examples/cpp_hello`](../sdk/examples/cpp_hello/cpp_hello.cpp) is the complete example: two commands, a subscription to `game.ready` and `tick`, and a service it publishes and looks up.

`SCO_PLUGIN(Class, "id", "version", "author")` emits `sco_plugin_query` (with this header's `SCO_API_MAJOR` / `SCO_API_MINOR`), `sco_plugin_load` (constructs `Class`, runs `OnLoad`) and `sco_plugin_unload` (runs `OnUnload`, releases the SDK's state, destroys the object). Use it once per DLL, at namespace scope; `id` must equal `plugin.ini`'s `id`. To run several `Plugin` objects in one binary (tests do), call `sco::sdk::LoadPlugin(plugin, api, self)` and `sco::sdk::UnloadPlugin(plugin)` yourself.

## Plugin

| Member | Does |
|---|---|
| `virtual sco_result OnLoad()` | Called from `sco_plugin_load`, on the game thread. Anything but `SCO_OK` unloads the plugin; `OnUnload` is not called, but the SDK still releases what was registered |
| `virtual void OnUnload()` | Called from `sco_plugin_unload`, on the game thread, before the SDK releases its state. Stop your own threads here |
| `Api()`, `Self()` | The host's table and this plugin's handle; valid from `OnLoad` until `OnUnload` returns |
| `ApiCovers(offsetof(sco_api, f))` | True when the host's table has function `f` (the `api->size` check) |
| `Log(level, fmt, ...)`, `Info`, `Warn`, `Error`, `Status` | printf formats into a 256-byte buffer (longer is cut). gcc and clang check the format; pass other text as `Info("%s", text)` |
| `Has(capability)` | `api->has(capability) != 0` |
| `RunOnGameThread(std::function<void()>)` | Queues the task; see [Lifetimes](#lifetimes) |
| `Subscribe(event, std::function<void(const void* data)>)` | Returns a `Subscription`; see [Lifetimes](#lifetimes) |
| `Invoke(name, {args...}, &reply)` | Runs a command. From the game thread it runs now and fills `reply`; from another thread it is queued, returns `SCO_OK`, and the reply is lost |
| `InvokeAsync(name, {args...}, done)` | Runs a command and calls `done(result, reply)` once on the game thread; any other return than `SCO_OK` means `done` is never called |

`MakeArg(3)`, `MakeArg(2.5)`, `MakeArg("Ada")` and `MakeArg(true)` build the `sco_arg` values for `Invoke`.

## Commands

`CommandBuilder(plugin, "id.name")` collects the command: `.Title`, `.Help`, `.Capability`, `.Arg<T>(name, help)` with `T` one of `int64_t`, `double`, `const char*` or `bool`, and `.Handle(fn)`. `Register()` returns the host's answer. The title defaults to the name.

The handler is `sco_result(const Args&, Reply&)`. `Args` reads by index: `Int(i)`, `Float(i)`, `String(i)`, `Bool(i)` (and `Count()`, `Type(i)`). The host has already checked the count and types against the `Arg<T>` list; a wrong index or type returns the fallback (`0`, `0.0`, `""`, `false`), never throws. `Reply` writes the reply: `Printf(fmt, ...)` or `Set(text)`, cut at the host's buffer (256 bytes with the NUL).

## Services

A service table is a C struct of function pointers that starts with `uint32_t size`; `Provide` and `ServiceRef` refuse to compile for anything else.

```cpp
struct greeter_v1 { uint32_t size; int (*greet)(const char* who, char* out, uint32_t n); };
static const greeter_v1 kGreeter = { sizeof(greeter_v1), Greet };
sco::sdk::Provide(*this, "hello.greeter", sco::sdk::ServiceVersion(1, 0), &kGreeter);

sco::sdk::ServiceRef<greeter_v1> g;                       // in another plugin
if (g.Query(*this, "hello.greeter", sco::sdk::ServiceVersion(1, 0)) == SCO_OK) g->greet(...);
```

- `Query` answers like `query_service` (`SCO_OK`, `SCO_UNAVAILABLE` for another major or an older version, `SCO_NOT_FOUND`) and `SCO_UNAVAILABLE` on a 1.0 host. The ref is empty unless `SCO_OK`; check it (`if (g)`) before `->`.
- `HasMember(ref, &T::field)` is true when the provider's `size` covers `field`: the check before calling a function a later minor added.
- `Release(plugin, name)` withdraws one of your services. The host withdraws the rest when you unload.
- A provider's table goes away when it unloads: query when you need it, and don't keep a `ServiceRef` across ticks.
- Entity ids, never pointers: a service that deals with game objects takes and returns entity and zone ids (opaque `uint64_t` values, as the game's are), resolves them on every call on the game thread, and answers `SCO_NOT_FOUND` or `SCO_UNAVAILABLE` once the entity has streamed out. Never return a pointer into the game: objects stream out and a stored pointer dangles. See [the API rule](api-v1.md#services-11).

## Raw handlers

`RegisterRaw<In, Out>(plugin, name, capability, fn)` registers a handler `sco_result(const In&, Out&)` (`sco_result(Out&)` for `In = void`); `InvokeRaw(plugin, name, in, out)` calls one. `In` and `Out` must be trivially copyable. The SDK does the size handshake from [Raw handlers](api-v1.md#raw-handlers-11):

- Input must be exactly `sizeof(In)` bytes (none for `void`); otherwise `SCO_BAD_ARG` and the handler isn't called.
- A caller whose buffer is smaller than `sizeof(Out)`, or who passes none, gets `SCO_TOO_MANY` and the size needed. `InvokeRaw`'s optional `uint32_t* size` reports it.
- `InvokeRaw` writes `out` only on `SCO_OK`, and answers `SCO_BAD_ARG` if the handler wrote a size other than `sizeof(Out)` (the two sides disagree about the struct).

For variable-size data use `RegisterRawBytes` and `InvokeRawBytes` with `std::span<const std::byte>` in and `std::span<std::byte>` out; the handler sets `written` to the bytes written, or to the bytes needed with `SCO_TOO_MANY`. Raw calls run on the game thread only (`SCO_WRONG_THREAD` elsewhere).

## Storage

`sco::sdk::Storage` wraps the host service [`sco.storage`](storage.md): per-plugin key-value and SQL. `Open(*this)` queries it once; a host service outlives every plugin, so keep the `Storage` for the plugin's life.

```cpp
#include "scosdk/storage.hpp"

sco::sdk::Storage store;
if (store.Open(*this) == SCO_OK) {
    store.Put("spots.home", spot);                       // trivially copyable: stored as its bytes
    Spot back{};
    store.Get("spots.home", back);                       // SCO_BAD_ARG if the stored size differs
    std::string name;
    store.Get("player.name", name);                      // any size; the handshake is done here
    sco::sdk::StorageTransaction tx(store);              // rolled back unless committed
    store.Exec("INSERT INTO visits VALUES (?, ?)", { sco::sdk::SqlText("Lorville"), sco::sdk::SqlInt(1) });
    tx.Commit();
    for (auto c = store.Query("SELECT place FROM visits"); c.Next() == SCO_OK;) { std::string p; c.Text(0, p); }
}
```

- Every call is `noexcept` and answers `sco_result` (out of memory is `SCO_TOO_MANY`); `LastError()` has the message of the last failure.
- `StorageCursor` is move-only and closes its cursor on destruction; `Int`, `Float`, `Text`, `Blob` and `Value` read the current row. `Keys(prefix, out)` lists keys in byte order.
- The transaction is the plugin's, not the thread's ([Storage § Threads](storage.md#threads)).

## Lifetimes

The SDK owns every callable you hand it (command and raw handlers, event handlers, tasks, `done` callbacks) and the strings a command keeps. The `ctx` it gives the host for each is an id, not a pointer: the callable sits in a registry, and the trampoline looks the id up on every call. When the plugin unloads, `UnloadPlugin` removes every entry, so a call the host makes later (or never makes) finds nothing instead of freed memory, and nothing leaks.

| What | Lives until |
|---|---|
| A command or raw handler | The plugin unloads |
| A task (`RunOnGameThread`) | It runs, then it is freed. A task still queued at unload is dropped and never runs: don't rely on one to save state |
| A `done` callback (`InvokeAsync`) | It runs. A call still queued at unload is dropped and `done` is not called |
| An event handler | Its `Subscription` is destroyed or `Reset`, or the plugin unloads |
| A service table | Yours: keep it valid (a `static`) until `Release` or unload |

`Subscription` is a move-only handle. The SDK keeps one host subscription per event and multiplexes your handlers over it (the host keys a subscription by plugin, event and function, so it can't hold two for one event). Destroying the handle or calling `Reset()` removes the handler; when the last handler for an event goes, the host subscription goes too.

- From the game thread (a tick, a command, a task, or the handler itself), the handler never runs again once `Reset` returns.
- From another thread a call already running on the game thread may still finish. It keeps its own reference to the handler, so the SDK frees nothing under it, but anything your handler captured by reference must outlive that call: this is the [Freeing ctx](api-v1.md#freeing-ctx) rule.
- A handle still alive when the plugin unloads is detached by the unload and does nothing afterwards, from any thread. A `Subscription` member of your plugin class is the normal way to hold one.

## The exception boundary

Every function in `scosdk/` is `noexcept`. An exception that escapes your code is caught where the host called in and logged as `[<id>] error: exception in <where>: <what>` (`unknown exception` for a non-`std::exception`):

| Escapes from | Becomes |
|---|---|
| The plugin's constructor or `OnLoad` | `sco_plugin_load` returns `SCO_FAILED`; the SDK releases what `OnLoad` registered and the host unloads the plugin |
| A command handler | `SCO_FAILED`, with `<name> failed: <what>` as the reply |
| A raw handler | `SCO_FAILED`, no output |
| An event handler, a task, a `done` callback, `OnUnload` | Logged; the dispatch goes on with the next handler |

An allocation failure inside the SDK answers `SCO_TOO_MANY`. A crash (an access violation) is not an exception: the host's crash guard handles it as for any native plugin.

## Threads

The [threading rules](api-v1.md#threading) are the host's. `RunOnGameThread`, `Subscribe`, `Subscription::Reset`, `Invoke`, `InvokeAsync`, `CommandBuilder::Register`, `Provide`, `Release`, `ServiceRef::Query` and `RegisterRaw` work from any thread; handlers, tasks and `done` callbacks run on the game thread; `InvokeRaw` is game thread only. `OnLoad` and `OnUnload` run on the game thread, and once `OnUnload` has returned no thread may call into the plugin object.

## Building

`sco_add_plugin` in `cmake/sco-plugin.cmake` builds `.cpp` sources as C++20 with hidden visibility (only the three exports are visible), the static C runtime, and `/W4 /WX` on MSVC or `-Wall -Wextra -Wpedantic -Werror` elsewhere. Enable C++ in your project:

```cmake
project(my_plugin CXX)
include("${SCO_SDK}/cmake/sco-plugin.cmake")
sco_add_plugin(my_plugin SOURCES plugin.cpp INI plugin.ini)
```

Visual Studio 2019 16.11 or newer (`/std:c++20`), clang 14 or gcc 11 or newer. `sco-plugin-check` checks a C++ plugin like a C one. sco-core's own tests: [`tests/test_sdk.cpp`](../tests/test_sdk.cpp) loads two SDK plugins through the real host table under ASan+UBSan and ThreadSanitizer, and the SDK zip's CI builds and checks `cpp_hello` with MSVC on Windows and with gcc on Linux.
