# cs_hello: a native plugin in C#

The C# version of [`hello`](../hello/hello.c) and [`cpp_hello`](../cpp_hello/cpp_hello.cpp), on the SDK's C# layer ([`csharp/Sco.Sdk`](../../csharp/Sco.Sdk)), published with NativeAOT into one native DLL. No .NET runtime is needed on the player's machine.

- `Info` and `Status` on load, `Has("teleport")` on `game.ready`,
- `game.ready` and `tick` subscriptions,
- two commands: `cs_hello.wave <name>` (replies `Hello, <name>`) and `cs_hello.ticks`,
- a service, `cs_hello.greeter` 1.0, published on load and looked up on `game.ready` as another plugin would.

## Build and check

You need the .NET 8 SDK and, on Windows, Visual Studio 2019 or newer with "Desktop development with C++" (NativeAOT links with MSVC). Build `sco-plugin-check` first (the SDK's [README](../../README.md)).

```powershell
dotnet publish -c Release -r win-x64 -o out\cs_hello
copy plugin.ini out\cs_hello\
sco-plugin-check out\cs_hello --invoke cs_hello.wave "Pilot One"
```

```
  query   cs_hello 1.0.0 by sco SDK example (api 1.1)
  service cs_hello.greeter 1.0
  load    ok: 2 command(s)
  invoke  cs_hello.wave -> ok "Hello, Pilot One"
  invoke  cs_hello.ticks -> ok "3 ticks"
OK: 0 failure(s)
```

`sco-plugin-check` can't look services up, so it logs `greeter service: not found`; in the game (and `sco-host-sim`) the lookup answers `Hello, services`. Install by copying `out\cs_hello` (`cs_hello.dll` and `plugin.ini`; leave the `.pdb` files) into sc-offline's `data\plugins\`.

On Linux, `dotnet publish -r linux-x64` builds `cs_hello.so` (needs `clang` and `zlib1g-dev`); rename it to `cs_hello.dll` for `sco-plugin-check`. The game needs the Windows build.

The [C# SDK reference](../../../docs/sdk-csharp.md) covers lifetimes, threads, the exception boundary and the NativeAOT rules.
