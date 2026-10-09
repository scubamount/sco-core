// Game-thread identity, callout depth, the bounded task queue and Release().
#include "sco/runtime.h"
#include "internal.h"
#include <atomic>
#include <iterator>
#include <list>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace sco {

const char* ResultName(Result r) {
    switch (r) {
    case Result::Ok:          return "OK";
    case Result::Unavailable: return "UNAVAILABLE";
    case Result::NotFound:    return "NOT_FOUND";
    case Result::BadArg:      return "BAD_ARG";
    case Result::Crashed:     return "CRASHED";
    case Result::WrongThread: return "WRONG_THREAD";
    case Result::TooMany:     return "TOO_MANY";
    case Result::Failed:      return "FAILED";
    }
    return "?";
}

static std::atomic<std::thread::id> g_gameThread{};   // default id = no thread

void SetGameThread() { g_gameThread.store(std::this_thread::get_id()); }

bool OnGameThread() {
    const std::thread::id id = g_gameThread.load();
    return id != std::thread::id() && id == std::this_thread::get_id();
}

namespace detail {
static int g_callDepth = 0;   // touched by the game thread only
bool InCallout() { return g_callDepth > 0; }
CallScope::CallScope() { ++g_callDepth; }
CallScope::~CallScope() { --g_callDepth; }
}  // namespace detail

static std::atomic<CalloutGuard> g_calloutGuard{ nullptr };

void SetCalloutGuard(CalloutGuard guard) { g_calloutGuard.store(guard); }

bool detail::Callout(const void* owner, const char* where, TaskFn thunk, void* ctx) {
    detail::CallScope scope;
    const CalloutGuard guard = owner ? g_calloutGuard.load() : nullptr;
    if (guard) return guard(owner, where, thunk, ctx);
    thunk(ctx);
    return true;
}

// Owners that Release() has started on; grows for the life of the process (one pointer per
// released plugin). Lock order is module lock -> g_ownerLock; Release takes g_ownerLock alone.
static std::mutex               g_ownerLock;
static std::vector<const void*> g_released;

bool detail::Released(const void* owner) {
    if (!owner) return false;
    std::lock_guard<std::mutex> hold(g_ownerLock);
    for (const void* r : g_released)
        if (r == owner) return true;
    return false;
}

// Two stages, both under g_queueLock, so posting is safe from any thread at any time:
//   - a fixed ring of kMaxQueuedTasks: the fast path, posting never allocates;
//   - an overflow list, used only while the ring is full or the list is non-empty (so the ring
//     always holds older tasks than the list and FIFO holds across both). Posting there allocates
//     one node; out of memory, or kMaxQueuedTasksHard tasks waiting in all, is TooMany.
// A list rather than a deque: Release splices an owner's nodes out without allocating, so
// dropping tasks can't fail. Each drain moves what fits back into the ring, so once the burst is
// over posting is allocation-free again.
// guarded: a task from Post(), run through the callout guard; runtime-internal tasks aren't.
struct Task { TaskFn fn; TaskFn drop; void* ctx; const void* owner; uint64_t seq; bool guarded; };
static std::mutex      g_queueLock;
static Task            g_ring[kMaxQueuedTasks];
static size_t          g_head = 0, g_count = 0;
static std::list<Task> g_overflow;   // posted after everything in the ring
static uint64_t        g_nextSeq = 0;   // post order; a drain runs only tasks posted before it began

static Result Push(TaskFn fn, TaskFn drop, void* ctx, const void* owner, bool guarded) {
    if (!fn) return Result::BadArg;
    std::lock_guard<std::mutex> hold(g_queueLock);
    if (detail::Released(owner)) return Result::BadArg;
    const Task t{ fn, drop, ctx, owner, g_nextSeq, guarded };
    if (g_count < kMaxQueuedTasks && g_overflow.empty()) {
        g_ring[(g_head + g_count) % kMaxQueuedTasks] = t;
        ++g_count;
    } else {
        if (g_count + g_overflow.size() >= kMaxQueuedTasksHard) return Result::TooMany;
        try {
            g_overflow.push_back(t);
        } catch (const std::bad_alloc&) {   // nothing queued
            return Result::TooMany;
        }
    }
    ++g_nextSeq;
    return Result::Ok;
}

// Refills the ring from the front of the overflow list (keeps order: the ring is older).
// Caller holds g_queueLock.
static void Refill() {
    while (!g_overflow.empty() && g_count < kMaxQueuedTasks) {
        g_ring[(g_head + g_count) % kMaxQueuedTasks] = g_overflow.front();
        ++g_count;
        g_overflow.pop_front();
    }
}

Result detail::PostOwned(TaskFn fn, TaskFn drop, void* ctx, const void* owner) {
    return Push(fn, drop, ctx, owner, false);
}

Result Post(TaskFn fn, void* ctx, const void* owner) { return Push(fn, nullptr, ctx, owner, true); }

size_t QueuedTasks() {
    std::lock_guard<std::mutex> hold(g_queueLock);
    return g_count + g_overflow.size();
}

