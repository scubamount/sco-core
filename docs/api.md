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

Rows today:

| Id | How it's found |
|---|---|
| `teleport.to_camera` | The function that references the `CmdTeleportToCamera` string, with 19 layout checks plus entity-position and zone vtable checks |
| `teleport.client_mgr` | The global read at `to_camera+0x24` |
| `teleport.handle_from_id` | The function called at `to_camera+0x9F` |
| `teleport.entity_system` | The global read at `to_camera+0x2B0` |

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

## `sco/pe_file.h`: host tools only

```cpp
struct FileImage {
    std::vector<uint8_t> mem;  Image img;  uint64_t preferredBase;  std::string error;
    bool Load(const std::string& path);
    uint32_t Rva(const void* p) const;
};
```

`Load` reads a 64-bit PE file and copies each section to its RVA in `mem`, so `img` looks like the loaded game. On failure it returns false and sets `error` (`not a PE file`, `not a 64-bit PE image`, `no .text or .rdata section`, ...). `Rva(p)` turns a pointer into `mem` back into an offset. Not compiled into the game DLL.
