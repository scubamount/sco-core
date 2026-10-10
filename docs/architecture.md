# How sco-core works

## The problem it solves

sc-offline works by finding pieces of `StarCitizen.exe` in memory: a function to call, a global pointer to read, a few bytes to patch. Star Citizen is rebuilt every patch, so those addresses move. The mod finds them by searching for byte patterns that survive rebuilds.

Before sco-core, each feature file carried its own patterns and checks, and reported failures in its own words, somewhere in `mod.log`. After a patch there was no single answer to "what broke?", and the only way to find out was to start the game.

sco-core makes every address a named row in a table, resolves all rows in one place, and writes one report. The same tables run against an `.exe` on disk, so a new game build can be checked before anyone plays it.

## Rows

A row is a `sco::SigDef` ([`sco/signatures.h`](../include/sco/signatures.h)):

```cpp
struct SigDef {
    const char* id;              // "teleport.to_camera": <feature>.<thing>
    const char* pattern;         // unique .text pattern, or nullptr when `resolve` is set
    int         ripDisp;         // >0: the result is the RIP target of the operand at match+ripDisp
    int         ripSize;         //      in an instruction of ripSize bytes; 0 = the match itself
    SigResolver resolve;         // function for rows a pattern can't express
    const char* needs[4];        // ids this row reads; nullptr-terminated
};
```

A row finds its address one of two ways:

- **Pattern row.** `pattern` must match exactly once in `.text`. The address is the match, or, with `ripDisp`/`ripSize`, the target of a RIP-relative operand inside the match (for example the global behind `mov rax, [rip+X]`).
- **Resolver row.** `resolve` is a function that gets the image and returns a `SigResult`. Use it when a pattern isn't enough: starting from a string the function references, checking a dozen offsets inside a function, or deriving an address from another row.

`needs` lists rows this row reads. A resolver may only call `sco::Sig()` on ids in its own `needs`.

### Ids

Ids are `<feature>.<thing>` in lowercase with underscores: `teleport.to_camera`, `teleport.entity_system`, `system.quit`. The feature part matches the file under `src/game/` and the header under `include/sco/game/`. Ids are unique across all tables; registering a duplicate fails.

## Results

Each row ends up in one state (`sco::SigState`):

| State | Report word | Means |
|---|---|---|
| `Ok` | `OK` | Found; `Sig(id)` returns the address |
| `Missing` | `MISSING` | Pattern row: 0 matches |
| `Ambiguous` | `AMBIG` | Pattern row: more than one match, so none is trusted |
| `Failed` | `FAILED` | Resolver said no, with a reason (`layout changed at +0x2b0`), or the row itself is broken (unknown need, circular need, no pattern and no resolver) |
| `Blocked` | `BLOCKED` | A row it needs isn't OK; this row wasn't tried |
| `NotRun` | `NOT RUN` | `ResolveAll()` hasn't run since the row was registered |

`Sig(id)` returns an address only for `Ok` rows, and `nullptr` for every other state and for unknown ids. Features never get a half-found address.

## Resolving

`sco::ResolveAll(image)` clears every result, then resolves each row depth-first: a row's needs are resolved before the row itself, whatever order the tables list them in. If a need isn't OK, the row is `Blocked` and its resolver never runs, so resolvers don't have to check their needs for `nullptr`. A need on an unknown id, or a cycle, makes the row `Failed`.

Calling `ResolveAll()` again re-resolves everything from scratch and gives the same answer for the same image.

## The report

`sco::LogSignatureReport(false)` writes:

```
[core] signatures: 4/5 OK
[core] FAILED   teleport.to_camera (layout changed at +0x2b0)
```

The first line always appears. Then one line per row that isn't OK, in registration order. `LogSignatureReport(true)` also lists the OK rows. In the game this goes to `mod.log`; in `sco-sigcheck` it goes to the terminal.

## Startup

In sc-offline (`src/dllmain.cpp`), after the offline patches are applied:

```cpp
sco::SetLogSink(ForwardCoreLog);                         // sco-core lines go to mod.log
sco::game::RegisterGameSignatures();                     // all game tables, once
sco::ResolveAll(sco::ModuleImage());                     // the running StarCitizen.exe
if (ResolveTeleportApi()) { ... }                        // features read their rows
...
sco::LogSignatureReport(false);                          // one block in mod.log
```

