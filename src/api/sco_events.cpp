// Event bus. The subscriber list is immutable and replaced on every change (copy on write);
// Dispatch walks the list it started with, never holding the lock while calling out. Each
// subscription also has a live flag, cleared by Unsubscribe/Release, so a dispatch already
// walking an old list skips removed subscribers.
#include "sco/runtime.h"
#include "internal.h"
#include <atomic>
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
    std::atomic<bool> live{ true };
    Subscription(const void* o, const char* e, EventFn f, void* c) : owner(o), event(e), fn(f), ctx(c) {}
};
using SubList = std::vector<std::shared_ptr<Subscription>>;

static std::mutex                     g_subLock;
static std::shared_ptr<const SubList> g_subs = std::make_shared<const SubList>();

static bool Same(const Subscription& s, const void* owner, const char* event, EventFn fn) {
    return s.owner == owner && s.fn == fn && s.event == event;
}

Result Subscribe(const void* owner, const char* event, EventFn fn, void* ctx) {
    if (!event || !*event || !fn) return Result::BadArg;
    try {
        std::lock_guard<std::mutex> hold(g_subLock);
        if (detail::Released(owner)) return Result::BadArg;
        for (const auto& s : *g_subs)
            if (Same(*s, owner, event, fn)) return Result::BadArg;
        if (g_subs->size() >= kMaxSubscriptions) return Result::TooMany;
        auto next = std::make_shared<SubList>(*g_subs);
        next->push_back(std::make_shared<Subscription>(owner, event, fn, ctx));
        g_subs = std::move(next);
        return Result::Ok;
    } catch (...) {   // out of memory: nothing changed
        return Result::TooMany;
    }
}

Result Unsubscribe(const void* owner, const char* event, EventFn fn) {
    if (!event || !fn) return Result::BadArg;
    try {
        std::lock_guard<std::mutex> hold(g_subLock);
        for (size_t i = 0; i < g_subs->size(); ++i) {
            const auto& s = (*g_subs)[i];
            if (!Same(*s, owner, event, fn)) continue;
            auto next = std::make_shared<SubList>(*g_subs);
            next->erase(next->begin() + static_cast<std::ptrdiff_t>(i));
            s->live.store(false);   // a dispatch walking the old list skips it from now on
            g_subs = std::move(next);
            return Result::Ok;
        }
        return Result::NotFound;
    } catch (...) {
        return Result::TooMany;
    }
}

long detail::ReleaseSubscriptions(const void* owner) {
    try {
        std::lock_guard<std::mutex> hold(g_subLock);
        auto next = std::make_shared<SubList>();
        next->reserve(g_subs->size());
        for (const auto& s : *g_subs)
            if (s->owner != owner) next->push_back(s);
        const long removed = static_cast<long>(g_subs->size() - next->size());
        for (const auto& s : *g_subs)
            if (s->owner == owner) s->live.store(false);
        g_subs = std::move(next);
        return removed;
    } catch (...) {
        return -1;
    }
}

size_t SubscriptionCount() {
    std::lock_guard<std::mutex> hold(g_subLock);
    return g_subs->size();
}

struct EventCall { const Subscription* sub; const char* event; const void* data; };
static void EventThunk(void* c) {
    const EventCall* e = static_cast<const EventCall*>(c);
    e->sub->fn(e->event, e->data, e->sub->ctx);
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
    for (const auto& s : *subs) {
        if (s->event != event || !s->live.load()) continue;
        EventCall call{ s.get(), event, data };
        detail::Callout(s->owner, event, EventThunk, &call);
        ++n;
    }
    if (called) *called = n;
    return Result::Ok;
}

}  // namespace sco
