// Native loader: maps a plugin DLL, checks what it says about itself, calls sco_plugin_load,
// and contains faults in plugin code. Every call into plugin code goes through Guarded().
#include "sco/plugins.h"
#include "sco/log.h"
#include "sco/runtime.h"
#include "sco/status.h"
#include "internal.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace sco::plugins {

// ---- module ops -----------------------------------------------------------------------------

#ifdef _WIN32
static void* OpenModule(const fs::path& file, std::string& error) {
    // Dependencies resolve from the plugin's own folder and System32 only: never the game folder,
    // the current directory or PATH. LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs a fully qualified
    // path (a relative one fails with ERROR_INVALID_PARAMETER), and the plugin root may be
    // relative ("data/plugins").
    std::error_code ec;
    fs::path full = fs::absolute(file, ec);
    if (ec) full = file;
    HMODULE m = LoadLibraryExW(full.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "LoadLibraryExW error %lu", GetLastError());
        error = buf;
    }
    return reinterpret_cast<void*>(m);
}
static void* ModuleSymbol(void* m, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m), name));
}
static void CloseModule(void* m) { FreeLibrary(static_cast<HMODULE>(m)); }
#else
static void* OpenModule(const fs::path& file, std::string& error) {
    void* m = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) { const char* e = dlerror(); error = e ? e : "dlopen failed"; }
    return m;
}
static void* ModuleSymbol(void* m, const char* name) { return dlsym(m, name); }
static void CloseModule(void* m) { dlclose(m); }
#endif

ModuleOps PlatformModuleOps() { return ModuleOps{ OpenModule, ModuleSymbol, CloseModule }; }

// ---- guard ----------------------------------------------------------------------------------

#ifndef _WIN32
// No containment off Windows: host tests install their own guard with SetCallGuard.
uint32_t detail::DefaultGuard(void (*thunk)(void*), void* ctx) { thunk(ctx); return 0; }
#endif

static std::atomic<CallGuard> g_guard{ nullptr };

void SetCallGuard(CallGuard guard) { g_guard.store(guard); }

uint32_t Guarded(void (*thunk)(void*), void* ctx) {
    const CallGuard g = g_guard.load();
    return g ? g(thunk, ctx) : detail::DefaultGuard(thunk, ctx);
}

// ---- helpers --------------------------------------------------------------------------------

static const char* IdOf(const Plugin& p) { return p.manifestOk ? p.manifest.id.c_str() : p.folder.c_str(); }

static std::string CrashReason(const char* where, uint32_t code) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "crashed in %s (0x%08X)", where, code);
    return buf;
}

// Ok when nothing of p's is left in the runtime: no owner yet, released now, or released already
// (BadArg: a nested fault's MarkCrashed got there first). Anything else (TooMany, WrongThread)
// means the runtime may still call p's code.
static Result ReleaseOwner(Plugin& p) {
    if (!p.self) return Result::Ok;
    const Result r = sco::Release(p.self);
    if (r == Result::Ok || r == Result::BadArg) return Result::Ok;
    sco::Log("[plugin] %s: release returned %s", IdOf(p), sco::ResultName(r));
    return r;
}

// Release failed: the runtime may still hold p's callbacks, so its module (or script) must stay
// alive. Marked Crashed so nothing calls it again (the ContainCallouts guard skips it).
static bool ReleaseFailed(Plugin& p, Result r) {
    p.state = State::Crashed;
    p.reason = std::string("release failed: ") + sco::ResultName(r);
    sco::Log("[plugin] %s %s; kept loaded and disabled", IdOf(p), p.reason.c_str());
    sco::Status("plugin %s could not be released and was disabled", IdOf(p));
    return false;
}

static void Crash(Plugin& p, const char* where, uint32_t code) {
    ReleaseOwner(p);
    p.state = State::Crashed;
    p.reason = CrashReason(where, code);
    sco::Log("[plugin] %s %s and was disabled", IdOf(p), p.reason.c_str());
    sco::Status("plugin %s crashed and was disabled", IdOf(p));
}

