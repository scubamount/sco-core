// scosdk/plugin.hpp: the C++20 layer of the sco SDK. Header-only, over sco_api.h.
//
//   #include "scosdk/plugin.hpp"
//
//   class Hello : public sco::sdk::Plugin {
//       sco::sdk::Subscription tick_;
//       sco_result OnLoad() override {
//           tick_ = Subscribe("tick", [this](const void*) { ++ticks_; });
//           Info("loaded on %s", Api()->host_version());
//           return tick_ ? SCO_OK : tick_.Result();
//       }
//       int ticks_ = 0;
//   };
//   SCO_PLUGIN(Hello, "hello", "1.0.0", "you");
//
// Reference: docs/sdk-cpp.md. Rules the layer keeps:
//   - Nothing throws across the C ABI. Every function here is noexcept; an exception that
//     escapes plugin code (OnLoad, a command, an event handler, a task, a raw handler) is caught
//     where the host called in, logged as "exception in <where>: <what>", and turned into
//     SCO_FAILED where the call returns a result.
//   - Every callback's ctx is an id, never a pointer the SDK frees: the callable it names lives
//     in a registry until the plugin unloads, so a call the host makes late (or never) can't touch
//     freed memory, and nothing the SDK owns leaks.
//   - Only the C++ standard library; no exceptions, RTTI or varargs cross the boundary.
//
// GPL-3.0, like sco-core and sco_api.h.
#ifndef SCOSDK_PLUGIN_HPP
#define SCOSDK_PLUGIN_HPP

#include "sco_api.h"

#include <atomic>
#include <concepts>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define SCOSDK_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define SCOSDK_PRINTF(fmt, args)
#endif

namespace sco::sdk {

class Plugin;
class Subscription;

// The service version format of sco_api 1.1: (major << 16) | minor.
constexpr uint32_t ServiceVersion(uint16_t major, uint16_t minor) noexcept {
    return (static_cast<uint32_t>(major) << 16) | minor;
}

// True when a host table built with `size` bytes has the member at `offset` (sizeof `bytes`).
constexpr bool Covers(uint32_t size, size_t offset, size_t bytes = sizeof(void*)) noexcept {
    return static_cast<size_t>(size) >= offset + bytes;
}

// ---- argument values ------------------------------------------------------------------------

// sco_arg builders for Invoke: MakeArg(3), MakeArg(2.5), MakeArg("Ada"), MakeArg(true).
template <std::integral T>
    requires(!std::same_as<T, bool>)
inline sco_arg MakeArg(T value) noexcept {
    sco_arg a{};
    a.type = SCO_ARG_INT;
    a.v.i = static_cast<int64_t>(value);
    return a;
}
inline sco_arg MakeArg(double value) noexcept {
    sco_arg a{};
    a.type = SCO_ARG_FLOAT;
    a.v.f = value;
    return a;
}
inline sco_arg MakeArg(const char* value) noexcept {
    sco_arg a{};
    a.type = SCO_ARG_STRING;
    a.v.s = value;
    return a;
}
inline sco_arg MakeArg(bool value) noexcept {
    sco_arg a{};
    a.type = SCO_ARG_BOOL;
    a.v.i = value ? 1 : 0;
    return a;
}

namespace detail {

// One plugin's SDK state. Shared with Subscription handles (weakly), so a handle that outlives
// its plugin finds `live` false and does nothing.
struct State {
    const sco_api* api = nullptr;
    sco_plugin*    self = nullptr;
    Plugin*        plugin = nullptr;
    std::mutex     m;
    bool           live = true;                                // guarded by m
    std::map<std::string, uint64_t, std::less<>> events;       // event -> its EventNode id; m
};

// A callable the host reaches through a trampoline. owner is compared, never dereferenced.
struct Node {
    virtual ~Node() = default;
    const State* owner = nullptr;
    Plugin*      plugin = nullptr;   // for logging; valid while the node is registered
};

// Every node, keyed by the id passed to the host as ctx. Process-wide (one per plugin binary)
// and never destroyed, so a trampoline running during process exit still finds a valid map.
class Registry {
public:
    uint64_t Add(std::shared_ptr<Node> node) {   // throws std::bad_alloc
        std::lock_guard<std::mutex> hold(m_);
        const uint64_t id = next_++;
        map_.emplace(id, std::move(node));
        return id;
    }
    std::shared_ptr<Node> Find(uint64_t id) noexcept {
        std::lock_guard<std::mutex> hold(m_);
        const auto it = map_.find(id);
        return it == map_.end() ? nullptr : it->second;
    }
    std::shared_ptr<Node> Take(uint64_t id) noexcept {
        std::lock_guard<std::mutex> hold(m_);
        const auto it = map_.find(id);
        if (it == map_.end()) return nullptr;
        std::shared_ptr<Node> n = std::move(it->second);
        map_.erase(it);
        return n;
    }
    void Remove(uint64_t id) noexcept { Take(id); }   // destroyed outside the lock
    // One at a time, so a node's destructor (user captures) runs with no SDK lock held and
    // nothing here allocates.
    void RemoveOwner(const State* owner) noexcept {
        for (;;) {
            std::shared_ptr<Node> n;
            {
                std::lock_guard<std::mutex> hold(m_);
                for (auto it = map_.begin(); it != map_.end(); ++it)
                    if (it->second->owner == owner) {
                        n = std::move(it->second);
                        map_.erase(it);
                        break;
                    }
            }
            if (!n) return;
        }
    }

private:
    std::mutex                                          m_;
    std::unordered_map<uint64_t, std::shared_ptr<Node>> map_;
    uint64_t                                            next_ = 1;
};

inline Registry& Nodes() noexcept {
    static Registry* const r = new Registry;   // never freed: see Registry
    return *r;
}

inline void* ToCtx(uint64_t id) noexcept { return reinterpret_cast<void*>(static_cast<uintptr_t>(id)); }
inline uint64_t FromCtx(void* ctx) noexcept { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ctx)); }

