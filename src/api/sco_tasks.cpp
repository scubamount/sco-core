// Game-thread identity and the bounded task queue.
#include "sco/runtime.h"
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

// Fixed ring: posting never allocates, so it is safe from any thread at any time.
struct Task { TaskFn fn; void* ctx; };
static std::mutex g_queueLock;
static Task       g_ring[kMaxQueuedTasks];
static size_t     g_head = 0, g_count = 0;

Result Post(TaskFn fn, void* ctx) {
    if (!fn) return Result::BadArg;
    std::lock_guard<std::mutex> hold(g_queueLock);
    if (g_count == kMaxQueuedTasks) return Result::TooMany;
    g_ring[(g_head + g_count) % kMaxQueuedTasks] = { fn, ctx };
    ++g_count;
    return Result::Ok;
}

size_t QueuedTasks() {
    std::lock_guard<std::mutex> hold(g_queueLock);
    return g_count;
}

size_t DrainTasks() {
    if (!OnGameThread()) return 0;
    size_t todo;
    {
        std::lock_guard<std::mutex> hold(g_queueLock);
        todo = g_count;   // tasks posted while draining wait for the next drain
    }
    size_t ran = 0;
    for (; ran < todo; ++ran) {
        Task t;
        {
            std::lock_guard<std::mutex> hold(g_queueLock);
            t = g_ring[g_head];
            g_head = (g_head + 1) % kMaxQueuedTasks;
            --g_count;
        }
        t.fn(t.ctx);   // outside the lock: a task may Post()
    }
    return ran;
}

Result GameThreadTick(uint32_t nowMs) {
    if (!OnGameThread()) return Result::WrongThread;
    DrainTasks();
    return Dispatch("tick", &nowMs);
}

}  // namespace sco