// Everything the query thunk reads out of plugin memory lands in host-owned fixed buffers, so a
// bad info pointer faults inside the guard, not later in host code.
struct QueryCall {
    sco_plugin_query_fn fn;
    bool     gotInfo = false;
    uint32_t size = 0;
    uint16_t major = 0, minor = 0;
    bool     hasName = false;
    char     name[kMaxIdLen + 2] = {};   // one spare byte detects a name longer than any id
};

static void QueryThunk(void* c) {
    auto* q = static_cast<QueryCall*>(c);
    const sco_plugin_info* info = q->fn();
    if (!info) return;
    q->gotInfo = true;
    q->size = info->size;
    if (q->size < offsetof(sco_plugin_info, author) + sizeof(info->author)) return;
    q->major = info->api_major;
    q->minor = info->api_minor;
    if (!info->name) return;
    q->hasName = true;
    for (size_t i = 0; i + 1 < sizeof(q->name) && info->name[i]; ++i) q->name[i] = info->name[i];
}

struct LoadCall {
    sco_plugin_load_fn fn;
    const sco_api* api;
    sco_plugin* self;
    sco_result result = SCO_OK;
};
static void LoadThunk(void* c) {
    auto* l = static_cast<LoadCall*>(c);
    l->result = l->fn(l->api, l->self);
}

static void UnloadThunk(void* c) { reinterpret_cast<sco_plugin_unload_fn>(c)(); }

static std::atomic<uint32_t> g_loadCounter{ 0 };

// ---- load / unload --------------------------------------------------------------------------

bool LoadNative(Plugin& p, const sco_api* api, sco_plugin* self, const Options& opts, const ModuleOps& ops) {
    if (p.state != State::Ready || p.manifest.kind != Kind::Native) return false;
    if (!api || !self) {
        p.state = State::Refused;
        p.reason = "host passed no api or owner";
        sco::Log("[plugin] refused %s: %s", IdOf(p), p.reason.c_str());
        return false;
    }

    auto refuse = [&](std::string why) {
        if (p.module) { ops.close(p.module); p.module = nullptr; }
        p.exports = {};
        p.state = State::Refused;
        p.reason = std::move(why);
        sco::Log("[plugin] refused %s: %s", IdOf(p), p.reason.c_str());
        return false;
    };

    // Not guarded: mapping runs the plugin's DllMain under the OS loader lock, and unwinding out
    // of it would leave that lock held. Plugins keep DllMain empty (docs/plugins.md).
    std::string error;
    p.module = ops.open(p.dir / detail::FromUtf8(p.manifest.entry), error);
    if (!p.module) return refuse("cannot load " + p.manifest.entry + ": " + error);

    p.exports.query  = reinterpret_cast<sco_plugin_query_fn>(ops.symbol(p.module, "sco_plugin_query"));
    p.exports.load   = reinterpret_cast<sco_plugin_load_fn>(ops.symbol(p.module, "sco_plugin_load"));
    p.exports.unload = reinterpret_cast<sco_plugin_unload_fn>(ops.symbol(p.module, "sco_plugin_unload"));
    if (!p.exports.query)  return refuse("missing export sco_plugin_query");
    if (!p.exports.load)   return refuse("missing export sco_plugin_load");
    if (!p.exports.unload) return refuse("missing export sco_plugin_unload");

    QueryCall q{ p.exports.query };
    if (const uint32_t code = Guarded(QueryThunk, &q)) { Crash(p, "sco_plugin_query", code); return false; }
    if (p.state != State::Ready) return false;   // a nested fault already marked it
    if (!q.gotInfo) return refuse("sco_plugin_query returned NULL");
    if (q.size < offsetof(sco_plugin_info, author) + sizeof(void*)) return refuse("sco_plugin_info.size too small");
    if (q.major != opts.hostMajor || q.minor > opts.hostMinor) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "DLL built for api %u.%u", q.major, q.minor);
        return refuse(buf);
    }
    if (!q.hasName || p.manifest.id != q.name)
        return refuse("DLL name '" + std::string(q.hasName ? q.name : "") + "' does not match id '" + p.manifest.id + "'");

    p.self = self;
    LoadCall l{ p.exports.load, api, self };
    if (const uint32_t code = Guarded(LoadThunk, &l)) { Crash(p, "sco_plugin_load", code); return false; }
    if (p.state != State::Ready) return false;   // a nested fault already marked it (and released it)
    if (l.result != SCO_OK) {
        if (const Result r = ReleaseOwner(p); r != Result::Ok) return ReleaseFailed(p, r);   // keep it mapped
        return refuse(std::string("sco_plugin_load returned ") + sco::ResultName(static_cast<Result>(l.result)));
    }

    p.state = State::Loaded;
    p.reason.clear();
    p.loadOrder = g_loadCounter.fetch_add(1) + 1;
    sco::Log("[plugin] loaded %s %s (api %u.%u) from %s", IdOf(p), p.manifest.version.c_str(), q.major, q.minor,
             (p.folder + "/" + p.manifest.entry).c_str());
    return true;
}