struct Handler {
    std::function<void(const void*)> fn;
    std::atomic<bool>                live{ true };
};

// One host subscription per (plugin, event); the handlers behind it are the SDK's. The host
// keys a subscription by (self, event, fn), so a second host subscription with the same
// trampoline would be refused.
struct EventNode : Node {
    std::mutex                            m;
    std::vector<std::shared_ptr<Handler>> handlers;   // m
};

struct TaskNode : Node {
    std::function<void()> fn;
};

struct DoneNode : Node {
    std::function<void(sco_result, const char*)> fn;
};

struct Access;   // reaches Plugin's state for the other scosdk headers

// Registers node for state s, unless s's plugin is unloading. OK, BAD_ARG or TOO_MANY.
inline sco_result AddNode(State* s, std::shared_ptr<Node> node, uint64_t& id) noexcept {
    if (!s) return SCO_BAD_ARG;
    try {
        std::lock_guard<std::mutex> hold(s->m);
        if (!s->live) return SCO_BAD_ARG;
        node->owner = s;
        node->plugin = s->plugin;
        id = Nodes().Add(std::move(node));
        return SCO_OK;
    } catch (...) {
        return SCO_TOO_MANY;
    }
}

inline void ReportException(Plugin* p, const char* where, std::exception_ptr e) noexcept;
inline void EventTrampoline(const char* event, const void* data, void* ctx) noexcept;
inline void TaskTrampoline(void* ctx) noexcept;
inline void DoneTrampoline(sco_result r, const char* reply, void* ctx) noexcept;

template <class T>
class Slot;

}  // namespace detail

// ---- subscriptions --------------------------------------------------------------------------

