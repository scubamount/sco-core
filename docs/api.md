# API reference

Everything is in namespace `sco` (game tables in `sco::game`). Version 0: internal to sc-offline and can change in any commit; see [CHANGELOG.md](../CHANGELOG.md). The plugin ABI, `include/sco_api.h`, is separate and versioned: see [Plugin API v1](api-v1.md).

## `sco/scan.h`: scanners

```cpp
struct Section { uint8_t* base; size_t size; };
struct Image   { uint8_t* base; Section text, rdata; uint32_t timestamp; uint32_t size; };
```

`Image` is the game image as the scanners see it: `base` is where it starts in memory, `text` and `rdata` are its two sections, `timestamp` and `size` come from the PE header.

Patterns are hex bytes separated by spaces, with `?` or `??` for any byte: `"48 8B 05 ?? ?? ?? ?? C3"`.

| Function | Returns |
|---|---|
| `int FindPattern(const Section& text, const char* pattern, uint8_t** out, int max)` | Number of matches; the first `max` are written to `out`. 0 for an empty pattern, an empty section, or a pattern longer than `kMaxPatternBytes` (96) |
| `uint8_t* FindUniquePattern(const Section& text, const char* pattern, int& matches)` | The match if there's exactly one, else `nullptr`; `matches` is the count |
| `bool BytesMatch(const uint8_t* p, const char* pattern)` | Whether the bytes at `p` match. Doesn't check bounds: `p` plus the pattern length must be inside the image |
| `const uint8_t* FindCString(const Section& s, const char* str)` | The first NUL-terminated copy of `str` that starts at a string boundary (`"Anchor"` doesn't match inside `"HelloAnchor"`) |
| `uint8_t* FindRipLea(const Section& text, uint8_t r0, uint8_t r1, uint8_t r2, const uint8_t* target)` | The first 7-byte instruction starting `r0 r1 r2` whose RIP-relative operand points at `target`. `48 8D 15` is `lea rdx, [rip+X]`; `4C 8D 05` is `lea r8, [rip+X]` |
| `int32_t Rel32(const uint8_t* p)` | The little-endian 32-bit displacement at `p` |
| `uint8_t* RipTarget(const uint8_t* insn, size_t dispOffset, size_t insnSize)` | `insn + insnSize + Rel32(insn + dispOffset)`: what a RIP-relative operand points at. `mov rax, [rip+X]` is `(insn, 3, 7)`; `call X` is `(insn, 1, 5)` |
| `Image ModuleImage()` | Windows only: the running process's main executable |

## `sco/signatures.h`: the registry

See [How it works](architecture.md) for the model.

```cpp
enum class SigState : uint8_t { NotRun, Ok, Missing, Ambiguous, Failed, Blocked };
struct SigResult { SigState state; uint8_t* at; int matches; const char* why; };
using  SigResolver = SigResult (*)(const Image& img);
struct SigDef { const char* id; const char* pattern; int ripDisp; int ripSize; SigResolver resolve; const char* needs[kMaxSigNeeds]; };
```

| Function | Does |
|---|---|
| `bool RegisterSignatures(const SigDef* rows, size_t n)` | Adds a table. False (and a log line) on a duplicate id or more than 512 rows in total; rows before the bad one stay registered. `rows` must outlive the registry (use a static array) |
| `void ResolveAll(const Image& img)` | Resolves every row; see [Resolving](architecture.md#resolving). Startup only |
| `uint8_t* Sig(const char* id)` | The address if the row is OK, else `nullptr` |
| `bool SigReady(const char* id)` | `Sig(id) != nullptr` |
| `const SigResult* SigLookup(const char* id)` | The full result, or `nullptr` for an unknown id |
| `void LogSignatureReport(bool verbose)` | Writes the `[core]` report; `verbose` also lists OK rows |
| `const char* SigStateName(SigState)` | `"OK"`, `"MISSING"`, `"AMBIG"`, `"FAILED"`, `"BLOCKED"`, `"NOT RUN"` |
| `size_t SignatureCount()`, `const SigDef* SignatureDef(size_t i)`, `const SigResult& SignatureResult(size_t i)` | Walk every row in registration order (used by `sco-sigcheck`) |

Helpers for resolvers:

| Helper | Returns |
|---|---|
| `SigOk(const void* at)` | `Ok` at `at` |
| `SigFail(const char* why)` | `Failed` with a reason; `why` must be a static string |
| `SigPattern(const Section& text, const char* pattern)` | `Ok`, `Missing` or `Ambiguous`, the same check a pattern row gets |

## `sco/game/signatures.h` and `sco/game/<feature>.h`: game tables

| Function | Does |
|---|---|
| `bool sco::game::RegisterGameSignatures()` | Registers every game table. Runs once; later calls return the first result |
| `bool sco::game::TeleportAddresses(TeleportAddrs& out)` | Fills `clientMgr`, `entitySystem` and `handleFromId`, or returns false and leaves `out` untouched if any `teleport.*` row isn't OK |
| `bool sco::game::QuitFunction(QuitHook& out)` (`sco/game/system.h`) | Fills `fn` (CSystem::Quit's entry) and `stolenBytes` (`kQuitStolenBytes`, 15: the whole instructions a 14-byte absolute jmp detour overwrites), or returns false and leaves `out` untouched if `system.quit` isn't OK |

Rows today:

| Id | How it's found |
|---|---|
| `teleport.to_camera` | The function that references the `CmdTeleportToCamera` string, with 19 layout checks plus entity-position and zone vtable checks |
| `teleport.client_mgr` | The global read at `to_camera+0x24` |
| `teleport.handle_from_id` | The function called at `to_camera+0x9F` |
| `teleport.entity_system` | The global read at `to_camera+0x2B0` |
| `system.quit` | CSystem::Quit: the function whose first `lea r9` to the `CSystem::Quit invoked with - cause=$$, ...` log string is at `+0xA3`, with 9 layout checks (the 15 prologue bytes a detour overwrites among them) and its second reference to the string at `+0x176`. `MISSING` if the string is gone |

## `sco/log.h`: log

| Function | Does |
|---|---|
| `void SetLogSink(LogSink sink)` | Where lines go: `void (*)(const char* line)`, one line per call, no trailing newline. `nullptr` drops lines (the default) |
| `void Log(const char* fmt, ...)` | printf-style; formats into 512 bytes and calls the sink |

## `sco/status.h`: status message

| Function | Does |
|---|---|
| `void Status(const char* fmt, ...)` | Stores the message (up to 255 characters) and logs `[status] <message>` |
| `bool GetStatus(char* out, size_t n)` | Copies the latest message into `out` (truncated to fit). False, with `out` empty, until the first `Status()` call; false for `out == nullptr` or `n == 0` |

## `sco/runtime.h`: game-thread runtime

See [The runtime](architecture.md#the-runtime) for the model. Every call returns `Result`: `Ok`, `Unavailable`, `NotFound`, `BadArg`, `Crashed`, `WrongThread`, `TooMany` (`ResultName()` gives `"OK"`, `"TOO_MANY"`, ...).

| Function | Does |
|---|---|
| `void SetGameThread()` | Marks the calling thread as the game thread. Call once, from the game's main thread |
| `bool OnGameThread()` | Whether the caller is the game thread; false until `SetGameThread()` |
| `Result GameThreadTick(uint32_t nowMs)` | Runs queued tasks, then dispatches `tick` with `data = &nowMs`. `WrongThread` off the game thread or from inside a task, callback or command |
| `Result Release(const void* owner, size_t* removed)` | Removes everything `owner` registered: subscriptions (at once, even mid-dispatch), commands, queued tasks and queued `Invoke` calls (dropped, `done` not called). Final: from the moment it starts, every `Post`, `Subscribe`, `RegisterCommand` and `Invoke` naming that owner returns `BadArg`, from any thread, so afterwards the runtime never calls the owner's functions again. Game thread only; `BadArg` for `nullptr` or an owner already released; `TooMany` only when out of memory (nothing removed; retry). No limit on released owners. Then calls every release hook |
| `Result AddReleaseHook(ReleaseHook hook)`, `Result RemoveReleaseHook(ReleaseHook hook)` | For host modules with per-owner state outside the runtime (`sco::storage`: a plugin's database): `hook(owner)` runs on the game thread at the end of every successful `Release`, in the order added. At most 8 (`TooMany`); adding one twice changes nothing; `RemoveReleaseHook` answers `NotFound` for one not added. A hook must not call `Release` |
| `Result Post(TaskFn fn, void* ctx, const void* owner)` | Queues `fn(ctx)` for the game thread, in order. `owner` (default `nullptr`) lets `Release` drop it. `TooMany` with 256 waiting, `BadArg` for a null `fn` |
| `size_t DrainTasks()`, `size_t QueuedTasks()` | Run the tasks queued when the call started (game thread only and never nested, else 0); count waiting tasks |
| `Result Subscribe(const void* owner, const char* event, EventFn fn, void* ctx)` | Adds a subscriber from the next dispatch; the name is copied. `BadArg` for a null/empty event, null `fn` or the same (owner, event, fn) twice; `TooMany` at 512 |
| `Result Unsubscribe(const void* owner, const char* event, EventFn fn)` | Removes it at once (a running dispatch skips it); `NotFound` if absent. From another thread, free `ctx` in a task posted after it returns |
| `Result Dispatch(const char* event, const void* data, size_t* called)` | Calls the event's subscribers in subscription order; `called` gets the count. Game thread only (`WrongThread`) |
| `Result RegisterCommand(const void* owner, const char* prefix, const Command& cmd)` | Adds a command. Copies every string and the arg defs; only `fn` and `ctx` are borrowed until `Release(owner)`. Names are `<x>.<y>` in lowercase, digits, `_`. With `prefix` (a plugin's name, no dot) the name must start `<prefix>.`, the prefix can't be reserved (`sco`, `host`, `menu`, `game`) or used by another owner's live command. `BadArg` for a bad or too-long name or string, a live duplicate, null `fn` or more than 16 args; `TooMany` after 512 registrations (released slots still count) |
| `size_t ListCommands(const Command** out, size_t max)` | Writes up to `max` live commands in registration order; returns how many are live. Pointers and their strings stay readable for the life of the process |
| `void SetCapabilityCheck(CapabilityCheck check)` | Answers `Command::capability`. With none installed, commands that name a capability are `Unavailable` |
| `Result Invoke(const char* name, const Arg* args, uint32_t nargs, InvokeDone done, void* ctx, const void* owner)` | Runs a command; see below |

`Invoke(name, args, nargs, done, ctx, owner)` on the game thread runs the command at once, calls `done(result, reply, ctx)` (if set) and returns the same result. From another thread it copies the name and arguments, queues the call as a task of `owner` and returns `Ok`; `done` runs exactly once on the game thread on a later tick, unless `Release(owner)` drops the call first. Any other return (`BadArg`, or `TooMany` for a full queue or no memory) means `done` is never called. Results: `NotFound` (no such live command), `BadArg` (argument count or type differs from the command's `ArgDef`s, a null string, or a `Bool` that isn't 0 or 1), `Unavailable` (capability check), else what the command returned. `reply` is at most 255 characters and always NUL-terminated.

### Services

`ProvideService(owner, prefix, name, version, table)` publishes a function table under a name; `QueryService(name, minVersion, &table)` finds it (same major, at least as new); `ReleaseService(owner, name)` withdraws one. `Release(owner)` withdraws all of the owner's. The host's `provide_service` / `query_service` / `release_service` ([API v1 § Services](api-v1.md#services-11)) pass the plugin's id as the prefix; a host feature may pass a null prefix. The runtime keeps only the name, version and pointer and never calls the table.

### Raw handlers

`RegisterRaw(owner, prefix, name, capability, fn, ctx)` registers a byte-in, byte-out handler under a command-style name; `InvokeRaw(caller, name, in, inSize, out, &outSize)` calls it now on the game thread, after the capability check and as one callout of its owner (a fault is `Crashed`). `*outSize` is the capacity in and the bytes written (or, with `TooMany`, needed) out. `Release(owner)` removes the owner's handlers. The host's `register_raw` / `invoke_raw` ([API v1 § Raw handlers](api-v1.md#raw-handlers-11)) run through a per-registration trampoline, like commands.

## `sco/caps.h`: capabilities

Named yes/no answers to "does this feature work on this game build?". `sco_api.has()` and the command capability check answer from here once `sco::host::BuildApi` has run. Every function is safe from any thread.

| Function | Does |
|---|---|
| `Result caps::Set(const char* name, bool ready, const char* reason)` | Sets or updates a capability. Names are lowercase dotted segments (`teleport`, `spawn.ship`), at most 63 characters; `reason` is copied (127 characters, truncated) and says why it isn't ready. Names are never removed. `BadArg` for a bad name; `TooMany` after 256 names |
| `Result caps::SetFromSignatures(const char* name, const char* const* ids, size_t n)` | Ready when every listed signature row is OK; else not ready with `needs <id> (<STATE>)` or `unknown signature <id>` for the first row that isn't. Call after `ResolveAll` |
| `bool caps::Has(const char* name)` | True when set ready; unknown names and `nullptr` are false |
| `size_t caps::List(Entry* out, size_t max)` | Copies up to `max` entries (name, ready, reason) in first-set order; returns the number of names |

## `sco/host.h`: the host's `sco_api` table

| Function | Does |
|---|---|
| `const sco_api* host::BuildApi(const HostInfo& info)` | The process's one `sco_api` table, over the runtime, caps, status and log. Wires `SetCapabilityCheck` to `caps::Has`. Calling again updates `host_version` and returns the same table; `nullptr` for a null version |
| `sco_plugin* host::NewPlugin(const char* id)` | A handle for one plugin load: `[a-z0-9_]`, 1-31 characters, not `sco`/`host`/`menu`/`game`, and not held by a handle that hasn't been released. Never freed or reused; 256 for the life of the process. The handle is the plugin's runtime owner, so `sco::Release(self)` removes everything it added |
| `const char* host::PluginId(const sco_plugin* p)` | The handle's id; `nullptr` for a pointer `NewPlugin` didn't return |
| `Result host::ProvideHostService(const char* name, uint32_t version, const void* table)` | Publishes a [host-owned service](api-v1.md#host-owned-services) under the reserved id `sco` (`host::kHostId`): `name` must be `sco.<name>`, the service name rule otherwise. Owner `host::HostOwner()`, never released. `BadArg` for another name, a taken one or a null table. Any thread |
| `Result host::WithdrawHostService(const char* name)`, `size_t host::WithdrawHostServices()` | Withdraw one (`NotFound` if the host publishes nothing by that name) or all (host shutdown; returns how many). A withdrawn name can be published again |

The table checks `self` on every call (`SCO_BAD_ARG` for a pointer the host didn't hand out or one already released; `status` and `log` are dropped). `register_command` uses the plugin id as the prefix, needs `size >= sizeof(sco_command)` and reads arg defs with `arg_def_size`. `list_commands` returns host-built views of every live command, host features' too; in a view `fn` and `ctx` are `NULL` (run commands with `invoke`). `log` writes `[<id>] message`, with `warning: ` or `error: ` for the higher levels.

## `sco/app.h`: the host kit

The startup order every product needs, over the host table, caps, signatures and the plugin loader. Game thread only.

```cpp
struct Platform {
    const char* hostVersion;                       // "sc-offline 0.8.0"; static
    std::filesystem::path pluginRoot;              // data/plugins
    bool pluginsEnabled = false;                   // plugins = on|off; built-ins load either way
    const plugins::Builtin* builtins = nullptr; size_t nBuiltins = 0;
    const plugins::ScriptRuntime* scripts = nullptr;   // sco-lua, or nullptr
    const Image* image = nullptr;                  // resolve signatures against this; nullptr skips
    void (*setCapabilities)() = nullptr;           // the product sets caps after ResolveAll
    plugins::ModuleOps moduleOps = plugins::PlatformModuleOps();
    std::filesystem::path dataRoot;                // data; set: sco.storage in <dataRoot>/storage/
};
```

| Function | Does |
|---|---|
| `bool app::Start(const Platform& pf)` | `SetGameThread`; with `image`, `RegisterGameSignatures` and `ResolveAll(*image)`; `setCapabilities()`; `host::BuildApi`; with `dataRoot`, `storage::Start` (a failure is logged `[app] storage not started: <RESULT>`); the list (every built-in, then `Discover(pluginRoot)` when `pluginsEnabled`); `ContainCallouts`; `LoadBuiltin` for the built-ins, then `LoadNative` / `LoadScript` in list order (a `lua` plugin is refused `no script runtime` without `scripts`); the content index; with `image`, `LogSignatureReport(false)`; `LogReport`; dispatches `game.ready`. Problems are logged, never fatal. False (nothing changes) when already started |
| `void app::Tick(uint32_t nowMs)` | `GameThreadTick(nowMs)`. No-op unless started |
| `void app::Stop()` | Dispatches `game.exit`, then `UnloadAll` (newest first, built-ins last), `storage::Stop`, `host::WithdrawHostServices` (host services outlive every plugin) and `ContainCallouts(nullptr)`. No-op unless started; `Start` works again afterwards, with fresh handles |
| `const std::vector<plugins::Plugin>& app::Plugins()` | The list of the last `Start` (built-ins first), final states after `Stop`. Never resized between two `Start`s |
| `const plugins::ContentIndex& app::Content()` | The data-pack index of the last `Start` |

`Platform` is copied; `builtins`, `scripts` and `hostVersion` must outlive `Stop`.

Call `Stop` from the game's own quit path, on the game thread. The game's menu Quit calls `CSystem::Quit` (`Quit via console command`), then `System Fast Shutdown (ExitOnQuit enabled)`: the process ends without the message loop ever getting `WM_QUIT`, so a `WM_QUIT` hook never runs `Stop` and plugins never see `game.exit`. sc-offline hooks `CSystem::Quit` through the `system.quit` signature row (`sco/game/system.h`, `QuitFunction`: the entry and the 15 prologue bytes a detour may overwrite). Never call `Stop` from `DLL_PROCESS_DETACH`: it runs under the loader lock, at the wrong time. Even from the quit path, `game.exit` is best effort: a crash or a killed process never sends it. Start the host kit regardless of whether any one feature resolved. Why: [framework plan, Lessons](framework.md#lessons). Further Phase 5 additions ([framework plan](framework.md#phase-5-services-and-storage)) will be more `Platform` fields with defaults.

## `sco/storage.h`: the `sco.storage` service

The host side of [`sco.storage`](storage.md) (library `sco_storage`, over vendored SQLite in `third_party/sqlite`). `sco::app::Start` calls it when `Platform::dataRoot` is set; other hosts call it themselves. Any thread.

```cpp
struct Options {
    std::filesystem::path dataRoot;      // databases go in <dataRoot>/storage/<plugin id>.db
    uint64_t quotaBytes = 64ull << 20;   // per plugin; past it a write is SCO_TOO_MANY
    uint32_t busyTimeoutMs = 1000;       // waiting for a lock another process holds
    uint32_t budgetMs = 1000;            // per call; a statement still running is interrupted
    bool     durable = true;             // synchronous FULL (false: NORMAL)
};
```

| Function | Does |
|---|---|
| `Result storage::Start(const Options& o)` | Publishes `sco.storage` 1.0 with `host::ProvideHostService` and installs a release hook, so `Release(self)` rolls back the plugin's transaction and closes its cursors and database. `BadArg`: empty `dataRoot`, `budgetMs` 0, already started. The folder is created on the first call |
| `void storage::Stop()` | Withdraws the service, rolls back open transactions, closes every database; the table then answers `SCO_UNAVAILABLE`. No-op unless started. Call after every plugin has unloaded |
| `bool storage::Started()`, `const sco_storage_v1* storage::Table()`, `fs::path storage::DatabasePath(const char* id)` | State, the table `query_service` hands out, and a plugin's database path (empty unless started) |

## `sco/hook.h`: detours and near-code memory

One place that patches game code (library `sco_hook`, x86-64 Windows and Linux), so two features or plugins can't detour the same function or allocate over each other.

| Function | Does |
|---|---|
| `InstallDetour(target, stolen, detour, &original)` | Detours `target`. The stolen bytes must be whole instructions with no RIP-relative operand or relative branch: they are copied as-is into the trampoline. `stolen = 0` counts them with `StolenLength(target, 5)` (`BadArg` when it can't); otherwise pass 5-32, pinned by a signature row's layout checks as `sco::game::QuitHook::stolenBytes` is. Near path: a relay (14-byte absolute jump to `detour`) and the trampoline (stolen bytes, then a jump back) go in a cave near `target`; `target` becomes `E9 rel32` plus NOPs. Far path, when no cave can be mapped within ±2 GiB: `target` becomes `FF 25 00000000 <detour>` (14 bytes) plus NOPs and the trampoline goes in any executable memory; this needs 14 stolen bytes (`stolen = 0` recounts with `StolenLength(target, 14)`), `NoCave` with fewer. `AlreadyHooked` for a second detour on one target |
| `RemoveDetour(target)` | Restores the stolen bytes; the trampoline stays callable |
| `RemoveAll()` | Restores every detour, last installed first (host shutdown, tests); returns how many. One the OS refused to restore stays installed |
| `Transaction`: `Add(target, stolen, detour, &original)`, `Commit()`, `Rollback()` | Hooks that only make sense together. `Add` queues; `Commit` installs in order and, if one fails, removes the ones it installed and returns that first error; `Rollback` removes what `Commit` installed; the destructor leaves committed hooks in place. Opt-in and **not for independent hooks**: sc-offline installs each feature's hook on its own, so one missing pattern leaves the others working |
| `StolenLength(code, atLeast)` | Bytes of whole instructions at `code` covering at least `atLeast`, or 0 when it meets one it can't decode or can't move (below), or a `ret` / `jmp r/m` before `atLeast` (what follows may be another function) |
| `MemoryProtectScope(address, size)` | RAII: the range is read/write/execute until the scope ends, then the old protection comes back (Windows; POSIX: read + execute). `Succeeded()` |
| `AllocateNear(anchor, n)` | `n` bytes (at most 64 KiB) of read/write/execute memory within ±2 GiB of `anchor`, 16-byte aligned, never freed |
| `WriteCode(at, bytes, n)` | Writes over code: unprotect (`MemoryProtectScope`), copy, restore, flush the instruction cache |
| `IsHooked(target)`, `DetourCount()`, `ErrorName(e)`, `LastOsError()` | Queries |
| `SetNearAllocatorForTesting(fn)` | **Test-only**: replaces the near-cave search (`nullptr` restores it) so a test can force the far path. Not for products |

`StolenLength` decodes the usual x86-64 prologue set: prefixes `66`/`F2`/`F3` and REX `40`-`4F`; `push`/`pop r` (`50`-`5F`); the ALU families `00`-`3D` (add/or/adc/sbb/and/sub/xor/cmp, r/m and accumulator-immediate forms); `80`/`81`/`83` imm; `63` movsxd; `68`/`6A` push imm; `69`/`6B` imul; `84`-`8B` test/xchg/mov; `8D` lea; `90`-`99`; `A8`/`A9` test imm; `B0`-`BF` mov r, imm (imm64 with REX.W); `C0`/`C1`/`D0`-`D3` shifts; `C2`/`C3` ret; `C6`/`C7` mov r/m, imm; `C9` leave; `CC`; `F6`/`F7` (test imm and the one-operand group); `FE`/`FF` inc/dec/call/jmp/push r/m; and `0F` `10`/`11`/`28`/`29` movups/movss/movsd/movaps, `1E` (endbr64), `1F` nop, `40`-`4F` cmov, `57` xorps, `90`-`9F` setcc, `AF` imul, `B6`/`B7`/`BE`/`BF` movzx/movsx; full ModRM/SIB/disp8/disp32. It refuses RIP-relative operands, `E8`/`E9`/`EB`/`70`-`7F`/`0F 80`-`8F` (relative call/jmp/jcc), `E0`-`E3` (loop/jrcxz), `C7 F8` (xbegin), far call/jmp, VEX/EVEX and anything not listed.

Errors: `BadArg`, `AlreadyHooked`, `NoCave`, `Protect` (the OS refused; `LastOsError()`), `NotHooked`, `Unsupported` (not x86-64). Patching isn't atomic against a thread running `target` at that moment: install before the game runs it, or from its own thread.

## `sco/engine/types.h`: spatial math

Engine-agnostic 64-bit math in namespace `sco::engine`, header-only (no library needed). sco-core holds no game offsets for it and never reads game memory: the host converts what it reads from the game into these types.

| Type | Has |
|---|---|
| `Vector3d` | `x, y, z` (24 bytes); `+ -` (also unary `-`), `* /` by a scalar, `Dot`, `Cross`, `LengthSquared`, `Length`, `Normalized` (the zero vector when the length is under 1e-12), `DistanceTo` |
| `Quatd` | `w, x, y, z` in that order (32 bytes; default identity); `Identity()`, `Conjugate`, `operator*` (Hamilton: `(a * b).Rotate(v) == a.Rotate(b.Rotate(v))`), `Normalized` (identity for a zero quaternion), `Rotate(v)`, `Unrotate(v)` (the inverse rotation), `FromAxisAngle(axis, radians)` (right-handed, axis normalized) |
| `Transform` | `position`, `rotation`, `scale` (uniform, default 1, must not be 0); `TransformPoint(local) = position + rotation.Rotate(local * scale)`, `InverseTransformPoint(parent)`, `a * b` (b is a child frame of a: `(a * b).TransformPoint(p) == a.TransformPoint(b.TransformPoint(p))`) |

Everything that doesn't need `sqrt`/`sin`/`cos` is `constexpr`. All three are standard-layout; the sizes and `Quatd`'s `w`-first order are static asserts.

**Quaternion order at the game boundary.** The game keeps rotations as `double rot[4]` in (x, y, z, w) order (as sc-offline's spawner reads and writes them), not `Quatd`'s (w, x, y, z). Convert with `Quatd FromXYZW(const double q[4])` and `void ToXYZW(const Quatd& q, double out[4])`; never cast the array to a `Quatd`.

## `sco/engine/zone.h`: the zone tree

Nested reference frames (system > planet > city > ship > room) and the transforms between them, in library `sco_engine`. Pure math over data the host feeds it: the host (sc-offline) reads the game's zone objects each tick and calls `Set` / `Remove`; features ask for positions in whichever zone they need. `ZoneTree` is a class, not a singleton: the host owns one.

```cpp
struct Zone { uint64_t id; uint64_t parentId; std::string name; Transform local; };
```

Ids are the host's. 0 is never a zone: as a `parentId` it makes a root zone (its transform is relative to the world), and as a query id it names the world frame.

| Function | Does |
|---|---|
| `bool Set(id, parentId, name, localTransform)` | Inserts or updates a zone. False for id 0 or `parentId == id`. The parent may come later; queries through the zone fail until it exists |
| `bool Remove(id)` | Removes one zone; its children stay and their queries fail until the id is set again |
| `void Clear()`, `bool Has(id)`, `size_t Size()` | |
| `bool Find(id, Zone* out)` | Copies the zone |
| `bool LocalToWorld(id, local, Vector3d* out)`, `bool WorldToLocal(id, world, Vector3d* out)` | A point in zone `id` to the world and back |
| `bool Transform(fromId, toId, pos, Vector3d* out)` | A point in one zone to another (either may be 0). Goes up only to the lowest common ancestor, never through world coordinates |

The queries return false and leave `out` alone when `out` is null, the zone or an ancestor is missing, or the chain from the zone to its root is longer than `ZoneTree::kMaxDepth` (32 zones) or loops (a cycle hits the same limit). Lookups are hash-map finds, a query is one walk per chain.

**Precision.** A double resolves about 1.5e-5 m at 1e11 m (Stanton's planets are that far from the star), so a world coordinate out there is only that good. `Transform` between two zones on one planet or ship stops at their common ancestor and keeps sub-micrometre precision whatever the planet's distance; use it rather than going through `LocalToWorld` and `WorldToLocal`.

**Threads.** Every method is safe from any thread. Queries take a shared lock and run concurrently; `Set`, `Remove` and `Clear` take it exclusively. Each query sees one consistent tree, but two queries in a row may see a `Set` between them: do related math in one `Transform` call.

## `sco/vfs.h`: game-file overrides

The engine-agnostic core of game-file overrides ([design](design/vfs-datacore.md#3-scovfs-game-file-overrides)), in library `sco_vfs`. A mounted game file is served as a virtual file: ranges of the real (base) file plus replacement bytes, described by splices in base offsets. Bytes, offsets and paths only; the engine adapter that routes CryPak's open/read/seek/close here comes separately.

```cpp
struct Splice   { uint64_t at, removed; std::shared_ptr<const Bytes> bytes, old; };   // base offsets
struct Segment  { uint64_t start, len; Kind kind /* Base | Buffer */; uint64_t from; const uint8_t* src; };
struct Composed { uint64_t baseSize, size; std::vector<Segment> segments; std::vector<std::shared_ptr<const Bytes>> buffers; };
struct Limits   { uint64_t bufferBytes = 64 MiB; size_t splicesPerPath = 65536; uint64_t fileSize = 4 GiB; };
struct Result   { std::string error; /* empty = ok */ };
```

| Function | Does |
|---|---|
| `Result Compose(baseSize, splices, Composed& out, maxSize = kMaxFileSize)` | Validates the splices and builds segments that tile `[0, size)`. Each splice removes or adds something; they are sorted by `at`, never at the same offset, never overlapping, inside the base; `old`, when set, is exactly `removed` bytes. The error names the first bad splice (`splice 3 (at 4096): overlaps splice 2 (at 4000, removes 100)`); `out` is untouched on failure |
| `Reader(file)`, `bool Seek(int64_t off, Whence)` | One open handle. `Set`, `Cur`, `End` (from `Size()`); a negative or overflowing result fails and leaves the position alone. Past the end is allowed, and reads there return 0 |
| `size_t Read(dst, n, BaseIo& base)` | Copies from `Tell()`. Finds the segment by binary search, copies buffers, reads base ranges through `base` (seeking it only when its position isn't already the one needed). A failed seek or short base read returns the bytes read so far |
| `Tell()`, `Size()`, `Eof()` | From the virtual file; `Eof()` is `Tell() >= Size()` |
| `std::string NormalizePath(path)` | `\Data\Game2.DCB` -> `data/game2.dcb`; empty over `kMaxPathLength` (1024) |

`BaseIo` is the engine's original read and seek for the handle (`bool Seek(uint64_t)`, `size_t Read(void*, size_t)`). Positions are 64-bit throughout; the adapter narrows the engine's `int` seeks.

**Mounts.** A `Mount` is `{ path, priority, source, producer }`, where the producer is a `SpliceList` (fixed splices, optionally with an expected base `header` and each splice's expected `old` bytes) or a `Transform` (`Result(BaseIo&, baseSize, std::vector<Splice>& out)`, run at composition time; `sco::datacore` will be one). `Table::Build(mounts, limits)` makes an immutable table:

| Member | Does |
|---|---|
| `bool Mounted(path)` | Any path form; no allocation, so unmounted files cost one hash lookup |
| `shared_ptr<const Composed> Open(path, base, baseSize)` | The virtual file, or null to pass through (not mounted, or every mount inert). Composes once per path and base identity (size plus a hash of the first 4 KiB) under a per-path guard: concurrent opens wait for the one composition. Never throws |
| `std::vector<MountInfo> Mounts()` | `{ path, source, priority, state, reason }` per mount, in Build order |
| `uint64_t BufferBytes()` | Replacement bytes counted against `Limits::bufferBytes` |

| State | Means |
|---|---|
| `Pending` | Not composed yet |
| `Applied` | In the virtual file. `reason` counts splices dropped because a higher-priority mount of the path already covers them, and names that mount |
| `Inert` | Contributes nothing this run: header or expected old bytes differ (`expected bytes differ at 150 (splice 2; game updated?)`), the transform failed or threw, or the merged path broke a limit. Other mounts of the path still apply |
| `Refused` | By Build: bad path, invalid splices, a mount over `splicesPerPath`, or over the replacement-byte budget (taken highest priority first, so the lowest go) |

All mounts of a path splice the same base. Higher `priority` wins an overlap, and among equal priorities the later mount in the Build list wins. A file is never served half-applied: if the merged result fails (too many splices, over `fileSize`), every mount of the path is inert and the file passes through.

**Threads.** `MountTable` holds the published `shared_ptr<const Table>`: `Current()` and `Publish()` are atomic, and handles keep the table and file they opened with. `Table` is safe from any thread. A `Reader` belongs to one handle and takes no lock; no sco-core lock is held while it calls `BaseIo`. Composition calls `BaseIo` and transforms under the path's guard.

## `sco/datacore.h`: the DataCore file

The game's DataCore database (`Data\Game2.dcb`) in library `sco_datacore` ([design](design/vfs-datacore.md), sections 2 and 4): the parser below, and the patcher that turns semantic overrides into `sco::vfs` splices ([Patching](#patching-datacore)). Standard library only; no engine, no Windows. `AddRecord` comes in a later PR.

```cpp
sco::datacore::Schema s;
if (!s.Parse(bytes)) Log("datacore: %s", s.error.c_str());   // bytes: std::span<const uint8_t>, kept alive
```

| Member | Does |
|---|---|
| `bool Parse(std::span<const uint8_t>)` | Reads the header and tables and runs validation rules 1-6. False with `failed` (a `Check`) and `error` (`"layout: ..."`) for the first failing check; the members then hold what was read before it. The bytes must outlive the `Schema` and stay unchanged (names are views into them) |
| `header`, `recordSize`, `tables`, `dataOffset`, `dataSize` | The section 2 header, the derived record entry size (32, 36 or 40), every table in file order with offset, count, entry size and bytes (they tile the file) |
| `structs`, `properties`, `enums`, `mappings`, `records`, `enumOptions` | The definition tables and records as stored |
| `structInfo[i]` | Computed instance `size`, `opaque` (rule 6: a field of unknown type; sized from its records if it has instances), `instances`, `blockOffset` |
| `blockOffsets[m]` | File offset of mapping `m`'s data block |
| `Name(off)`, `ValueString(off)`, `StructName(i)` | Strings from the name and value pools; empty when out of range |
| `Properties(i)` | A struct's property indices, inherited first |
| `FindStruct(name)`, `FindRecord(guid)`, `FindRecordByName(name)` | Lookups; the first entry wins when one repeats |

`Check` is `None`, `File` (smaller than the header), `Totals` (rules 1-3: nothing adds up to the file size), `Structure` (an index or range outside its table), `RecordSize` (rule 4) or `NameOffset` (rule 5). `FormatGuid` prints a `Guid` the way unp4k does. Every count is checked against the file size before anything is read or allocated, so a truncated or corrupted file is refused, never read past. A `Schema` is plain data: concurrent `const` use is safe. The tool on top is [`sco-dcb`](datacore.md). `File()` returns the bytes it parsed.

### Patching DataCore

`Patch` collects one batch of overrides against a parsed file and emits the splices `sco::vfs` serves ([design section 4](design/vfs-datacore.md#4-the-semantic-datacore-patcher)). Records are addressed by GUID or name, fields by name, never by offset, so a batch written once applies to any build whose records and fields still exist.

```cpp
sco::datacore::Patch p(schema);                      // schema: a parsed Schema; it and its bytes outlive p
sco::datacore::RecordRef ship{ std::nullopt, "AEGS_Gladius" };
p.OverrideField(ship, "Components[SCItemQuantumDriveParams].params.spoolUpTime", Value::OfFloat(4.0));
sco::datacore::InstanceId fresh;
p.AddInstance("SCItemQuantumDriveParams", { ship, "Components[SCItemQuantumDriveParams].params" }, fresh);
p.SetPointer(ship, "Components[SCItemQuantumDriveParams].params", fresh);
std::vector<sco::vfs::Splice> splices;
if (Status st = p.Emit(splices); !st) Log("datacore: %s: %s", RefusalName(st.category), st.message.c_str());
```

| Member | Does |
|---|---|
| `OverrideField(rec \| instance, path, Value)` | Writes a bool, integer, float, double, guid, string, locale, enum (by option name) or pointer (`Instance` or null) in place. The value's kind must fit the field's type and range |
| `AddInstance(type, cloneFrom, out)` | Appends an instance to the end of the struct's block (the last mapping's), copied from an instance of exactly that struct or zero-filled; `out` is its id |
| `SetPointer(rec \| instance, path, target)` | Points a strong or weak pointer (field or array element) at an instance of its type or a derived one |
| `AppendElement(rec \| instance, path, Value)` | Copies the array's elements plus the new one to the end of its pool (or block, for arrays of structs, where `Value` is the instance to copy) and repoints the array; appends in place when the array already ends there |
| `FindInstance(source, out)` | An existing instance (record root, array element, strong pointer target) as a pointer target |
| `Emit(out)` | The splices: sorted, non-overlapping, each with its expected `old` bytes, the header rewritten as one 120-byte overwrite when a count or the value-string length changes. Then re-validation: the splices are applied to the base and the result re-parsed. On any failure `out` is empty |
| `Reports()` | One `{ op, Status }` per operation, in call order |
| `ApplySplices(base, splices, out)` | Applies splices to a base in memory through `Compose` and `Reader`, checking `old` bytes (tests and tools) |

**Paths** walk from the record's root instance: `name` (a property, inherited ones included), `name[3]`, `name[Type]` (the first element whose struct is `Type` or derives from it). Inline structs and strong pointers are followed; weak pointers and references are not.

**Refusals are data.** `Status` is `{ Refusal category; std::string message; }`; act on the category, show the message (`record "ShipA" field "speedX": no property "speedX" in Ship`). Categories: `Layout` (the base failed validation), `BadArgument`, `RecordNotFound`, `StructNotFound`, `FieldNotFound` (also null, weak and reference steps), `IndexOutOfRange`, `TypeMismatch`, `ValueOutOfRange`, `UnknownEnumOption`, `Opaque` (validation rule 6), `Unsupported` (reference fields until research R1; structs with no data block), `DependencyFailed` (an instance whose `AddInstance` was refused), `Corrupt` (the file's arrays or pointers point outside their pools), `Limit` (a count past 32 bits) and `Revalidation`.

**Atomicity.** An operation is checked in full before it writes, so a refused one changes nothing. With `PatchOptions::atomic` (the default, the design's per-pack rule) `Emit` refuses the whole batch with the first refused operation's category; with `atomic = false` it emits the accepted ones. Nothing existing is renumbered: overwrites are in place, and instances, array copies and strings are appended. Strings reuse the field's current pool offset when it already holds the same text, or one this batch added; the value-string pool is never searched. A null pointer is written as struct and instance `0xFFFFFFFF`, as 4.10.193 stores it.

Also on `Patch`: `Fork()` (a copy to try more operations on and keep or drop whole, which is how packs get per-pack atomicity), `ReadFields(rec | instance, path, out, maxDepth)` (the values under a record or field as patched so far, in pack syntax: what `sco-dcb show` prints), and each `OpReport` carries the `FieldSlot` an `OverrideField` or `SetPointer` wrote, so two operations that reach one field by different paths are recognized. Free functions: `CheckFieldPath(path)` (syntax only) and `ParseGuid(text, out)` (the inverse of `FormatGuid`).

### DataCore data packs: `sco/datacore_pack.h`

The `.toml` files of a data pack's `datacore\` folder ([format](datacore.md#pack-format)) as patcher operations. `ParsePack(text, pack, error)` reads one file with vendored toml++ (no exceptions; the first error with its line) and checks everything that needs no game file. `WritePack(pack)` writes the canonical form back. `ApplyPacks(schema, packs)` applies packs in the order given, which is plugin order: each pack is tried on a `Fork` of what is accepted so far and kept or dropped whole (or per operation with `atomic = false`); a later pack wins a field an earlier one set, and the conflict is listed with both sources; then one `Emit`. The result is a `PackResult` with `{ status, splices, packs (one PackReport each: state Applied, Partial or Refused, applied and skipped counts, the first reason, one PackOpReport per patcher call), conflicts }`. `Summary(result)` gives the log line `[datacore] 3 packs: gladius_qt 12/12 applied; ui_tweaks 40/41 applied (1 skipped: line 9: ...); old_mod refused (line 3: ...)`.

```cpp
sco::datacore::Pack pack;
pack.plugin = "gladius_qt";                         // its position in plugin order is the priority
pack.name = "datacore/drive.toml";
std::string error;
if (!sco::datacore::ParsePack(text, pack, error)) Log("[datacore] %s: %s", pack.name.c_str(), error.c_str());
const auto result = sco::datacore::ApplyPacks(schema, std::vector{ pack });
Log("%s", sco::datacore::Summary(result).c_str());   // result.splices go to sco::vfs
```

The content index lists the files (`ContentKind::DataCore`, [plugins](plugins.md#data-packs)); applying them when the game opens `Game2.dcb` is the CryPak adapter's job (design plan PR 7), and the capability `datacore.pack.<id>` follows each pack's state there.

## `sco/pe_file.h`: host tools only

```cpp
struct FileImage {
    std::vector<uint8_t> mem;  Image img;  uint64_t preferredBase;  std::string error;
    bool Load(const std::string& path);
    uint32_t Rva(const void* p) const;
};
```

`Load` reads a 64-bit PE file and copies each section to its RVA in `mem`, so `img` looks like the loaded game. On failure it returns false and sets `error` (`not a PE file`, `not a 64-bit PE image`, `no .text or .rdata section`, ...). `Rva(p)` turns a pointer into `mem` back into an offset. Not compiled into the game DLL.
