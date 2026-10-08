#pragma once
// Private to src/api/. Callout depth (game thread only, so no lock) and the per-module halves
// of Release().
#include "sco/runtime.h"

namespace sco::detail {

bool InCallout();

// Marks a callout (a task, an event callback, a command, a done callback) for its lifetime.
struct CallScope {
    CallScope();
    ~CallScope();
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;
};

// True once Release(owner) has started. Checked under each module's own lock, in the same
// critical section that adds the item, so nothing an owner adds can outlive its Release.
bool Released(const void* owner);

// Queues fn(ctx); if the task is dropped by Release(owner) instead, drop(ctx) runs so the
// queued state can be freed. drop must not call back into the runtime.
Result PostOwned(TaskFn fn, TaskFn drop, void* ctx, const void* owner);

// Invoke() with one addition for callers whose done ctx is heap state (sco::host's C-ABI
// trampoline): when Release(owner) drops the queued off-thread call, dropCtx(ctx) runs so ctx
// can be freed. dropCtx must not call back into the runtime. Never called on the game-thread
// path or when Invoke returns anything but Ok.
Result InvokeOwned(const char* name, const Arg* args, uint32_t nargs, InvokeDone done, void* ctx,
                   const void* owner, TaskFn dropCtx);

// Each removes the owner's items and returns how many, or -1 when out of memory (nothing
// removed). Called by Release() on the game thread.
long ReleaseTasks(const void* owner);
long ReleaseSubscriptions(const void* owner);
long ReleaseCommands(const void* owner);

}  // namespace sco::detail