// The handle Plugin::Subscribe returns. Move-only. Destroying it (or Reset) unsubscribes: from
// the game thread the handler never runs again once Reset returns; from another thread a call
// already running on the game thread may still finish (it holds its own reference to the
// handler, so nothing it uses is freed under it). A handle still alive when its plugin unloads
// is detached by the unload and then does nothing.
class Subscription {
public:
    Subscription() noexcept = default;
    explicit Subscription(sco_result failed) noexcept : result_(failed) {}
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& o) noexcept { Steal(o); }
    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) {
            Reset();
            Steal(o);
        }
        return *this;
    }
    ~Subscription() { Reset(); }

    // SCO_OK while subscribed; else why Subscribe failed (SCO_NOT_FOUND for an empty handle).
    sco_result Result() const noexcept { return result_; }
    explicit operator bool() const noexcept { return handler_ != nullptr; }
    void Reset() noexcept;

private:
    friend class Plugin;
    Subscription(std::weak_ptr<detail::State> s, std::string event, std::shared_ptr<detail::Handler> h) noexcept
        : state_(std::move(s)), event_(std::move(event)), handler_(std::move(h)), result_(SCO_OK) {}
    void Steal(Subscription& o) noexcept {
        state_ = std::move(o.state_);
        event_ = std::move(o.event_);
        handler_ = std::move(o.handler_);
        result_ = o.result_;
        o.result_ = SCO_NOT_FOUND;
    }

    std::weak_ptr<detail::State>     state_;
    std::string                      event_;
    std::shared_ptr<detail::Handler> handler_;
    sco_result                       result_ = SCO_NOT_FOUND;
};

// ---- the plugin -----------------------------------------------------------------------------

// Derive from Plugin, override OnLoad (and OnUnload if needed), and export it with SCO_PLUGIN.
// Api() and Self() are valid from OnLoad until OnUnload returns. Calls from other threads are
// fine where sco_api allows them (everything here except InvokeRaw), but no thread may call
// into the plugin once OnUnload has returned: stop your threads in OnUnload.
class Plugin {
public:
    Plugin() noexcept = default;
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;
    virtual ~Plugin() = default;

    // Called from sco_plugin_load, on the game thread. Anything but SCO_OK unloads the plugin
    // (OnUnload is not called; the SDK still releases what it registered).
    virtual sco_result OnLoad() { return SCO_OK; }
    // Called from sco_plugin_unload, on the game thread, before the SDK releases its state.
    virtual void OnUnload() {}

    const sco_api* Api() const noexcept { return api_; }
    sco_plugin* Self() const noexcept { return self_; }
    // True when the host's table has the member at `offset`: ApiCovers(offsetof(sco_api, register_raw)).
    bool ApiCovers(size_t offset) const noexcept { return api_ && Covers(api_->size, offset); }

    // Log and status lines: printf formats, formatted into 256 bytes (longer is cut). gcc and
    // clang check the format; pass other text as Info("%s", text).
    void Log(sco_log_level level, const char* fmt, ...) const noexcept SCOSDK_PRINTF(3, 4);
    void Info(const char* fmt, ...) const noexcept SCOSDK_PRINTF(2, 3);
    void Warn(const char* fmt, ...) const noexcept SCOSDK_PRINTF(2, 3);
    void Error(const char* fmt, ...) const noexcept SCOSDK_PRINTF(2, 3);
    void Status(const char* fmt, ...) const noexcept SCOSDK_PRINTF(2, 3);

    bool Has(const char* capability) const noexcept { return api_ && capability && api_->has(capability) != 0; }

    // Any thread. Runs task on the game thread at the next tick. The SDK owns the task and frees
    // it after it runs. A task still queued when the plugin unloads is dropped, never run: don't
    // rely on one to save state. SCO_TOO_MANY when the host's queue (256) is full.
    sco_result RunOnGameThread(std::function<void()> task) noexcept;

    // Any thread. Calls fn(data) for each dispatch of event, from the next dispatch on, until the
    // returned handle is destroyed or reset, or the plugin unloads. An empty handle (operator
    // bool false, Result() says why) if the host refused.
    [[nodiscard]] Subscription Subscribe(const char* event, std::function<void(const void*)> fn) noexcept;

