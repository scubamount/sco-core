# C# SDK (`sdk/csharp/Sco.Sdk`)

The C# layer of the sco plugin SDK: a .NET 8 library over [`sco_api.h`](api-v1.md) and the host-owned service headers, with no package references. A plugin references it and is published with **NativeAOT** into one self-contained native DLL with the three C exports; the player needs no .NET runtime, and the host can't tell it from a C plugin (`kind = native`).

`sco_api.h` stays the contract. Everything here is a wrapper, so the [API v1 reference](api-v1.md) rules (threads, the reply buffer, `done` callbacks, services, raw handlers) still apply; this page covers what the C# layer adds. It mirrors the [C++ SDK](sdk-cpp.md): same registry, same lifetimes, same exception boundary.

## Contents

- [Setup](#setup)
- [A plugin](#a-plugin)
- [Plugin](#plugin)
- [Commands](#commands)
- [Services](#services)
- [Raw handlers](#raw-handlers)
- [Storage, DataCore and UI](#storage-datacore-and-ui)
- [Lifetimes](#lifetimes)
- [The exception boundary](#the-exception-boundary)
- [Threads](#threads)
- [NativeAOT and trimming rules](#nativeaot-and-trimming-rules)
- [The layout test](#the-layout-test)

## Setup

- The [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0) (a newer SDK builds `net8.0` too).
- On Windows, Visual Studio 2019 or newer (or the Build Tools) with "Desktop development with C++": NativeAOT links with MSVC's `link.exe`. On Linux, `clang` and `zlib1g-dev` (for a build `sco-plugin-check` can load; the game needs the Windows DLL).
- The SDK's `csharp/` folder next to your project, or anywhere: reference `csharp/Sco.Sdk/Sco.Sdk.csproj`.

```powershell
cd examples\cs_hello
dotnet publish -c Release -r win-x64 -o out\cs_hello
copy plugin.ini out\cs_hello\
..\..\out\bin\sco-plugin-check out\cs_hello --invoke cs_hello.wave "Pilot One"
```

`out\cs_hello\cs_hello.dll` is the plugin (the `.pdb` files are symbols; don't ship them). Copy `out\cs_hello` into sc-offline's `data\plugins\`.

A project file needs these properties (see [`examples/cs_hello/cs_hello.csproj`](../sdk/examples/cs_hello/cs_hello.csproj)):

```xml
<TargetFramework>net8.0</TargetFramework>
<AssemblyName>my_plugin</AssemblyName>      <!-- the DLL's name: plugin.ini's entry -->
<AllowUnsafeBlocks>true</AllowUnsafeBlocks>
<PublishAot>true</PublishAot>
<NativeLib>Shared</NativeLib>
<InvariantGlobalization>true</InvariantGlobalization>
```

## A plugin

```csharp
using System.Runtime.InteropServices;
using Sco.Sdk;
using Sco.Sdk.Interop;

public sealed class MyPlugin : Plugin
{
    private Subscription? _tick;
    private uint _ticks;

    public override ScoResult OnLoad()
    {
        ScoResult r = new CommandBuilder(this, "my_plugin.wave")
            .Title("Wave")
            .Arg("name", ScoArgType.String, "Who to wave at")
            .Handle((args, reply) => { reply.Set("Hello, " + args.String(0)); return ScoResult.Ok; })
            .Register();
        if (r != ScoResult.Ok) return r;
        _tick = Subscribe("tick", () => ++_ticks);
        Info("loaded on " + HostVersion);
        return _tick.Result;
    }
}

public static unsafe class Exports
{
    [UnmanagedCallersOnly(EntryPoint = "sco_plugin_query")]
    public static ScoPluginInfo* Query() => PluginExports.Query("my_plugin", "1.0.0", "you");

    [UnmanagedCallersOnly(EntryPoint = "sco_plugin_load")]
    public static ScoResult Load(ScoApi* api, void* self) => PluginExports.Load<MyPlugin>(api, self, "my_plugin");

    [UnmanagedCallersOnly(EntryPoint = "sco_plugin_unload")]
    public static void Unload() => PluginExports.Unload();
}
```

NativeAOT exports only the `[UnmanagedCallersOnly(EntryPoint = ...)]` methods of the assembly being published, so the three exports live in your project and forward to `PluginExports`. `Query` builds `sco_plugin_info` once with this SDK's `ApiMajor` / `ApiMinor` and keeps it for the DLL's life; `Load<T>` constructs `T` and runs `OnLoad`; `Unload` runs `OnUnload` and releases the SDK's state. The id must equal `plugin.ini`'s `id`. To run several `Plugin` objects in one binary (tests do), call `PluginExports.LoadPlugin(plugin, api, self, id)` and `PluginExports.UnloadPlugin(plugin)`.

[`sdk/examples/cs_hello`](../sdk/examples/cs_hello/CsHello.cs) is the complete example: two commands, `game.ready` and `tick` subscriptions, and a service it publishes and looks up.

## Plugin

| Member | Does |
|---|---|
| `virtual ScoResult OnLoad()` | Called from `sco_plugin_load`, on the game thread. Anything but `Ok` unloads the plugin; `OnUnload` is not called, but the SDK still releases what was registered |
| `virtual void OnUnload()` | Called from `sco_plugin_unload`, on the game thread, before the SDK releases its state. Stop your own threads here |
| `Id`, `Api`, `Self`, `IsLoaded` | The plugin id, the host's table (`ScoApi*`) and this plugin's handle; valid from `OnLoad` until `OnUnload` returns |
| `ApiCovers(ApiOffset.query_service)` | True when the host's table has that function (the `api->size` check) |
| `Log(level, text)`, `Info`, `Warn`, `Error`, `Status` | Any thread. Strings are passed as UTF-8 |
| `Has(capability)`, `HostVersion` | `api->has(capability) != 0`; the host's name and version |
| `RunOnGameThread(Action)` | Queues the task; see [Lifetimes](#lifetimes) |
| `Subscribe(event, Action<nint> handler)` / `Subscribe(event, Action)` | Returns a `Subscription`; `data` is the event's pointer, valid only during the call (`TickMs(data)` reads a tick's milliseconds) |
| `Invoke(name, out reply, args...)` | Runs a command. From the game thread it runs now and fills `reply`; from another thread it is queued, returns `Ok`, and `reply` is `""` |
| `InvokeAsync(name, done, args...)` | Runs a command and calls `done(result, reply)` once on the game thread; any other return than `Ok` means `done` is never called |

`Arg.Int(3)`, `Arg.Float(2.5)`, `Arg.Text("Ada")` and `Arg.Bool(true)` (or plain `3`, `2.5`, `"Ada"`, `true`: they convert) build the `sco_arg` values for `Invoke` and `Ui.BindHotkey`.

## Commands

`new CommandBuilder(plugin, "id.name")` collects the command: `.Title`, `.Help`, `.Capability`, `.Arg(name, ScoArgType, help)` and `.Handle(handler)`. `Register()` returns the host's answer. The title defaults to the name.

The handler is `ScoResult (Args args, Reply reply)`. `Args` reads by index: `Int(i)`, `Float(i)`, `String(i)`, `Bool(i)` (and `Count`, `Type(i)`); the host has already checked the count and types, and a wrong index or type returns the fallback (`0`, `0.0`, `""`, `false`), never throws. `Reply.Set(text)` writes the reply, cut at the host's buffer (256 bytes with the NUL) on a whole UTF-8 character. Both are `ref struct`s: valid only during the call, they can't be stored or captured.

## Services

A service table is a blittable struct that starts with `uint size`, allocated once in native memory (`NativeMemory.AllocZeroed`) and kept until `Release` or unload; its functions are `delegate* unmanaged<...>` pointers to `[UnmanagedCallersOnly]` methods.

```csharp
[StructLayout(LayoutKind.Sequential)]
public unsafe struct GreeterV1 { public uint size; public uint _pad; public delegate* unmanaged<byte*, byte*, uint, int> greet; }

Provide("my_plugin.greeter", ServiceVersion(1, 0), table);          // table: GreeterV1*, size set

if (Query("my_plugin.greeter", ServiceVersion(1, 0), out ServiceRef<GreeterV1> g) == ScoResult.Ok && g.Covers(8))
    g.Table->greet(...);                                             // in another plugin
```

- `Query` answers like `query_service` (`Ok`, `Unavailable` for another major or an older version, `NotFound`), `Unavailable` on a 1.0 host, and `BadArg` when the table found doesn't start with a size of at least 4. The `ServiceRef` is empty unless `Ok`.
- `ServiceRef.Covers(offset, fieldSize = 8)` is true when the provider's `size` covers the field: the check before calling a function a later minor added.
- `Release(name)` withdraws one of your services. The host withdraws the rest when you unload.
- A provider's table goes away when it unloads: query when you need it, and don't keep a `ServiceRef` across ticks (host services under `sco.` may be kept).
- Ids, never pointers: a service that deals with game objects takes and returns entity ids and resolves them on every call ([the API rule](api-v1.md#services-11)).
- A function in your table is called from native code: wrap its body in `try`/`catch` (see [The exception boundary](#the-exception-boundary)).

## Raw handlers

`RegisterRaw<TIn, TOut>(name, capability, (in TIn x, out TOut y) => ...)` registers a handler for unmanaged structs (`RegisterRaw<TOut>` takes no input); `InvokeRaw(name, in x, out y)` calls one. The SDK does the size handshake from [Raw handlers](api-v1.md#raw-handlers-11):

- Input must be exactly `sizeof(TIn)` bytes (none for the no-input form); otherwise `BadArg` and the handler isn't called.
- A caller whose buffer is smaller than `sizeof(TOut)`, or who passes none, gets `TooMany` and the size needed.
- `InvokeRaw` writes its output only on `Ok`, and answers `BadArg` if the handler wrote a size other than `sizeof(TOut)`.

For variable-size data use `RegisterRawBytes` (`ScoResult (ReadOnlySpan<byte> input, Span<byte> output, out uint written)`) and `InvokeRawBytes`. Raw calls run on the game thread only (`WrongThread` elsewhere).

## Storage, DataCore and UI

The host-owned services each have a class. `Open(plugin)` queries the service once and checks its table size; a host service outlives every plugin, so keep the object for the plugin's life. Every call answers a `ScoResult`; nothing throws.

| Class | Service | Does |
|---|---|---|
| `Storage`, `StorageCursor` | [`sco.storage`](storage.md) ([`sco_storage.h`](../include/sco_storage.h)) | `Put` / `Get` (bytes, unmanaged structs, `PutString` / `GetString`; any size, the handshake is done here), `Delete`, `Keys(prefix, list)`, `Begin` / `Commit` / `Rollback`, `Exec(sql, out changes, params SqlParam[])`, `Query(sql, out cursor, ...)` with `Next`, `Int`, `Float`, `Text`, `Blob`, `ColumnName`; `LastError()`. Dispose a cursor to close it |
| `Sco.Sdk.Game.Vehicles` | [`game.vehicles`](api-v1.md#gamevehicles-10-game-pack) ([`sc_vehicles.h`](../include/sc_vehicles.h)) | `Open`, `PlayerShip`, `Seats(ship, Span<ScVehicleSeat>, out count, out more)` (each seat's `Name` and `Flags`), `SeatOccupant`, `Seat`, `Eject`, `PowerOn`, `LastError()`. Game thread only |
| `DataCore`, `DataCorePatch` | [`sco.datacore`](datacore.md#the-scodatacore-service) ([`sco_datacore.h`](../include/sco_datacore.h)) | `State`, `Begin(out patch, flags)`; on the patch `Set(record, field, DcValue)`, `AddInstance`, `SetPointer`, `Append`, `AddRecord` (needs `sco.datacore` 1.1, `Abi.DataCoreVersion1_1`; a 1.0 host answers `Unavailable`), `Commit`, `Discard`, `Report(i, out DcReport)` / `Reports()`. A patch disposed without `Commit` is discarded. Results arrive with the event `datacore.applied` (data: `ScoDcApplied*`) |
| `Ui` | [`sco.ui`](ui.md) ([`sco_ui.h`](../include/sco_ui.h)) | `AddTab(id, title, order, draw)`, `RemoveTab`, `SetBadge`, `AddOverlay(id, draw)`, `RemoveOverlay`, `BindHotkey(chord, command, args...)`, `UnbindHotkey`, `NormalizeChord`, `LastError()`. `draw(frame)` gets the product's frame context, on the game thread |

```csharp
var store = new Storage();
if (store.Open(this) == ScoResult.Ok)
{
    store.PutString("player.name", "Ada");
    store.Exec("CREATE TABLE IF NOT EXISTS visits (place TEXT, n INTEGER)");
    store.Exec("INSERT INTO visits VALUES (?, ?)", "Lorville", 1);
    store.Query("SELECT place FROM visits", out StorageCursor c);
    using (c) while (c.Next() == ScoResult.Ok) Info(c.Text(0));
}

var dc = new DataCore();
if (dc.Open(this) == ScoResult.Ok && dc.Begin(out DataCorePatch p) == ScoResult.Ok)
{
    p.Set("EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem",
          "Components[SCItemQuantumDriveParams].params.spoolUpTime", 3.5);
    p.Commit();
}
```

## Lifetimes

The SDK owns every callable you hand it (command and raw handlers, event handlers, tasks, `done` callbacks, draw functions) and the UTF-8 strings a command keeps (in a native arena freed at unload). The `ctx` it gives the host for each is an **id into a registry**, never a `GCHandle` or a pointer: the trampoline looks the id up on every call, and unload removes the plugin's entries, so a call the host makes late (or never makes) finds nothing instead of a freed object. Ids are never reused.

| What | Lives until |
|---|---|
| A command or raw handler | The plugin unloads |
| A task (`RunOnGameThread`) | It runs. A task still queued at unload is dropped and never runs: don't rely on one to save state |
| A `done` callback (`InvokeAsync`) | It runs. A call still queued at unload is dropped and `done` is not called |
| An event handler | Its `Subscription` is disposed, or the plugin unloads |
| A tab or overlay draw function | `RemoveTab` / `RemoveOverlay`, or the plugin unloads |
| A service table | Yours: keep it valid until `Release` or unload |

The SDK keeps one host subscription per event and multiplexes your handlers over it (the host keys a subscription by plugin, event and function). Disposing the `Subscription` removes the handler; when the last handler for an event goes, the host subscription goes too.

- From the game thread, the handler never runs again once `Dispose` returns.
- From another thread a call already running on the game thread may still finish; it holds its own reference, so nothing is freed under it, but what the handler uses must outlive that call (the [Freeing ctx](api-v1.md#freeing-ctx) rule).
- A handle still alive at unload is detached by the unload and does nothing afterwards.

Strings and argument arrays the SDK passes for one call (`Invoke`, `Storage.Exec`, `DataCorePatch.Set`, ...) are native copies freed when the call returns; the host copies what it keeps.

## The exception boundary

No exception crosses into native code. Every trampoline catches, and an exception that escapes your code is logged as `[<id>] error: exception in <where>: <Type>: <message>`:

| Escapes from | Becomes |
|---|---|
| The plugin's constructor or `OnLoad` | `sco_plugin_load` returns `Failed`; the SDK releases what `OnLoad` registered and the host unloads the plugin |
| A command handler | `Failed`, with `<name> failed: <message>` as the reply |
| A raw handler | `Failed`, no output |
| An event handler, a task, a `done` callback, a draw function, `OnUnload` | Logged; the dispatch goes on with the next handler |

Your own `[UnmanagedCallersOnly]` functions (service tables) are outside the SDK: an exception escaping one terminates the process, so catch everything in them. A crash (an access violation in unsafe code) is not an exception: the host's crash guard handles it as for any native plugin.

## Threads

The [threading rules](api-v1.md#threading) are the host's. `RunOnGameThread`, `Subscribe`, `Subscription.Dispose`, `Invoke`, `InvokeAsync`, `CommandBuilder.Register`, `Provide`, `Release`, `Query`, `RegisterRaw`, logging and the storage, DataCore and UI registration calls work from any thread; handlers, tasks, `done` callbacks and draw functions run on the game thread; `InvokeRaw` is game thread only. `OnLoad` and `OnUnload` run on the game thread, and once `OnUnload` has returned no thread of yours may call into the plugin. A thread you start must be stopped in `OnUnload`: the DLL stays mapped, but the SDK has released everything.

## NativeAOT and trimming rules

The plugin is compiled ahead of time and trimmed: there is no JIT and no runtime next to the game.

- `Sco.Sdk` is `IsAotCompatible` and builds with warnings as errors; keep your project warning-free under `PublishAot` too. Trim and AOT warnings (`IL2xxx`, `IL3xxx`) mean the code will fail at run time.
- No reflection over types the trimmer can't see, no `Assembly.Load`, no `System.Reflection.Emit`, no `dynamic`, no runtime-generated serializers: use source-generated `System.Text.Json` if you need JSON.
- No `Marshal.GetFunctionPointerForDelegate` or `DllImport` callbacks to hand the host: use `[UnmanagedCallersOnly]` methods and `delegate* unmanaged` pointers, as the SDK does.
- Structs passed to native code are blittable (`[StructLayout(LayoutKind.Sequential)]`, no `bool`, `char` or references); `bool` crosses as `int`.
- `InvariantGlobalization` is on: the DLL ships without ICU, and culture-sensitive formatting uses the invariant culture.
- One plugin per DLL; the .NET runtime inside it is the plugin's own (each NativeAOT plugin carries one; `cs_hello.dll` is about 1 MB). A NativeAOT library can't be unloaded, so the DLL stays mapped for the game's life, like a C plugin.
- 64-bit only: `win-x64` for the game, `linux-x64` for `sco-plugin-check` on Linux (rename the `.so` to the `entry` name in `plugin.ini`).

## The layout test

`csharp/Sco.Sdk.Tests` checks every struct against the C headers and drives the wrappers through a stand-in host table:

```sh
dotnet run -c Release --project sdk/csharp/Sco.Sdk.Tests -- tests/abi_v1.c tests/abi_storage.c tests/abi_ui.c tests/abi_datacore.c
```

`Pins.cs` holds every `SIZE`, `AT` and `PIN` line of the `tests/abi_*.c` files (sco-core's own pins of `sco_api.h`, `sco_storage.h`, `sco_ui.h` and `sco_datacore.h`); the test checks each one with `Marshal.SizeOf`, `Unsafe.SizeOf` and `Marshal.OffsetOf` and the SDK's constants, and a pin it can't check is a failure. Given the C files, it also fails unless `Pins.cs` equals their lines, so a changed header can't slip past. Then `FakeHost.cs` loads plugins through a table of `[UnmanagedCallersOnly]` stand-ins: commands, a throwing command and `OnLoad`, event multiplexing, tasks, raw handlers and the size handshake, the service size check, and a late call through an old `ctx` after unload. Exit 0 = every check passed. The SDK's CI ([`sdk.yml`](../.github/workflows/sdk.yml)) runs it on Linux and Windows, publishes `cs_hello` with NativeAOT on both, checks it with `sco-plugin-check`, and loads it through the real host kit with `sco-host-sim`.
