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

// Queues fn(ctx); if the task is dropped by Release(owner) instead, drop(ctx) runs so the
// queued state can be freed. drop must not call back into the runtime.
Result PostOwned(TaskFn fn, TaskFn drop, void* ctx, const void* owner);

// Each removes the owner's items and returns how many, or -1 when out of memory (nothing
// removed). Called by Release() on the game thread.
long ReleaseTasks(const void* owner);
long ReleaseSubscriptions(const void* owner);
long ReleaseCommands(const void* owner);

}  // namespace sco::detail