    // Runs a command. From the game thread it runs now and *reply (if given) gets its reply.
    // From another thread the call is queued and returns SCO_OK; the reply is then lost (use
    // InvokeAsync).
    sco_result Invoke(const char* name, std::initializer_list<sco_arg> args = {}, std::string* reply = nullptr) noexcept {
        return Invoke(name, args.begin(), static_cast<uint32_t>(args.size()), reply);
    }
    sco_result Invoke(const char* name, const sco_arg* args, uint32_t nargs, std::string* reply = nullptr) noexcept;
    // Runs a command and calls done(result, reply) once, on the game thread (before returning
    // when called from it). Any return but SCO_OK means done is never called. A call still queued
    // when the plugin unloads is dropped and done is not called.
    sco_result InvokeAsync(const char* name, std::initializer_list<sco_arg> args,
                           std::function<void(sco_result, const char*)> done) noexcept;

private:
    friend struct detail::Access;
    friend sco_result LoadPlugin(Plugin& plugin, const sco_api* api, sco_plugin* self) noexcept;
    friend void UnloadPlugin(Plugin& plugin) noexcept;
    void VLog(sco_log_level level, const char* fmt, va_list ap) const noexcept;
    void Shutdown() noexcept;

    const sco_api*                 api_ = nullptr;
    sco_plugin*                    self_ = nullptr;
    std::shared_ptr<detail::State> state_;
};

// Runs plugin as the plugin behind (api, self): OnLoad behind the exception boundary. On any
// result but SCO_OK the SDK has already released what the plugin registered. SCO_PLUGIN calls
// this; call it yourself only to host several Plugin objects in one binary (tests).
sco_result LoadPlugin(Plugin& plugin, const sco_api* api, sco_plugin* self) noexcept;
// OnUnload, then releases everything the SDK holds for the plugin: event subscriptions, queued
// tasks and invokes (dropped), command and raw handlers. Game thread. The plugin object may be
// destroyed afterwards; the host then releases the plugin's registrations.
void UnloadPlugin(Plugin& plugin) noexcept;

namespace detail {

struct Access {
    static State* StateOf(const Plugin& p) noexcept { return p.state_.get(); }
};

inline void ReportException(Plugin* p, const char* where, std::exception_ptr e) noexcept {
    const char* what = "unknown exception";
    try {
        if (e) std::rethrow_exception(e);
    } catch (const std::exception& x) {
        what = x.what();
        if (p) p->Error("exception in %s: %s", where, what);
        return;
    } catch (...) {
    }
    if (p) p->Error("exception in %s: %s", where, what);
}

inline void EventTrampoline(const char* event, const void* data, void* ctx) noexcept {
    const std::shared_ptr<Node> n = Nodes().Find(FromCtx(ctx));
    if (!n) return;
    EventNode* ev = static_cast<EventNode*>(n.get());
    std::vector<std::shared_ptr<Handler>> now;
    try {
        std::lock_guard<std::mutex> hold(ev->m);
        now = ev->handlers;
    } catch (...) {
        if (ev->plugin) ev->plugin->Error("out of memory dispatching %s", event ? event : "?");
        return;
    }
    for (const std::shared_ptr<Handler>& h : now) {
        if (!h->live.load(std::memory_order_acquire)) continue;   // reset during this dispatch
        try {
            h->fn(data);
        } catch (...) {
            ReportException(ev->plugin, event ? event : "event", std::current_exception());
        }
    }
}

inline void TaskTrampoline(void* ctx) noexcept {
    const std::shared_ptr<Node> n = Nodes().Take(FromCtx(ctx));
    if (!n) return;   // dropped at unload
    TaskNode* t = static_cast<TaskNode*>(n.get());
    try {
        t->fn();
    } catch (...) {
        ReportException(t->plugin, "task", std::current_exception());
    }
}

inline void DoneTrampoline(sco_result r, const char* reply, void* ctx) noexcept {
    const std::shared_ptr<Node> n = Nodes().Take(FromCtx(ctx));
    if (!n) return;
    DoneNode* d = static_cast<DoneNode*>(n.get());
    try {
        d->fn(r, reply ? reply : "");
    } catch (...) {
        ReportException(d->plugin, "invoke done", std::current_exception());
    }
}

// The storage SCO_PLUGIN emits: the one plugin object of a DLL, created on load and destroyed
// after unload. Constant-initialized, no destructor.
template <class T>
class Slot {
public:
    static_assert(std::is_base_of_v<Plugin, T>, "SCO_PLUGIN: the class must derive from sco::sdk::Plugin");
    static_assert(std::is_default_constructible_v<T>, "SCO_PLUGIN: the class needs a default constructor");

