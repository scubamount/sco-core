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
| `Result Release(const void* owner, size_t* removed)` | Removes everything `owner` registered: subscriptions (at once, even mid-dispatch), commands, queued tasks and queued `Invoke` calls (dropped, `done` not called). Final: from the moment it starts, every `Post`, `Subscribe`, `RegisterCommand` and `Invoke` naming that owner returns `BadArg`, from any thread, so afterwards the runtime never calls the owner's functions again. Game thread only; `BadArg` for `nullptr` or an owner already released; `TooMany` only when out of memory (nothing removed; retry). No limit on released owners |
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
};
```

| Function | Does |
|---|---|
| `bool app::Start(const Platform& pf)` | `SetGameThread`; with `image`, `RegisterGameSignatures` and `ResolveAll(*image)`; `setCapabilities()`; `host::BuildApi`; the list (every built-in, then `Discover(pluginRoot)` when `pluginsEnabled`); `ContainCallouts`; `LoadBuiltin` for the built-ins, then `LoadNative` / `LoadScript` in list order (a `lua` plugin is refused `no script runtime` without `scripts`); the content index; with `image`, `LogSignatureReport(false)`; `LogReport`; dispatches `game.ready`. Problems are logged, never fatal. False (nothing changes) when already started |
| `void app::Tick(uint32_t nowMs)` | `GameThreadTick(nowMs)`. No-op unless started |
| `void app::Stop()` | Dispatches `game.exit`, then `UnloadAll` (newest first, built-ins last) and `ContainCallouts(nullptr)`. No-op unless started; `Start` works again afterwards, with fresh handles |
| `const std::vector<plugins::Plugin>& app::Plugins()` | The list of the last `Start` (built-ins first), final states after `Stop`. Never resized between two `Start`s |
| `const plugins::ContentIndex& app::Content()` | The data-pack index of the last `Start` |

`Platform` is copied; `builtins`, `scripts` and `hostVersion` must outlive `Stop`.

Call `Stop` from the game's own quit path, on the game thread. The game's menu Quit calls `CSystem::Quit` (`Quit via console command`), then `System Fast Shutdown (ExitOnQuit enabled)`: the process ends without the message loop ever getting `WM_QUIT`, so a `WM_QUIT` hook never runs `Stop` and plugins never see `game.exit`. sc-offline hooks `CSystem::Quit` through the `system.quit` signature row (`sco/game/system.h`, `QuitFunction`: the entry and the 15 prologue bytes a detour may overwrite). Never call `Stop` from `DLL_PROCESS_DETACH`: it runs under the loader lock, at the wrong time. Even from the quit path, `game.exit` is best effort: a crash or a killed process never sends it. Start the host kit regardless of whether any one feature resolved. Why: [framework plan, Lessons](framework.md#lessons). Storage and services ([framework plan, Phase 5](framework.md#phase-5-services-and-storage)) will be further `Platform` fields with defaults.

## `sco/pe_file.h`: host tools only

```cpp
struct FileImage {
    std::vector<uint8_t> mem;  Image img;  uint64_t preferredBase;  std::string error;
    bool Load(const std::string& path);
    uint32_t Rva(const void* p) const;
};
```

`Load` reads a 64-bit PE file and copies each section to its RVA in `mem`, so `img` looks like the loaded game. On failure it returns false and sets `error` (`not a PE file`, `not a 64-bit PE image`, `no .text or .rdata section`, ...). `Rva(p)` turns a pointer into `mem` back into an offset. Not compiled into the game DLL.
