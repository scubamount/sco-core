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

Ids are `<feature>.<thing>` in lowercase with underscores: `teleport.to_camera`, `teleport.entity_system`. The feature part matches the file under `src/game/` and the header under `include/sco/game/`. Ids are unique across all tables; registering a duplicate fails.

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
[core] signatures: 3/4 OK
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

- **Task queue.** A fixed ring of 256 tasks; posting never allocates. A full queue refuses with `TooMany` rather than growing or dropping work. Tasks posted while the queue drains wait for the next tick.
- **Event bus.** Subscribers are keyed by owner, event name and callback. The subscriber list is replaced on every change and a dispatch walks the list it started with, so changes never invalidate the walk. A new subscriber is called from the next dispatch; a removed one is flagged and skipped at once, even by a dispatch already running. Since tasks never run during a dispatch, a task posted after `Unsubscribe` is the safe place to free `ctx`.
- **Commands.** Features register named actions (`spawn.ship`) with typed arguments. `Invoke()` checks the argument count and types and the command's capability before calling it. On the game thread it runs at once; from another thread it copies the name and arguments, queues a task, and reports the result through the `done` callback on the game thread. The registry copies each command's strings and arg defs into a slot that never moves, so `ListCommands()` pointers stay readable.
- **Owners.** Subscriptions, commands, tasks and queued `Invoke` calls carry an owner handle. `Release(owner)` removes them all at once and is final: later calls naming that owner are refused, so nothing it adds can outlive it. That is what unloading or disabling a plugin needs.

The runtime is C++ and internal (version 0). The plain-C [`sco_api.h`](../include/sco_api.h) is a thin layer over it: `Result` and `ArgType` share their numbers with `sco_result` and `sco_arg_type`, and `Arg` and `ArgDef` share their layout with `sco_arg` and `sco_arg_def` (checked at compile time). `Command` is not `sco_command`: the host table ([`sco/host.h`](../include/sco/host.h)) builds `sco_command` views of registered commands for `list_commands`.

## Limits

| Limit | Value | What happens at the limit |
|---|---|---|
| Rows across all tables | 512 (`kMaxRows`) | `RegisterSignatures` logs `signature table full` and returns false |
| Needs per row | 4 (`kMaxSigNeeds`) | Compile error |
| Pattern length | 96 bytes (`kMaxPatternBytes`) | The pattern matches nothing (`MISSING`), never a shortened match |
| Log line | 511 characters | Truncated |
| Status message | 255 characters | Truncated |
| Queued tasks | 256 (`kMaxQueuedTasks`) | `Post` returns `TooMany` |
| Event subscriptions | 512 (`kMaxSubscriptions`) | `Subscribe` returns `TooMany` |
| Commands | 512 registrations (`kMaxCommands`); released ones still use a slot | `RegisterCommand` returns `TooMany` |
| Command strings | name, title, capability 63; help 255; arg name 31; arg help 127 | `RegisterCommand` returns `BadArg` |
| Arguments per command | 16 (`kMaxCommandArgs`) | `RegisterCommand` / `Invoke` return `BadArg` |
| Command reply | 255 characters | Truncated |

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
| `src/api/sco_caps.cpp` | Capability registry |
| `src/host/sco_host.cpp` | The host's `sco_api` table and per-plugin handles |
| `src/api/internal.h` | Callout depth and the per-module halves of `Release`, shared by `src/api/` |
| `src/game/signatures.cpp` | `RegisterGameSignatures()`: the list of game tables |
| `src/game/<feature>_sigs.cpp` | One feature's rows and its typed accessor |
| `tests/test_core.cpp` | Unit tests against a synthetic image |
| `tests/test_runtime.cpp` | Runtime tests (run under ASan+UBSan and ThreadSanitizer) |
| `tests/test_host.cpp` | Capability and `sco_api` table tests (ASan+UBSan and ThreadSanitizer) |
| `tools/sco-sigcheck.cpp` | The offline checker |