    sco_result Load(const sco_api* api, sco_plugin* self) noexcept {
        if (plugin_ || !api || !self) return SCO_BAD_ARG;
        T* p = nullptr;
        try {
            p = new T();
        } catch (...) {
            const char* what = "unknown exception";
            try {
                throw;
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
            }
            char line[256];
            std::snprintf(line, sizeof line, "exception in constructor: %s", what);
            api->log(self, SCO_LOG_ERROR, line);
            return SCO_FAILED;
        }
        const sco_result r = LoadPlugin(*p, api, self);
        if (r != SCO_OK) {
            delete p;
            return r;
        }
        plugin_ = p;
        return SCO_OK;
    }

    void Unload() noexcept {
        if (!plugin_) return;
        UnloadPlugin(*plugin_);
        delete plugin_;
        plugin_ = nullptr;
    }

private:
    T* plugin_ = nullptr;
};

}  // namespace detail

// ---- definitions ----------------------------------------------------------------------------

inline void Subscription::Reset() noexcept {
    if (!handler_) return;
    handler_->live.store(false, std::memory_order_release);
    if (const std::shared_ptr<detail::State> s = state_.lock()) {
        std::lock_guard<std::mutex> hold(s->m);
        const auto it = s->live ? s->events.find(event_) : s->events.end();
        if (it != s->events.end()) {
            bool empty = false;
            if (const std::shared_ptr<detail::Node> n = detail::Nodes().Find(it->second)) {
                detail::EventNode* ev = static_cast<detail::EventNode*>(n.get());
                std::lock_guard<std::mutex> holdEv(ev->m);
                for (auto h = ev->handlers.begin(); h != ev->handlers.end(); ++h)
                    if (*h == handler_) {
                        ev->handlers.erase(h);
                        break;
                    }
                empty = ev->handlers.empty();
            }
            if (empty) {   // the last handler: drop the host subscription too
                s->api->unsubscribe(s->self, it->first.c_str(), detail::EventTrampoline);
                detail::Nodes().Remove(it->second);
                s->events.erase(it);
            }
        }
    }
    handler_.reset();
    state_.reset();
    event_.clear();
    result_ = SCO_NOT_FOUND;
}

inline void Plugin::VLog(sco_log_level level, const char* fmt, va_list ap) const noexcept {
    if (!api_ || !fmt) return;
    char line[256];
    line[0] = '\0';
    if (std::vsnprintf(line, sizeof line, fmt, ap) < 0) line[0] = '\0';
    line[sizeof line - 1] = '\0';
    api_->log(self_, level, line);
}

inline void Plugin::Log(sco_log_level level, const char* fmt, ...) const noexcept {
    va_list ap;
    va_start(ap, fmt);
    VLog(level, fmt, ap);
    va_end(ap);
}

inline void Plugin::Info(const char* fmt, ...) const noexcept {
    va_list ap;
    va_start(ap, fmt);
    VLog(SCO_LOG_INFO, fmt, ap);
    va_end(ap);
}

inline void Plugin::Warn(const char* fmt, ...) const noexcept {
    va_list ap;
    va_start(ap, fmt);
    VLog(SCO_LOG_WARN, fmt, ap);
    va_end(ap);
}

inline void Plugin::Error(const char* fmt, ...) const noexcept {
    va_list ap;
    va_start(ap, fmt);
    VLog(SCO_LOG_ERROR, fmt, ap);
    va_end(ap);
}

inline void Plugin::Status(const char* fmt, ...) const noexcept {
    if (!api_ || !fmt) return;
    char line[256];
    line[0] = '\0';
    va_list ap;
    va_start(ap, fmt);
    if (std::vsnprintf(line, sizeof line, fmt, ap) < 0) line[0] = '\0';
    va_end(ap);
    line[sizeof line - 1] = '\0';
    api_->status(self_, line);
}