size_t DrainTasks() {
    // Never nested: a drain inside a callout would run tasks while an outer dispatch is still on
    // the stack, which breaks the "post a task, free ctx there" rule in runtime.h. It would
    // also pop tasks the outer drain counted.
    if (!OnGameThread() || detail::InCallout()) return 0;
    uint64_t end;
    {
        std::lock_guard<std::mutex> hold(g_queueLock);
        end = g_nextSeq;   // tasks posted while draining wait for the next drain
    }
    size_t ran = 0;
    for (;;) {
        Task t;
        {
            std::lock_guard<std::mutex> hold(g_queueLock);
            // Empty, or only newer tasks left (a task may Release() others mid-drain). The ring
            // is older than the overflow list; when the ring empties, refill it from the list.
            if (g_count == 0) Refill();
            if (g_count == 0 || g_ring[g_head].seq >= end) {
                Refill();   // what's left waits in the ring when it fits
                break;
            }
            t = g_ring[g_head];
            g_head = (g_head + 1) % kMaxQueuedTasks;
            --g_count;
        }
        // Outside the lock: a task may Post().
        if (t.guarded) {
            detail::Callout(t.owner, "task", t.fn, t.ctx);
        } else {
            detail::CallScope scope;
            t.fn(t.ctx);
        }
        ++ran;
    }
    return ran;
}

long detail::ReleaseTasks(const void* owner) {
    // Never allocates: ring entries are copied to a fixed array, overflow nodes are spliced.
    Task dropped[kMaxQueuedTasks];
    size_t nDropped = 0;
    std::list<Task> droppedOverflow;
    {
        std::lock_guard<std::mutex> hold(g_queueLock);
        size_t kept = 0;
        for (size_t i = 0; i < g_count; ++i) {
            const Task t = g_ring[(g_head + i) % kMaxQueuedTasks];
            if (t.owner == owner) dropped[nDropped++] = t;
            else g_ring[(g_head + kept++) % kMaxQueuedTasks] = t;   // keeps order
        }
        g_count = kept;
        for (auto it = g_overflow.begin(); it != g_overflow.end();) {
            const auto next = std::next(it);
            if (it->owner == owner) droppedOverflow.splice(droppedOverflow.end(), g_overflow, it);
            it = next;
        }
        Refill();
    }
    // Outside the lock, in posting order: a drop callback may Post().
    for (size_t i = 0; i < nDropped; ++i)
        if (dropped[i].drop) dropped[i].drop(dropped[i].ctx);
    for (const Task& t : droppedOverflow)
        if (t.drop) t.drop(t.ctx);
    return static_cast<long>(nDropped + droppedOverflow.size());
}

static std::mutex  g_hookLock;
static ReleaseHook g_hooks[kMaxReleaseHooks];
static size_t      g_hookCount = 0;

Result AddReleaseHook(ReleaseHook hook) {
    if (!hook) return Result::BadArg;
    std::lock_guard<std::mutex> hold(g_hookLock);
    for (size_t i = 0; i < g_hookCount; ++i)
        if (g_hooks[i] == hook) return Result::Ok;
    if (g_hookCount == kMaxReleaseHooks) return Result::TooMany;
    g_hooks[g_hookCount++] = hook;
    return Result::Ok;
}

Result RemoveReleaseHook(ReleaseHook hook) {
    std::lock_guard<std::mutex> hold(g_hookLock);
    for (size_t i = 0; i < g_hookCount; ++i)
        if (g_hooks[i] == hook) {
            for (size_t j = i + 1; j < g_hookCount; ++j) g_hooks[j - 1] = g_hooks[j];
            --g_hookCount;
            return Result::Ok;
        }
    return Result::NotFound;
}

Result Release(const void* owner, size_t* removed) {
    if (removed) *removed = 0;
    if (!owner) return Result::BadArg;
    if (!OnGameThread()) return Result::WrongThread;
    {
        // Mark first: an add that hasn't taken its module lock yet will see the mark; one that
        // already has finishes before the removal below takes the same lock, and is removed.
        std::lock_guard<std::mutex> hold(g_ownerLock);
        for (const void* r : g_released)
            if (r == owner) return Result::BadArg;
        try {
            g_released.push_back(owner);
        } catch (...) {   // out of memory: nothing marked, nothing removed
            return Result::TooMany;
        }
    }
    // Subscriptions first: they are the only part a running dispatch can still reach. Out of
    // memory here leaves them in place, so undo the mark and let the caller retry.
    const long subs = detail::ReleaseSubscriptions(owner);
    if (subs < 0) {
        std::lock_guard<std::mutex> hold(g_ownerLock);
        g_released.pop_back();   // only the game thread appends, so the last entry is ours
        return Result::TooMany;
    }
    const long cmds = detail::ReleaseCommands(owner);
    const long tasks = detail::ReleaseTasks(owner);
    const long services = detail::ReleaseServices(owner);
    const long raw = detail::ReleaseRaw(owner);
    if (removed) *removed = static_cast<size_t>(subs + cmds + tasks + services + raw);
    // Hooks run outside g_hookLock (a hook may add or remove hooks), from a copy.
    ReleaseHook hooks[kMaxReleaseHooks];
    size_t nHooks;
    {
        std::lock_guard<std::mutex> hold(g_hookLock);
        nHooks = g_hookCount;
        for (size_t i = 0; i < nHooks; ++i) hooks[i] = g_hooks[i];
    }
    for (size_t i = 0; i < nHooks; ++i) hooks[i](owner);
    return Result::Ok;
}

Result GameThreadTick(uint32_t nowMs) {
    if (!OnGameThread() || detail::InCallout()) return Result::WrongThread;
    DrainTasks();
    return Dispatch("tick", &nowMs);
}

}  // namespace sco