void MarkCrashed(Plugin& p, const char* where, uint32_t code) {
    if (p.state != State::Loaded) return;
    Crash(p, where ? where : "a callback", code);
}

bool CallPlugin(Plugin& p, const char* where, void (*thunk)(void*), void* ctx) {
    if (p.state != State::Loaded || !thunk) return false;
    if (const uint32_t code = Guarded(thunk, ctx)) { MarkCrashed(p, where, code); return false; }
    return true;
}

static std::vector<Plugin>* g_contained = nullptr;   // game thread only

static bool ContainedCallout(const void* owner, const char* where, TaskFn thunk, void* ctx) {
    Plugin* p = nullptr;
    if (g_contained)
        for (Plugin& q : *g_contained)
            if (q.self && q.self == owner) { p = &q; break; }
    if (!p || p->state == State::Ready) {   // a host feature, or a plugin inside its load guard
        thunk(ctx);
        return true;
    }
    return CallPlugin(*p, where, thunk, ctx);
}

void ContainCallouts(std::vector<Plugin>* list) {
    g_contained = list;
    sco::SetCalloutGuard(list ? ContainedCallout : nullptr);
}

void UnloadNative(Plugin& p, const ModuleOps& ops) {
    if (p.state != State::Loaded || p.manifest.kind != Kind::Native) return;
    if (const uint32_t code = Guarded(UnloadThunk, reinterpret_cast<void*>(p.exports.unload))) {
        Crash(p, "sco_plugin_unload", code);   // module stays mapped
        return;
    }
    if (p.state != State::Loaded) return;   // a nested fault in unload() marked it Crashed: keep that
    if (const Result r = ReleaseOwner(p); r != Result::Ok) { ReleaseFailed(p, r); return; }   // never unmap
    if (p.module) ops.close(p.module);
    p.module = nullptr;
    p.exports = {};
    p.state = State::Unloaded;
    sco::Log("[plugin] unloaded %s", IdOf(p));
}

// ---- scripts --------------------------------------------------------------------------------

struct ScriptCall {
    const ScriptRuntime* rt;
    const sco_api* api;
    sco_plugin* self;
    const char* chunk;
    const std::string* text;
    char err[256] = {};
    sco_result result = SCO_OK;
};
static void ScriptThunk(void* c) {
    auto* k = static_cast<ScriptCall*>(c);
    k->result = k->rt->load(k->api, k->self, k->chunk, k->text->data(), k->text->size(), k->err, sizeof(k->err));
}