inline sco_result Plugin::RunOnGameThread(std::function<void()> task) noexcept {
    if (!task) return SCO_BAD_ARG;
    std::shared_ptr<detail::TaskNode> node;
    try {
        node = std::make_shared<detail::TaskNode>();
        node->fn = std::move(task);
    } catch (...) {
        return SCO_TOO_MANY;
    }
    uint64_t id = 0;
    if (const sco_result added = detail::AddNode(state_.get(), node, id); added != SCO_OK) return added;
    const sco_result r = api_->run_on_game_thread(self_, detail::TaskTrampoline, detail::ToCtx(id));
    if (r != SCO_OK) detail::Nodes().Remove(id);   // not queued: never runs
    return r;
}

inline Subscription Plugin::Subscribe(const char* event, std::function<void(const void*)> fn) noexcept {
    detail::State* s = state_.get();
    if (!s || !event || !*event || !fn) return Subscription(SCO_BAD_ARG);
    try {
        auto h = std::make_shared<detail::Handler>();
        h->fn = std::move(fn);
        std::string name(event);
        std::lock_guard<std::mutex> hold(s->m);
        if (!s->live) return Subscription(SCO_BAD_ARG);
        const auto it = s->events.find(name);
        std::shared_ptr<detail::Node> found = it == s->events.end() ? nullptr : detail::Nodes().Find(it->second);
        if (found) {
            detail::EventNode* ev = static_cast<detail::EventNode*>(found.get());
            std::lock_guard<std::mutex> holdEv(ev->m);
            ev->handlers.push_back(h);
        } else {
            auto ev = std::make_shared<detail::EventNode>();
            ev->owner = s;
            ev->plugin = this;
            ev->handlers.push_back(h);
            auto slot = s->events.emplace(name, 0).first;   // reserve the map entry before subscribing
            uint64_t id = 0;
            try {
                id = detail::Nodes().Add(ev);
            } catch (...) {
                s->events.erase(slot);
                throw;
            }
            const sco_result r = api_->subscribe(self_, event, detail::EventTrampoline, detail::ToCtx(id));
            if (r != SCO_OK) {
                detail::Nodes().Remove(id);
                s->events.erase(slot);
                return Subscription(r);
            }
            slot->second = id;
        }
        return Subscription(state_, std::move(name), std::move(h));
    } catch (...) {
        return Subscription(SCO_TOO_MANY);
    }
}

inline sco_result Plugin::Invoke(const char* name, const sco_arg* args, uint32_t nargs, std::string* reply) noexcept {
    if (!api_) return SCO_BAD_ARG;
    if (!reply) return api_->invoke(self_, name, args, nargs, nullptr, nullptr);
    // The reply lands in a buffer shared with the done node, so a done that runs later (the call
    // was queued from another thread) writes somewhere valid.
    struct Box {
        std::string       text;
        std::atomic<bool> done{ false };
    };
    std::shared_ptr<Box> box;
    std::shared_ptr<detail::DoneNode> node;
    try {
        box = std::make_shared<Box>();
        node = std::make_shared<detail::DoneNode>();
        node->fn = [box](sco_result, const char* text) {
            box->text = text;
            box->done.store(true, std::memory_order_release);
        };
    } catch (...) {
        return SCO_TOO_MANY;
    }
    uint64_t id = 0;
    if (const sco_result added = detail::AddNode(state_.get(), std::move(node), id); added != SCO_OK) return added;
    const sco_result r = api_->invoke(self_, name, args, nargs, detail::DoneTrampoline, detail::ToCtx(id));
    if (r != SCO_OK) detail::Nodes().Remove(id);   // done ran already, or never will
    if (box->done.load(std::memory_order_acquire)) {
        try {
            *reply = box->text;
        } catch (...) {
            return SCO_TOO_MANY;
        }
    } else {
        reply->clear();
    }
    return r;
}

