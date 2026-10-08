// Event bus. Subscribers live in an immutable list that Subscribe/Unsubscribe replace
// (copy on write); Dispatch takes a reference to the current list, so it never holds the lock
// while calling out and changes made during a dispatch apply from the next one.
#include "sco/runtime.h"
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sco {

struct Subscription {
    const void* owner;
    std::string event;
    EventFn fn;
    void* ctx;
};
using SubList = std::vector<Subscription>;

static std::mutex                     g_subLock;
static std::shared_ptr<const SubList> g_subs = std::make_shared<const SubList>();

static bool Same(const Subscription& s, const void* owner, const char* event, EventFn fn) {
    return s.owner == owner && s.fn == fn && s.event == event;
}

Result Subscribe(const void* owner, const char* event, EventFn fn, void* ctx) {
    if (!event || !*event || !fn) return Result::BadArg;
    std::lock_guard<std::mutex> hold(g_subLock);
    if (g_subs->size() >= kMaxSubscriptions) return Result::TooMany;
    for (const Subscription& s : *g_subs)
        if (Same(s, owner, event, fn)) return Result::BadArg;
    auto next = std::make_shared<SubList>(*g_subs);
    next->push_back({ owner, event, fn, ctx });
    g_subs = std::move(next);
    return Result::Ok;
}

Result Unsubscribe(const void* owner, const char* event, EventFn fn) {
    if (!event || !fn) return Result::BadArg;
    std::lock_guard<std::mutex> hold(g_subLock);
    for (size_t i = 0; i < g_subs->size(); ++i) {
        if (!Same((*g_subs)[i], owner, event, fn)) continue;
        auto next = std::make_shared<SubList>(*g_subs);
        next->erase(next->begin() + static_cast<std::ptrdiff_t>(i));
        g_subs = std::move(next);
        return Result::Ok;
    }
    return Result::NotFound;
}

size_t SubscriptionCount() {
    std::lock_guard<std::mutex> hold(g_subLock);
    return g_subs->size();
}

Result Dispatch(const char* event, const void* data, size_t* called) {
    if (called) *called = 0;
    if (!event) return Result::BadArg;
    if (!OnGameThread()) return Result::WrongThread;
    std::shared_ptr<const SubList> subs;
    {
        std::lock_guard<std::mutex> hold(g_subLock);
        subs = g_subs;
    }
    size_t n = 0;
    for (const Subscription& s : *subs) {
        if (s.event != event) continue;
        s.fn(event, data, s.ctx);
        ++n;
    }
    if (called) *called = n;
    return Result::Ok;
}

}  // namespace sco