Then, on the first main-thread tick, `sco::app::Start` runs the host kit (capabilities, the `sco_api` table, plugins with `plugins = on`, `game.ready`) with `image` left null, since the rows are already resolved; `sco::app::Tick` runs on every tick after. See [C++ API § sco/app.h](api.md#scoapph-the-host-kit).

`sco::ModuleImage()` (Windows only) reads the running executable's PE headers to find `.text` and `.rdata`. In `sco-sigcheck`, `sco::FileImage::Load()` builds the same `Image` from the file on disk by copying each section to its RVA, so every scanner and resolver sees the same bytes at the same offsets. No relocations are applied and no game code runs; addresses are only compared.

## The status channel

Features tell the player what happened through `sco::Status("Spawning %s...", name)`. It stores the message and writes `[status] Spawning ...` to the log. The menu shows the latest message with `sco::GetStatus()`. Features don't include menu headers, so feature code and UI code can change separately.

## Threading

| Call | Thread safety |
|---|---|
| `RegisterSignatures`, `ResolveAll` | Startup only, one thread. Not safe to run while other threads call `Sig()`. |
| `Sig`, `SigReady`, `SigLookup`, `SignatureResult` | Safe from any thread once `ResolveAll` has returned (read-only) |
| `Status`, `GetStatus` | Safe from any thread (mutex) |
| `SetLogSink`, `Log` | Safe from any thread (atomic sink); the sink itself must be thread-safe |
| `Post`, `Subscribe`, `Unsubscribe`, `RegisterCommand`, `ListCommands`, `Invoke` | Safe from any thread. Tasks, event callbacks and commands always run on the game thread |
| `GameThreadTick`, `DrainTasks`, `Dispatch`, `Release` | Game thread only (the thread that called `SetGameThread`); elsewhere they run nothing. `GameThreadTick` and `DrainTasks` also refuse to run inside a task, callback or command |

## The runtime

[`sco/runtime.h`](../include/sco/runtime.h) is how work reaches the game thread. The host calls `SetGameThread()` once from the game's main thread and `GameThreadTick(nowMs)` on every main-thread tick. Each tick first runs the tasks queued with `Post()`, in order, then dispatches the `tick` event.

- **Task queue.** A fixed ring of 256 tasks, the fast path: posting into it never allocates. When the ring is full, posts go to an overflow list under the same lock, and while the overflow holds anything new posts go there too, so tasks run in posting order across both; each drain moves what fits back into the ring. The total waiting is capped at 65,536 (`kMaxQueuedTasksHard`) so a runaway plugin can't eat memory: past it, or out of memory, `Post` refuses with `TooMany` and queues nothing. A drain runs only the tasks queued when it started; tasks posted while it runs (a task re-posting itself too) wait for the next tick. `Release` drops an owner's tasks from both stages without allocating.
- **Event bus.** Subscribers are keyed by owner, event name and callback. The subscriber list is replaced on every change and a dispatch walks the list it started with, so changes never invalidate the walk. A new subscriber is called from the next dispatch; a removed one is flagged and skipped at once, even by a dispatch already running. Since tasks never run during a dispatch, a task posted after `Unsubscribe` is the safe place to free `ctx`.
- **Commands.** Features register named actions (`spawn.ship`) with typed arguments. `Invoke()` checks the argument count and types and the command's capability before calling it. On the game thread it runs at once; from another thread it copies the name and arguments, queues a task, and reports the result through the `done` callback on the game thread. The registry copies each command's strings and arg defs into a slot that never moves, so `ListCommands()` pointers stay readable.
- **Owners.** Subscriptions, commands, tasks and queued `Invoke` calls carry an owner handle. `Release(owner)` removes them all at once and is final: later calls naming that owner are refused, so nothing it adds can outlive it. That is what unloading or disabling a plugin needs.
- **Services and faults.** `QueryService` hands the caller the provider's own table, which the runtime never calls into, so two rules live in the plugin loader ([plugins](plugins.md#crash-containment)). A fault is blamed on the code that faulted: the Windows guard's filter walks the faulting thread's frames and the loader marks the first plugin DLL found, not the callout's owner, as crashed (the caller stays loaded; built-ins share the product's module and keep owner blame). And a native plugin whose table was handed out (`sco::ServiceTableHandedOut`, a per-owner flag `QueryService` sets) is never unmapped: its services are withdrawn and its state changes as for any unload, but `ops.close` is skipped. [Design](design/service-safety.md) parts 1 and 3; the opt-in provider-side guard (part 2) is not built.

The runtime is C++ and internal (version 0). The plain-C [`sco_api.h`](../include/sco_api.h) is a thin layer over it: `Result` and `ArgType` share their numbers with `sco_result` and `sco_arg_type`, and `Arg` and `ArgDef` share their layout with `sco_arg` and `sco_arg_def` (checked at compile time). `Command` is not `sco_command`: the host table ([`sco/host.h`](../include/sco/host.h)) builds `sco_command` views of registered commands for `list_commands`.

## Limits

| Limit | Value | What happens at the limit |
|---|---|---|
| Rows across all tables | 512 (`kMaxRows`) | `RegisterSignatures` logs `signature table full` and returns false |
| Needs per row | 4 (`kMaxSigNeeds`) | Compile error |
| Pattern length | 96 bytes (`kMaxPatternBytes`) | The pattern matches nothing (`MISSING`), never a shortened match |
| Log line | 511 characters | Truncated |
| Status message | 255 characters | Truncated |
| Queued tasks | 65,536 in all (`kMaxQueuedTasksHard`); the first 256 (`kMaxQueuedTasks`) in an allocation-free ring, the rest in an overflow | `Post` returns `TooMany` |
| Event subscriptions | 512 (`kMaxSubscriptions`) | `Subscribe` returns `TooMany` |
| Commands | 512 registrations (`kMaxCommands`); released ones still use a slot | `RegisterCommand` returns `TooMany` |
| Command strings | name, title, capability 63; help 255; arg name 31; arg help 127 | `RegisterCommand` returns `BadArg` |
| Arguments per command | 16 (`kMaxCommandArgs`) | `RegisterCommand` / `Invoke` return `BadArg` |
| Command reply | 255 characters | Truncated |

## Kernel and game pack

sco-core builds as a game-agnostic **kernel** plus one **game pack**, Star Citizen's. The boundary is a build option that CI enforces, not a promise in a document.

- **Kernel:** `sco_api.h` and the runtime (`src/api`), the host table (`src/host`), plugins and the host kit (`src/plugins`, `src/app`), the host services `sco.storage`, `sco.ui`, `sco.ipc` and `sco.net`, the hook engine, the VFS engine, the zone tree (`src/engine`), the signature engine (`sco_scan.cpp`, `sco_signatures.cpp`, `sco_pe_file.cpp`) and sco-lua.
- **Star Citizen game pack** (`SCO_GAME_SC`, on by default): every row in `src/game/*_sigs.cpp` and `sco::game::RegisterGameSignatures` (`sco_game_sc`), the CryPak adapter (`sco_pak`), DataCore and `sco.datacore` (`sco_datacore`, `sco_datacore_service`), `sco-sigcheck` and `sco-dcb`. The SDK headers `sco_datacore.h`, `sc_spatial.h`, `sc_spawn.h` and `sc_vehicles.h` describe game-pack services; they stay in `include/` and in the SDK zip, so plugins built against sdk-v1.1.0 keep working.
- **The gate:** CI's `kernel-only` job (`cmake.yml`) configures with `-DSCO_GAME_SC=OFF`, checks that no game-pack object or symbol was built, and runs the tests. Kernel code that reaches into the game pack fails that job. Where the kernel can use a game-pack service when one is there (the host kit's `Platform::dataCore`, sco-lua's `sco.datacore` binding, `sco-host-sim`'s pack lint), the code is guarded by `SCO_KERNEL_ONLY` or finds the service at run time.
- **Game services:** the game pack publishes its services under the reserved owner `game` with `host::ProvideGameService` (the sibling of `ProvideHostService`), from `src/game/services/` (`sco_game_services`, Windows). Today: `teleport.spatial` 1.0 and `spawn.entities` 1.2, both moved from sc-offline's built-ins with the same tables, and `game.actors` 1.0 (your player, NPC spawn and despawn, moved from sc-offline's `npc.cpp`). `game.vehicles` 1.0 (seats, seating, Flight Ready; `vehicles.cpp`, moved from sc-offline's spawner seat code) has one capability per system (`game.vehicles.seats`, `.seat`, `.flight_ready`), `self` first on every function that changes the game, caller buffers and `last_error`, and wrappers in C++ (`scosdk/game/vehicles.hpp`), C# (`Sco.Sdk.Game`) and Lua (`sco.game.vehicles`, read-only); its `seat` / `eject` take the player's own actor and the caller's NPCs (`spawn_as` or `game.actors` `spawn_npc`). An NPC a plugin spawns through `game.actors` is removed when that plugin unloads or crashes: `spawn.cpp`'s one release hook (`sco::AddReleaseHook`) releases both services' per-plugin state. The same library also holds internal helpers a product calls directly, such as `sco::game::missions::ScriptLibrary` (the mission script library lookup, `sco/game/missions.h`). A product turns them on with `Platform::gameServices`. Its own version line: `SCO_GAME_PACK_VERSION` in `sc_game_pack.h`, tags `game-sc-vX.Y`, verified builds in [game-pack.md](game-pack.md). See [game services](game-services.md) and the [design](design/game-services.md).
- **A product** picks the pack's parts: sc-offline links `sco_app` and `sco_pak` (which bring `sco_game_sc`), registers the game tables itself before its patches, and starts the host kit with `Platform::dataCore`. A host for another game would link the kernel targets and pass its own tables as `Platform::registerSignatures`.

## Source map

| File | What |
|---|---|
| `src/sco_scan.cpp` | `FindPattern`, `FindCString`, `FindRipLea`, `BytesMatch`, `Rel32` |
| `src/sco_signatures.cpp` | Registry, resolver, report |
| `src/sco_log_status.cpp` | Log sink and status message |
| `src/sco_image_win.cpp` | `ModuleImage()` for the running game (Windows) |
| `src/sco_pe_file.cpp` | `FileImage::Load()` for host tools |
| `src/api/sco_tasks.cpp` | Game-thread identity, task queue, `GameThreadTick`, `Release` |
| `src/api/sco_events.cpp` | Event bus |
| `src/api/sco_commands.cpp` | Command registry and `Invoke` |
| `src/api/sco_services.cpp` | Service registry (`ProvideService`, `QueryService`) |
| `src/hook/sco_hook.cpp` | Near caves, `WriteCode`, the detour registry (`sco/hook.h`) |
| `include/sco/engine/types.h` | `Vector3d`, `Quatd`, `Transform`: header-only 64-bit spatial math, XYZW helpers for the game's quaternion order |
| `src/engine/zone.cpp` | `ZoneTree` (`sco/engine/zone.h`): zones the host feeds, transforms between them |
| `src/api/sco_caps.cpp` | Capability registry |
| `src/host/sco_host.cpp` | The host's `sco_api` table and per-plugin handles |
| `src/api/internal.h` | Callout depth and the per-module halves of `Release`, shared by `src/api/` |
| `src/game/signatures.cpp` | `RegisterGameSignatures()`: the list of game tables |
| `src/game/<feature>_sigs.cpp` | One feature's rows and its typed accessor |
| `tests/test_core.cpp` | Unit tests against a synthetic image |
| `tests/test_runtime.cpp` | Runtime tests (run under ASan+UBSan and ThreadSanitizer) |
| `tests/test_host.cpp` | Capability and `sco_api` table tests (ASan+UBSan and ThreadSanitizer) |
| `tests/test_hook.cpp` | Detours over small functions written into executable memory (x86-64) |
| `tests/test_spatial.cpp` | Spatial math and the zone tree (ASan+UBSan and ThreadSanitizer) |
| `tools/sco-sigcheck.cpp` | The offline checker |
| `src/datacore/datacore.cpp` | `sco::datacore::Schema::Parse` (`sco/datacore.h`): the DataCore layout and its validation |
| `tests/dcb_builder.h` | Synthetic DataCore files for tests (32-, 36- or 40-byte records), written independently of the parser |
| `tests/test_datacore.cpp` | Parser round trips, each refusal, truncated and corrupted files (ASan+UBSan) |
| `tools/sco-dcb.cpp` | `sco-dcb info` and `records` over a `Game2.dcb` on disk |