inline sco_result Plugin::InvokeAsync(const char* name, std::initializer_list<sco_arg> args,
                                      std::function<void(sco_result, const char*)> done) noexcept {
    if (!api_) return SCO_BAD_ARG;
    if (!done) return api_->invoke(self_, name, args.begin(), static_cast<uint32_t>(args.size()), nullptr, nullptr);
    std::shared_ptr<detail::DoneNode> node;
    try {
        node = std::make_shared<detail::DoneNode>();
        node->fn = std::move(done);
    } catch (...) {
        return SCO_TOO_MANY;
    }
    uint64_t id = 0;
    if (const sco_result added = detail::AddNode(state_.get(), std::move(node), id); added != SCO_OK) return added;
    const sco_result r = api_->invoke(self_, name, args.begin(), static_cast<uint32_t>(args.size()),
                                      detail::DoneTrampoline, detail::ToCtx(id));
    if (r != SCO_OK) detail::Nodes().Remove(id);
    return r;
}

// Game thread, from unload or a failed load: nothing the host calls runs concurrently.
inline void Plugin::Shutdown() noexcept {
    detail::State* s = state_.get();
    if (!s) return;
    std::map<std::string, uint64_t, std::less<>> events;
    {
        std::lock_guard<std::mutex> hold(s->m);
        s->live = false;
        events.swap(s->events);
    }
    for (const auto& entry : events) api_->unsubscribe(self_, entry.first.c_str(), detail::EventTrampoline);
    detail::Nodes().RemoveOwner(s);
    state_.reset();
}

inline sco_result LoadPlugin(Plugin& plugin, const sco_api* api, sco_plugin* self) noexcept {
    if (!api || !self || plugin.state_) return SCO_BAD_ARG;
    if (!Covers(api->size, offsetof(sco_api, list_commands))) return SCO_UNAVAILABLE;   // not a 1.0 table
    plugin.api_ = api;
    plugin.self_ = self;
    try {
        plugin.state_ = std::make_shared<detail::State>();
    } catch (...) {
        return SCO_TOO_MANY;
    }
    plugin.state_->api = api;
    plugin.state_->self = self;
    plugin.state_->plugin = &plugin;
    sco_result r = SCO_FAILED;
    try {
        r = plugin.OnLoad();
    } catch (...) {
        detail::ReportException(&plugin, "OnLoad", std::current_exception());
        r = SCO_FAILED;
    }
    if (r != SCO_OK) plugin.Shutdown();
    return r;
}

inline void UnloadPlugin(Plugin& plugin) noexcept {
    if (!plugin.state_) return;
    try {
        plugin.OnUnload();
    } catch (...) {
        detail::ReportException(&plugin, "OnUnload", std::current_exception());
    }
    plugin.Shutdown();
}

}  // namespace sco::sdk

// Exports PluginClass as this DLL's plugin: sco_plugin_query returns its info (api = the
// SCO_API_MAJOR/MINOR of this header), sco_plugin_load constructs it and runs OnLoad,
// sco_plugin_unload runs OnUnload, releases the SDK's state and destroys it. id, version and
// author are string literals; id must equal plugin.ini's id. Use once per DLL, at namespace scope:
//   SCO_PLUGIN(Hello, "hello", "1.0.0", "you");
#define SCO_PLUGIN(PluginClass, id, version, author)                                                   \
    namespace {                                                                                       \
    ::sco::sdk::detail::Slot<PluginClass> sco_sdk_slot_;                                              \
    }                                                                                                 \
    extern "C" SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) {                             \
        static const sco_plugin_info info = { sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, \
                                              id, version, author };                                  \
        return &info;                                                                                 \
    }                                                                                                 \
    extern "C" SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {          \
        return sco_sdk_slot_.Load(api, self);                                                         \
    }                                                                                                 \
    extern "C" SCO_EXPORT void sco_plugin_unload(void) { sco_sdk_slot_.Unload(); }                    \
    static_assert(true, "SCO_PLUGIN")

#endif  // SCOSDK_PLUGIN_HPP
