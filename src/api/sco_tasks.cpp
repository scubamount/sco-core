// Game-thread identity, callout depth, the bounded task queue and Release().
#include "sco/runtime.h"
#include "internal.h"
#include <atomic>
#include <mutex>
#include <thread>

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

// Owners that Release() has started on. Append-only, published by count; lock order is
// module lock -> g_ownerLock (Release takes g_ownerLock alone).
static std::mutex  g_ownerLock;
static const void* g_released[kMaxReleasedOwners];
static size_t      g_releasedCount = 0;

bool detail::Released(const void* owner) {
    if (!owner) return false;
    std::lock_guard<std::mutex> hold(g_ownerLock);
    for (size_t i = 0; i < g_releasedCount; ++i)
        if (g_released[i] == owner) return true;
    return false;
}

// Fixed ring: posting never allocates, so it is safe from any thread at any time.
struct Task { TaskFn fn; TaskFn drop; void* ctx; const void* owner; uint64_t seq; };
static std::mutex g_queueLock;
static Task       g_ring[kMaxQueuedTasks];
static size_t     g_head = 0, g_count = 0;
static uint64_t   g_nextSeq = 0;   // post order; a drain runs only tasks posted before it began

Result detail::PostOwned(TaskFn fn, TaskFn drop, void* ctx, const void* owner) {
    if (!fn) return Result::BadArg;
    std::lock_guard<std::mutex> hold(g_queueLock);
    if (detail::Released(owner)) return Result::BadArg;
    if (g_count == kMaxQueuedTasks) return Result::TooMany;
    g_ring[(g_head + g_count) % kMaxQueuedTasks] = { fn, drop, ctx, owner, g_nextSeq++ };
    ++g_count;
    return Result::Ok;
}

Result Post(TaskFn fn, void* ctx, const void* owner) { return detail::PostOwned(fn, nullptr, ctx, owner); }

size_t QueuedTasks() {
    std::lock_guard<std::mutex> hold(g_queueLock);
    return g_count;
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
            // Empty, or only newer tasks left (a task may Release() others mid-drain).
            if (g_count == 0 || g_ring[g_head].seq >= end) break;
            t = g_ring[g_head];
            g_head = (g_head + 1) % kMaxQueuedTasks;
            --g_count;
        }
        detail::CallScope scope;
        t.fn(t.ctx);   // outside the lock: a task may Post()
        ++ran;
    }
    return ran;
}

long detail::ReleaseTasks(const void* owner) {
    Task dropped[kMaxQueuedTasks];
    size_t nDropped = 0;
    {
        std::lock_guard<std::mutex> hold(g_queueLock);
        size_t kept = 0;
        for (size_t i = 0; i < g_count; ++i) {
            const Task t = g_ring[(g_head + i) % kMaxQueuedTasks];
            if (t.owner == owner) dropped[nDropped++] = t;
            else g_ring[(g_head + kept++) % kMaxQueuedTasks] = t;   // keeps order
        }
        g_count = kept;
    }
    for (size_t i = 0; i < nDropped; ++i)
        if (dropped[i].drop) dropped[i].drop(dropped[i].ctx);
    return static_cast<long>(nDropped);
}

Result Release(const void* owner, size_t* removed) {
    if (removed) *removed = 0;
    if (!owner) return Result::BadArg;
    if (!OnGameThread()) return Result::WrongThread;
    {
        // Mark first: an add that hasn't taken its module lock yet will see the mark; one that
        // already has finishes before the removal below takes the same lock, and is removed.
        std::lock_guard<std::mutex> hold(g_ownerLock);
        for (size_t i = 0; i < g_releasedCount; ++i)
            if (g_released[i] == owner) return Result::BadArg;
        if (g_releasedCount == kMaxReleasedOwners) return Result::TooMany;
        g_released[g_releasedCount++] = owner;
    }
    // Subscriptions first: they are the only part a running dispatch can still reach. Out of
    // memory here leaves them in place, so undo the mark and let the caller retry.
    const long subs = detail::ReleaseSubscriptions(owner);
    if (subs < 0) {
        std::lock_guard<std::mutex> hold(g_ownerLock);
        --g_releasedCount;
        return Result::TooMany;
    }
    const long cmds = detail::ReleaseCommands(owner);
    const long tasks = detail::ReleaseTasks(owner);
    if (removed) *removed = static_cast<size_t>(subs + cmds + tasks);
    return Result::Ok;
}

Result GameThreadTick(uint32_t nowMs) {
    if (!OnGameThread() || detail::InCallout()) return Result::WrongThread;
    DrainTasks();
    return Dispatch("tick", &nowMs);
}

}  // namespace sco
