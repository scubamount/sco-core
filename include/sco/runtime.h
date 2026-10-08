#pragma once
// Game-thread runtime: the task queue, the event bus and the command registry.
// C++ and internal. The plain-C sco_api.h (SDK step 4) is a thin layer over it: results share
// their numbers with sco_result, and Arg / ArgDef share their layout with sco_arg /
// sco_arg_def (static_asserts in src/api/sco_commands.cpp). Command is NOT sco_command: the
// host builds sco_command views of registered commands for list_commands in step 4.
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
    Ok = 0, Unavailable = 1, NotFound = 2, BadArg = 3, Crashed = 4, WrongThread = 5, TooMany = 6
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
// TooMany when kMaxReleasedOwners have been released, or out of memory (nothing removed).
// `removed` (optional) gets the number of items removed.
constexpr size_t kMaxReleasedOwners = 64;
Result Release(const void* owner, size_t* removed = nullptr);

// ---- task queue -----------------------------------------------------------------------------

using TaskFn = void (*)(void* ctx);
constexpr size_t kMaxQueuedTasks = 256;

// Queues fn(ctx) for the game thread. Tasks run in the order they were posted.
// TooMany when kMaxQueuedTasks are already waiting; BadArg when fn is null or owner released.
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

constexpr size_t   kMaxCommands = 512;    // registrations for the life of the process
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
// broken, a string too long, nargs > kMaxCommandArgs, an arg def with a null name or unknown
// type, a live command with the same name, or owner released.
// TooMany: kMaxCommands registrations used (released ones still count: slots never move), or
// out of memory.
Result RegisterCommand(const void* owner, const char* prefix, const Command& cmd);

// Writes up to max live commands (registration order) to out; returns the number live.
// Pointers and the strings they point at stay readable for the life of the process; a released
// command drops out of later lists.
size_t ListCommands(const Command** out, size_t max);

// Answers a command's `capability`. Until a check is installed (step 3), every command that
// names a capability is Unavailable.
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
// Results from the command run: NotFound (no such live command), BadArg (arg count or a type
// differs from the defs, a null string, a Bool not 0 or 1), Unavailable (capability check
// says no), else fn's result.
Result Invoke(const char* name, const Arg* args, uint32_t nargs, InvokeDone done, void* ctx,
              const void* owner = nullptr);

}  // namespace sco
