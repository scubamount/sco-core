#pragma once
// Game-thread runtime: the task queue, the event bus and the command registry.
// C++ only and internal for now; sco_api.h (SDK step 4) wraps these one to one.
//
// Threading model:
//   - The host calls SetGameThread() once from the game's main thread, then GameThreadTick()
//     on every main-thread tick (sc-offline: OnMainThreadTick in the WH_GETMESSAGE hook).
//   - Post(), Subscribe(), Unsubscribe(), RegisterCommand(), ListCommands() and Invoke() are
//     callable from any thread.
//   - Tasks, event callbacks and commands always run on the game thread.
#include <cstddef>
#include <cstdint>

namespace sco {

// Values match sco_result in the planned sco_api.h, so step 4 can cast.
enum class Result : uint32_t {
    Ok = 0, Unavailable = 1, NotFound = 2, BadArg = 3, Crashed = 4, WrongThread = 5, TooMany = 6
};
const char* ResultName(Result r);   // "OK", "UNAVAILABLE", ...

// ---- game thread ----------------------------------------------------------------------------

void SetGameThread();               // call once, on the game's main thread
bool OnGameThread();                // false until SetGameThread() has run

// Drains the task queue, then dispatches "tick" with data = &nowMs.
// WrongThread (and nothing runs) when called off the game thread.
Result GameThreadTick(uint32_t nowMs);

// ---- task queue -----------------------------------------------------------------------------

using TaskFn = void (*)(void* ctx);
constexpr size_t kMaxQueuedTasks = 256;

// Queues fn(ctx) for the game thread. Tasks run in the order they were posted.
// TooMany when kMaxQueuedTasks are already waiting; BadArg when fn is null.
Result Post(TaskFn fn, void* ctx);

// Runs every task that was queued when the call started; tasks posted while draining wait for
// the next drain. Returns the number run. Game thread only (0 and nothing runs otherwise).
size_t DrainTasks();
size_t QueuedTasks();

// ---- event bus ------------------------------------------------------------------------------

using EventFn = void (*)(const char* event, const void* data, void* ctx);
constexpr size_t kMaxSubscriptions = 512;

// A subscription is keyed by (owner, event, fn). owner is the caller's opaque handle (a plugin,
// or a host feature's own static); it is never dereferenced. The event name is copied.
// Changes apply from the next Dispatch(): a dispatch in progress keeps its snapshot.
//   Subscribe:   BadArg (null/empty event or null fn, or the key already subscribed),
//                TooMany (kMaxSubscriptions reached)
//   Unsubscribe: NotFound when the key isn't subscribed
Result Subscribe(const void* owner, const char* event, EventFn fn, void* ctx);
Result Unsubscribe(const void* owner, const char* event, EventFn fn);
size_t SubscriptionCount();

// Calls every subscriber of `event`, in subscription order. Returns the number called.
// Game thread only: WrongThread and nothing called otherwise.
Result Dispatch(const char* event, const void* data, size_t* called = nullptr);

// ---- command registry -----------------------------------------------------------------------

enum class ArgType : uint32_t { Int = 0, Float = 1, String = 2, Bool = 3 };

struct Arg {
    ArgType type;
    uint32_t pad_;
    union { int64_t i; double f; const char* s; } v;   // Bool uses i (0 or 1)
};

struct ArgDef { const char* name; ArgType type; const char* help; };

// reply: a short message for the player ("Spawned Cutlass Black"), NUL-terminated by the host.
using CommandFn = Result (*)(const Arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t replySize);
using InvokeDone = void (*)(Result r, const char* reply, void* ctx);

struct Command {
    const char* name;          // "<owner>.<action>": "spawn.ship", "hello.wave"
    const char* title;         // menu label
    const char* help;
    const char* capability;    // checked with the capability check before fn runs; nullptr = always
    const ArgDef* args;
    uint32_t nargs;
    CommandFn fn;
    void* ctx;
};

constexpr size_t kMaxCommands = 512;
constexpr uint32_t kMaxCommandArgs = 16;
constexpr uint32_t kReplySize = 256;

// The registry copies the Command; the strings and the args array it points at must stay
// valid while registered (use statics).
// ownerName: a plugin's name, and the name must then start with "<ownerName>."; nullptr for
// host features (any "<x>.<y>" name).
// BadArg: null fn, name not "<x>.<y>" (lowercase letters, digits, '_' and '.'), wrong owner
// prefix, nargs > kMaxCommandArgs, or an arg def with a null name. BadArg also for a duplicate
// name. TooMany when kMaxCommands are registered.
Result RegisterCommand(const char* ownerName, const Command& cmd);

// Writes up to max pointers (registration order) to out; returns the total registered.
// Pointers stay valid for the life of the process.
size_t ListCommands(const Command** out, size_t max);

// Answers a command's `capability`. Until a check is installed (step 3), every command that
// names a capability is Unavailable.
using CapabilityCheck = bool (*)(const char* capability);
void SetCapabilityCheck(CapabilityCheck check);

// Runs the named command on the game thread.
//   On the game thread: runs now; done (if set) is called once before Invoke returns, and
//   Invoke returns the same result.
//   Off the game thread: name and args are copied and queued; Invoke returns Ok and done runs
//   once on the game thread with the result. If Invoke returns anything else (BadArg for a null
//   name or too many args, TooMany when the task queue is full) done is never called.
// Results from the command run: NotFound (no such name), BadArg (arg count or a type differs
// from the defs, or a null string), Unavailable (capability check says no), else fn's result.
Result Invoke(const char* name, const Arg* args, uint32_t nargs, InvokeDone done, void* ctx);

}  // namespace sco