bool LoadScript(Plugin& p, const sco_api* api, sco_plugin* self, const ScriptRuntime& runtime) {
    if (p.state != State::Ready || p.manifest.kind != Kind::Lua) return false;
    auto refuse = [&](std::string why) {
        if (const Result r = ReleaseOwner(p); r != Result::Ok) return ReleaseFailed(p, r);
        p.state = State::Refused;
        p.reason = std::move(why);
        sco::Log("[plugin] refused %s: %s", IdOf(p), p.reason.c_str());
        return false;
    };
    if (!api || !self || !runtime.load || !runtime.unload) {
        p.state = State::Refused;
        p.reason = "host passed no api, owner or script runtime";
        sco::Log("[plugin] refused %s: %s", IdOf(p), p.reason.c_str());
        return false;
    }
    p.self = self;
    std::ifstream in(p.dir / detail::FromUtf8(p.manifest.entry), std::ios::binary);
    if (!in) return refuse("cannot read " + p.manifest.entry);
    std::string text;
    char buf[8192];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
        text.append(buf, static_cast<size_t>(in.gcount()));
        if (text.size() > kMaxScriptBytes) return refuse(p.manifest.entry + ": script too big");
    }
    ScriptCall k;
    k.rt = &runtime; k.api = api; k.self = self; k.chunk = p.manifest.entry.c_str(); k.text = &text;
    if (const uint32_t code = Guarded(ScriptThunk, &k)) { Crash(p, "the script runtime", code); return false; }
    if (p.state != State::Ready) return false;   // a nested fault already marked it
    if (k.result != SCO_OK) {
        runtime.unload(self);
        return refuse(k.err[0] ? std::string(k.err) : std::string("script load returned ") +
                      sco::ResultName(static_cast<Result>(k.result)));
    }
    p.state = State::Loaded;
    p.reason.clear();
    p.loadOrder = g_loadCounter.fetch_add(1) + 1;
    sco::Log("[plugin] loaded %s %s (lua) from %s", IdOf(p), p.manifest.version.c_str(),
             (p.folder + "/" + p.manifest.entry).c_str());
    return true;
}

static void UnloadScript(Plugin& p, const ScriptRuntime* runtime) {
    if (const Result r = ReleaseOwner(p); r != Result::Ok) { ReleaseFailed(p, r); return; }   // script kept
    if (runtime && runtime->unload) runtime->unload(p.self);
    p.state = State::Unloaded;
    sco::Log("[plugin] unloaded %s", IdOf(p));
}

void UnloadAll(std::vector<Plugin>& list, const ModuleOps& ops, const ScriptRuntime* runtime) {
    std::vector<Plugin*> loaded;
    for (auto& p : list)
        if (p.state == State::Loaded && (p.manifest.kind == Kind::Native || p.manifest.kind == Kind::Lua))
            loaded.push_back(&p);
    for (size_t i = 1; i < loaded.size(); ++i)         // insertion sort, newest first
        for (size_t j = i; j > 0 && loaded[j - 1]->loadOrder < loaded[j]->loadOrder; --j) std::swap(loaded[j - 1], loaded[j]);
    for (Plugin* p : loaded) {
        if (p->manifest.kind == Kind::Native) UnloadNative(*p, ops);
        else UnloadScript(*p, runtime);
    }
}

// ---- status ---------------------------------------------------------------------------------

std::string Describe(const Plugin& p) {
    std::string s = IdOf(p);
    s += ' ';
    s += p.manifestOk ? p.manifest.version : "?";
    s += ' ';
    s += p.manifestOk ? KindName(p.manifest.kind) : "?";
    s += ' ';
    s += StateName(p.state);
    if ((p.state == State::Refused || p.state == State::Crashed) && !p.reason.empty()) { s += ": "; s += p.reason; }
    return s;
}

void LogReport(const std::vector<Plugin>& list, bool enabled) {
    size_t loaded = 0;
    for (const auto& p : list) loaded += p.state == State::Loaded;
    sco::Log("[plugin] %zu found, %zu loaded (plugins = %s)", list.size(), loaded, enabled ? "on" : "off");
    for (const auto& p : list) sco::Log("[plugin] %s", Describe(p).c_str());
}

}  // namespace sco::plugins
