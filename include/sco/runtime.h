#pragma once
// Game-thread runtime: the task queue, the event bus and the command registry.
// C++ and internal. The plain-C sco_api.h (filled by sco/host.h) is a thin layer over it: results share
// their numbers with sco_result, and Arg / ArgDef share their layout with sco_arg /
// sco_arg_def (static_asserts in src/api/sco_commands.cpp). Command is NOT sco_command: the
// host table (sco/host.h) builds sco_command views of registered commands for list_commands.
//
// Threading model:
//   - The host calls SetGameThread() once from the game's main thread, then GameThreadTick()
//     on every main-thread tick (sc-offline: OnMainThreadTick in the WH_GETMESSAGE hook).
//   - Post(), Subscribe(), Unsubscribe(), RegisterCommand(), ListCommands() and Invoke() are
//     callable from any thread.
//   - Tasks, event callbacks and commands always run on the game thread. While one runs, the
//     game thread is "in a callout": GameThreadTick and DrainTasks refuse to nest.
//   - Release() is game thread only; it may run inside a callout.
//
// Owners: every subscription, command and task carries an owner, an opaque handle that is never
// dereferenced (a plugin's sco_plugin*, or a host feature's own static). nullptr is the default
// owner. Release(owner) removes everything an owner left behind.
#include <cstddef>
#include <cstdint>

namespace sco {

// Values match sco_result in sco_api.h.
enum class Result : uint32_t {
    Ok = 0, Unavailable = 1, NotFound = 2, BadArg = 3, Crashed = 4, WrongThread = 5, TooMany = 6,
    Failed = 7,   // ran, but couldn't do its job; the reply says why
};
const char* ResultName(Result r);   // "OK", "UNAVAILABLE", ...

// ---- game thread ----------------------------------------------------------------------------

void SetGameThread();               // call once, on the game's main thread
bool OnGameThread();                // false until SetGameThread() has run

// Drains the task queue, then dispatches "tick" with data = &nowMs.
// WrongThread (and nothing runs) off the game thread or from inside a callout.
Result GameThreadTick(uint32_t nowMs);

// Removes everything `owner` registered: its subscriptions (at once, even from inside a running
// dispatch), its commands, and its queued tasks and queued off-thread Invoke() calls (dropped;
// their done is never called). Release is final: from the moment it starts, every Post,
// Subscribe, RegisterCommand and Invoke naming that owner returns BadArg, from any thread, so
// after Release returns the runtime never calls the owner's functions or touches its ctx
// pointers again. The owner handle stays released for the life of the process; don't reuse
// its address (the host keeps plugin handles alive).
// Game thread only (WrongThread otherwise). BadArg for nullptr or an owner already released;
// TooMany when out of memory (nothing removed; retry). The released-owner list has no limit.
// `removed` (optional) gets the number of items removed.
Result Release(const void* owner, size_t* removed = nullptr);

// Host modules that keep per-owner state outside the runtime (sco::storage: a plugin's open
// database) learn of a Release through a hook: once Release(owner) has removed the owner's
// items, it calls every hook with owner, on the game thread, in the order they were added. A
// hook must not call Release. At most kMaxReleaseHooks; adding one already added is Ok and
// changes nothing. AddReleaseHook: BadArg for null, TooMany when full. RemoveReleaseHook:
// NotFound when not added. Any thread; a Release already running may still call a hook being
// removed.
using ReleaseHook = void (*)(const void* owner);
constexpr size_t kMaxReleaseHooks = 8;
Result AddReleaseHook(ReleaseHook hook);
Result RemoveReleaseHook(ReleaseHook hook);

// ---- services (any thread) -------------------------------------------------------------------
//
// A service is a function table one owner publishes under a name for others to call directly
// (sco_api 1.1 provide_service / query_service). The runtime only keeps the name, version and
// table pointer; it never calls into the table. Release(owner) withdraws the owner's services.
// A caller that holds a table must stop using it when its provider unloads: query it when
// needed rather than caching it across ticks (built-in plugins unload after every other plugin,
// so a table from a built-in stays valid for the life of any plugin that queried it).
// A native plugin whose table was handed out is never unmapped by the plugin loader, even after it
// unloads or crashes (ServiceTableHandedOut): a caller that kept the table jumps into code that is
// still there, running against the provider's torn-down state, which is the provider's to guard.
// A fault inside such a call is blamed on the provider, not on the caller (sco/plugins.h).

constexpr size_t kMaxServiceNameLen = 63;

// Publishes `table` under `name` for owner. name: [a-z0-9_.], 1-63 characters, no leading,
// trailing or doubled '.'; with a non-null prefix it must equal the prefix or start with
// "<prefix>." (a plugin publishes under its own id). version: (major << 16) | minor.
// BadArg: null owner or table, a bad name, a name already published, or owner released.
// TooMany: out of memory.
Result ProvideService(const void* owner, const char* prefix, const char* name, uint32_t version,
                      const void* table);

// True when a service by that name is published, whatever its version. Hands nothing out.
bool ServiceExists(const char* name);

// Finds a published service. Ok: *out = its table. NotFound: no service by that name.
// Unavailable: its major version differs from minVersion's, or it is older than minVersion.
// BadArg: null name or out. TooMany: out of memory (nothing handed out). *out is null unless Ok.
// Ok marks the provider as handed out, for the rest of the process (see above).
Result QueryService(const char* name, uint32_t minVersion, const void** out);

// True once QueryService has returned a table of `owner`'s, even if that service has been
// withdrawn since. Final once Release(owner) has returned: nothing of owner's can be queried
// after that. Any thread.
bool ServiceTableHandedOut(const void* owner);

// Withdraws one of owner's services. NotFound: owner publishes nothing by that name.
// BadArg: null owner or name.
Result ReleaseService(const void* owner, const char* name);

// ---- raw calls (game thread) ------------------------------------------------------------------
//
// A raw handler takes and returns bytes, for calls whose data doesn't fit typed arguments and a
// 256-byte reply (sco_api 1.1 register_raw / invoke_raw). The bytes' layout is the provider's
// contract, like a service table's.

// fn(in, inSize, out, outSize, ctx): *outSize holds out's capacity on entry; the handler sets it
// to the bytes it wrote, or to the bytes it needs and answers TooMany.
using RawFn = Result (*)(const void* in, uint32_t inSize, void* out, uint32_t* outSize, void* ctx);
constexpr size_t kMaxRawHandlers = 256;   // live handlers

// Registers fn under name ("<x>.<y>", the command name rule). With a non-null prefix the name
// must start with "<prefix>.". capability: as for commands; nullptr for none. Only fn and ctx are
// borrowed, until Release(owner). BadArg: null owner or fn, a bad name, capability or prefix, a
// name already registered, or owner released. TooMany: kMaxRawHandlers live, or out of memory.
// Any thread.
Result RegisterRaw(const void* owner, const char* prefix, const char* name, const char* capability,
                   RawFn fn, void* ctx);

// Calls a raw handler now, on the game thread, as one callout of its owner (a fault is Crashed
// and marks the owner, through the callout guard). in may be null only with inSize 0; out may be
// null only with outSize null (no output wanted). Returns the handler's result, with *outSize
// the bytes written or (TooMany) needed. NotFound: no handler by that name. Unavailable: its
// capability is missing. WrongThread: not the game thread. BadArg: null name, a bad buffer pair,
// or caller released. Crashed: the handler faulted (*outSize 0).
Result InvokeRaw(const void* caller, const char* name, const void* in, uint32_t inSize, void* out,
                 uint32_t* outSize);

// ---- task queue -----------------------------------------------------------------------------

using TaskFn = void (*)(void* ctx);
// The queue is a fixed ring of kMaxQueuedTasks (posting into it never allocates) backed by an
// overflow list that takes posts while the ring is full. kMaxQueuedTasksHard caps the total
// waiting, so a plugin posting in a loop can't eat the process's memory.
constexpr size_t kMaxQueuedTasks = 256;          // ring size
constexpr size_t kMaxQueuedTasksHard = 65536;    // ring + overflow

// Queues fn(ctx) for the game thread. Tasks run in the order they were posted, across the ring
// and the overflow. Any thread, at any time. TooMany when kMaxQueuedTasksHard tasks are already
// waiting or memory runs out (nothing queued); BadArg when fn is null or owner released.
Result Post(TaskFn fn, void* ctx, const void* owner = nullptr);

// Runs every task that was queued when the call started; tasks posted while draining wait for
// the next drain. Returns the number run. Game thread only and never from inside a callout
// (0 and nothing runs otherwise).
size_t DrainTasks();
size_t QueuedTasks();

// ---- event bus ------------------------------------------------------------------------------

using EventFn = void (*)(const char* event, const void* data, void* ctx);
constexpr size_t kMaxSubscriptions = 512;

// A subscription is keyed by (owner, event, fn). The event name is copied.
//   Subscribe:   applies from the next Dispatch(). BadArg (null/empty event, null fn, the key
//                already subscribed, or owner released), TooMany (kMaxSubscriptions reached)
//   Unsubscribe: applies at once: a running dispatch won't call it again. NotFound when the key
//                isn't subscribed.
// Freeing ctx after Unsubscribe: on the game thread, as soon as Unsubscribe returns (from
// inside the callback itself too: the runtime doesn't touch ctx after fn returns). From another
// thread the game thread may be inside fn(ctx) right now; Post() a task after Unsubscribe
// returns and free ctx from that task. Tasks never run while a dispatch is on the stack.
Result Subscribe(const void* owner, const char* event, EventFn fn, void* ctx);
Result Unsubscribe(const void* owner, const char* event, EventFn fn);
size_t SubscriptionCount();

// Calls every subscriber of `event`, in subscription order. `called` gets the number called.
// Game thread only: WrongThread and nothing called otherwise.
Result Dispatch(const char* event, const void* data, size_t* called = nullptr);

// ---- command registry -----------------------------------------------------------------------

enum class ArgType : uint32_t { Int = 0, Float = 1, String = 2, Bool = 3 };

struct Arg {
    ArgType type;
    uint32_t pad_;
    union { int64_t i; double f; const char* s; } v;   // Bool uses i: 0 or 1
};

struct ArgDef { const char* name; ArgType type; const char* help; };

// reply: a short message for the player ("Spawned Cutlass Black"), NUL-terminated by the host.
using CommandFn = Result (*)(const Arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t replySize);
using InvokeDone = void (*)(Result r, const char* reply, void* ctx);

struct Command {
    const char* name;          // "<prefix>.<action>": "spawn.ship", "hello.wave"
    const char* title;         // menu label
    const char* help;          // nullptr = none
    const char* capability;    // checked with the capability check before fn runs; nullptr = always
    const ArgDef* args;
    uint32_t nargs;
    CommandFn fn;
    void* ctx;
};

constexpr size_t   kMaxCommands = 512;    // live registrations; a released one frees its slot
constexpr uint32_t kMaxCommandArgs = 16;
constexpr uint32_t kReplySize = 256;
// Longest copied strings, NUL excluded. Longer is BadArg.
constexpr size_t kMaxNameLen = 63, kMaxTitleLen = 63, kMaxHelpLen = 255, kMaxCapabilityLen = 63;
constexpr size_t kMaxArgNameLen = 31, kMaxArgHelpLen = 127;

// Prefixes no plugin may register under.
constexpr const char* kReservedPrefixes[] = { "sco", "host", "menu", "game" };

// Registers cmd. The registry copies name, title, help, capability and the arg defs (with their
// strings); only fn and ctx are borrowed, until Release(owner).
// prefix: a plugin's name, one segment with no '.'; the command name must then start with
// "<prefix>.", the prefix may not be reserved, and no other owner may hold live commands
// whose first segment is the prefix. nullptr for host features (any "<x>.<y>" name).
// BadArg: null fn, name not "<x>.<y>" (lowercase letters, digits, '_' and '.'), a prefix rule
// broken, a string too long, a capability that isn't a capability name (lowercase letters,
// digits and '_' segments joined by '.', as caps::Set requires), nargs > kMaxCommandArgs, an arg
// def with a null name or unknown type, a live command with the same name, or owner released.
// TooMany: kMaxCommands commands are live (Release(owner) frees its owner's slots for reuse), or
// out of memory.
Result RegisterCommand(const void* owner, const char* prefix, const Command& cmd);

// Writes up to max live commands (registration order) to out; returns the number live.
// A pointer and its strings describe their command until Release(its owner): after that the
// registry reuses the slot, so the memory stays mapped but may describe a later command. Copy
// what you need, or keep only the name, before releasing the owner.
size_t ListCommands(const Command** out, size_t max);

// Answers a command's `capability`. Until a check is installed (sco::host::BuildApi installs
// sco::caps::Has), every command that names a capability is Unavailable.
using CapabilityCheck = bool (*)(const char* capability);
void SetCapabilityCheck(CapabilityCheck check);

// Runs the named command on the game thread.
//   On the game thread: runs now; done (if set) is called once before Invoke returns, and
//   Invoke returns the same result.
//   Off the game thread: name and args are copied and queued as a task of `owner`; Invoke
//   returns Ok and done runs exactly once, on the game thread, with the result, unless
//   Release(owner) drops the queued call first. If Invoke returns anything other than Ok
//   (BadArg for a null name, too many args or a released owner, TooMany when the task queue is
//   full or memory runs out) done is never called.
//   Either way, done is not called if `owner` was released while the command ran (the command,
//   or something it called, ran Release(owner)): after Release the runtime never calls an
//   owner's functions again.
// Results from the command run: NotFound (no such live command), BadArg (arg count or a type
// differs from the defs, a null string, a Bool not 0 or 1), Unavailable (capability check
// says no), else fn's result.
Result Invoke(const char* name, const Arg* args, uint32_t nargs, InvokeDone done, void* ctx,
              const void* owner = nullptr);

// ---- crash containment ----------------------------------------------------------------------

// Runs one callout for `owner`: calls thunk(ctx) (or skips it) and returns true when the call
// completed, false when it did not (it faulted, or the owner may not be called any more).
using CalloutGuard = bool (*)(const void* owner, const char* where, TaskFn thunk, void* ctx);

// Installs the guard every owned callout runs through; nullptr (the default) calls straight
// through. When set, every task, event callback, command fn and Invoke done callback whose owner
// is non-null runs as guard(owner, where, thunk, ctx); nullptr-owner callouts never see it.
//   owner: the task's, the subscription's or the command's owner; for done, the owner passed to
//          Invoke
//   where: "task" for tasks, the event name for events, the command name for commands,
//          "invoke done" for done callbacks
// A false return means the callout did not complete: a command that faults answers Crashed (with
// an empty reply, and done still gets Crashed); nothing else is retried or reported.
// Install from the game thread while nothing runs (sco::plugins::ContainCallouts does).
void SetCalloutGuard(CalloutGuard guard);

}  // namespace sco
