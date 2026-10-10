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
#include <memory>
#include <new>
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
    // A file that isn't a valid image (a broken or truncated plugin DLL) makes the loader raise a
    // "Bad Image" hard-error message box and wait for someone to close it: the game thread would
    // hang on it. Fail the load quietly instead, for this thread and this call only.
    DWORD oldMode = 0;
    const BOOL quiet = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode);
    HMODULE m = LoadLibraryExW(full.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const DWORD loadError = m ? 0 : GetLastError();
    if (quiet) SetThreadErrorMode(oldMode, nullptr);
    if (!m) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "LoadLibraryExW error %lu", loadError);
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

// What the guard on this thread reported about the last fault it caught (internal.h).
static thread_local detail::FaultTrace t_trace;

void detail::RecordFaultTrace(void* const* modules, uint32_t n) {
    t_trace.n = n < kMaxTraceFrames ? n : kMaxTraceFrames;
    for (uint32_t i = 0; i < t_trace.n; ++i) t_trace.modules[i] = modules[i];
}

// Guarded() that also hands back the fault trace. The trace is cleared before the call and after
// it, so one a guard left behind (a fault in a call nobody asked about) never reaches a later,
// unrelated fault; a guard that doesn't report leaves it empty.
static uint32_t GuardedTraced(void (*thunk)(void*), void* ctx, detail::FaultTrace& out) {
    t_trace.n = 0;
    const CallGuard g = g_guard.load();
    const uint32_t code = g ? g(thunk, ctx) : detail::DefaultGuard(thunk, ctx);
    out.n = code ? t_trace.n : 0;
    for (uint32_t i = 0; i < out.n; ++i) out.modules[i] = t_trace.modules[i];
    t_trace.n = 0;
    return code;
}

uint32_t Guarded(void (*thunk)(void*), void* ctx) {
    detail::FaultTrace unused;
    return GuardedTraced(thunk, ctx, unused);
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
    bool     full = false;               // also copy version and author (built-ins)
    bool     hasName = false;
    char     name[kMaxIdLen + 2] = {};   // one spare byte detects a name longer than any id
    char     version[kMaxVersionLen + 1] = {};   // built-ins: the manifest comes from here
    char     author[kMaxAuthorLen + 1] = {};
};

static void CopyCapped(char* to, size_t cap, const char* from) {
    for (size_t i = 0; from && i + 1 < cap && from[i]; ++i) to[i] = from[i];
}

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
    CopyCapped(q->name, sizeof(q->name), info->name);
    if (!q->full) return;
    CopyCapped(q->version, sizeof(q->version), info->version);
    CopyCapped(q->author, sizeof(q->author), info->author);
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

// True when a service table of p's was handed out by query_service: its code must stay mapped for
// the session, because a caller may still hold the table. Ask after sco::Release(p.self) has
// withdrawn p's services: from then on no new table can be handed out, so the answer is final.
static bool KeepMapped(const Plugin& p) {
    if (!p.self || !sco::ServiceTableHandedOut(p.self)) return false;
    sco::Log("[plugin] %s: kept mapped: its service table was handed out", IdOf(p));
    return true;
}

// Closes p's module (natives) unless KeepMapped; p.module stays set while the module does, so a
// fault inside it is still attributed to p.
static void CloseOrKeep(Plugin& p, const ModuleOps* ops) {
    if (p.module && ops && KeepMapped(p)) return;
    if (p.module && ops) ops->close(p.module);
    p.module = nullptr;
}

// Refuses p: closes its module (natives), forgets its exports, logs. Always false.
static bool RefuseLoad(Plugin& p, std::string why, const ModuleOps* ops) {
    CloseOrKeep(p, ops);
    p.exports = {};
    p.state = State::Refused;
    p.reason = std::move(why);
    sco::Log("[plugin] refused %s: %s", IdOf(p), p.reason.c_str());
    return false;
}

// The part natives and built-ins share once the three exports are known: query() and its
// checks, then load(), both guarded. ops is null for a built-in (no module to close).
static bool QueryAndLoad(Plugin& p, const sco_api* api, sco_plugin* self, const Options& opts,
                         const ModuleOps* ops, QueryCall& q) {
    const bool builtin = p.manifest.kind == Kind::Builtin;
    q.fn = p.exports.query;
    q.full = builtin;
    if (const uint32_t code = Guarded(QueryThunk, &q)) { Crash(p, "sco_plugin_query", code); return false; }
    if (p.state != State::Ready) return false;   // a nested fault already marked it
    if (!q.gotInfo) return RefuseLoad(p, "sco_plugin_query returned NULL", ops);
    if (q.size < offsetof(sco_plugin_info, author) + sizeof(void*)) return RefuseLoad(p, "sco_plugin_info.size too small", ops);
    if (q.major != opts.hostMajor || q.minor > opts.hostMinor) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%s built for api %u.%u", builtin ? "built-in" : "DLL", q.major, q.minor);
        return RefuseLoad(p, buf, ops);
    }
    if (!q.hasName || p.manifest.id != q.name)
        return RefuseLoad(p, std::string(builtin ? "built-in" : "DLL") + " name '" + (q.hasName ? q.name : "") +
                          "' does not match id '" + p.manifest.id + "'", ops);
    if (builtin) {   // no plugin.ini: the manifest is what the plugin says about itself
        p.manifest.name = p.manifest.id;
        p.manifest.version = q.version[0] ? q.version : "?";
        p.manifest.author = q.author;
        p.manifest.apiMajor = q.major;
        p.manifest.apiMinor = q.minor;
    }

    p.self = self;
    LoadCall l{ p.exports.load, api, self };
    if (const uint32_t code = Guarded(LoadThunk, &l)) { Crash(p, "sco_plugin_load", code); return false; }
    if (p.state != State::Ready) return false;   // a nested fault already marked it (and released it)
    if (l.result != SCO_OK) {
        if (const Result r = ReleaseOwner(p); r != Result::Ok) return ReleaseFailed(p, r);   // keep it mapped
        return RefuseLoad(p, std::string("sco_plugin_load returned ") + sco::ResultName(static_cast<Result>(l.result)), ops);
    }

    p.state = State::Loaded;
    p.reason.clear();
    p.loadOrder = g_loadCounter.fetch_add(1) + 1;
    if (builtin)
        sco::Log("[plugin] loaded %s %s (api %u.%u) built in", IdOf(p), p.manifest.version.c_str(), q.major, q.minor);
    else
        sco::Log("[plugin] loaded %s %s (api %u.%u) from %s", IdOf(p), p.manifest.version.c_str(), q.major, q.minor,
                 (p.folder + "/" + p.manifest.entry).c_str());
    return true;
}

bool LoadNative(Plugin& p, const sco_api* api, sco_plugin* self, const Options& opts, const ModuleOps& ops) {
    if (p.state != State::Ready || p.manifest.kind != Kind::Native) return false;
    if (!api || !self) return RefuseLoad(p, "host passed no api or owner", nullptr);

    // Not guarded: mapping runs the plugin's DllMain under the OS loader lock, and unwinding out
    // of it would leave that lock held. Plugins keep DllMain empty (docs/plugins.md).
    std::string error;
    p.module = ops.open(p.dir / detail::FromUtf8(p.manifest.entry), error);
    if (!p.module) return RefuseLoad(p, "cannot load " + p.manifest.entry + ": " + error, &ops);

    p.exports.query  = reinterpret_cast<sco_plugin_query_fn>(ops.symbol(p.module, "sco_plugin_query"));
    p.exports.load   = reinterpret_cast<sco_plugin_load_fn>(ops.symbol(p.module, "sco_plugin_load"));
    p.exports.unload = reinterpret_cast<sco_plugin_unload_fn>(ops.symbol(p.module, "sco_plugin_unload"));
    if (!p.exports.query)  return RefuseLoad(p, "missing export sco_plugin_query", &ops);
    if (!p.exports.load)   return RefuseLoad(p, "missing export sco_plugin_load", &ops);
    if (!p.exports.unload) return RefuseLoad(p, "missing export sco_plugin_unload", &ops);

    QueryCall q{};
    return QueryAndLoad(p, api, self, opts, &ops, q);
}

// ---- built-ins ------------------------------------------------------------------------------

static bool ValidBuiltinId(const char* id) {
    if (!id || !*id) return false;
    size_t n = 0;
    for (const char* c = id; *c; ++c, ++n)
        if (n == kMaxIdLen || !((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;
    for (const char* r : { "sco", "host", "menu", "game" })
        if (std::strcmp(id, r) == 0) return false;
    return true;
}

Plugin FromBuiltin(const Builtin& b) {
    Plugin p;
    p.manifestOk = true;
    p.manifest.kind = Kind::Builtin;
    p.exports = { b.query, b.load, b.unload };
    if (!ValidBuiltinId(b.id)) {
        p.folder = p.manifest.id = b.id ? std::string(b.id, strnlen(b.id, kMaxIdLen + 1)) : std::string("?");
        p.state = State::Refused;
        p.reason = "built-in id '" + p.folder + "' is not valid";
        return p;
    }
    p.folder = p.manifest.id = b.id;
    p.state = State::Ready;
    const char* missing = !b.query ? "sco_plugin_query" : !b.load ? "sco_plugin_load" : !b.unload ? "sco_plugin_unload" : nullptr;
    if (missing) {
        p.state = State::Refused;
        p.reason = std::string("built-in ") + b.id + " has no " + missing;
    }
    return p;
}

bool LoadBuiltin(Plugin& p, const sco_api* api, sco_plugin* self, const Options& opts) {
    if (p.state != State::Ready || p.manifest.kind != Kind::Builtin) return false;
    if (!api || !self) return RefuseLoad(p, "host passed no api or owner", nullptr);
    QueryCall q{};
    return QueryAndLoad(p, api, self, opts, nullptr, q);
}

void MarkCrashed(Plugin& p, const char* where, uint32_t code) {
    if (p.state != State::Loaded) return;
    Crash(p, where ? where : "a callback", code);
}

static std::vector<Plugin>* g_contained = nullptr;   // game thread only

// The plugin whose code faulted when it is not the callout's owner: the first frame, innermost
// first, that lies in a plugin's module. Null (blame the owner) when there is no list, no trace,
// no plugin frame, or the first plugin frame is the owner's. Frames of host, game and built-in
// code never name anyone: built-ins share the product's module.
static Plugin* FindCulprit(const Plugin& owner, const detail::FaultTrace& trace) {
    if (!g_contained) return nullptr;
    for (uint32_t i = 0; i < trace.n; ++i)
        for (Plugin& q : *g_contained)
            if (q.module && q.module == trace.modules[i]) return &q == &owner ? nullptr : &q;
    return nullptr;
}

// A provider faulted inside a service call made from `caller`'s callout. Marks it Crashed; one
// that is not Loaded any more (crashed already, or unloaded with its module kept mapped) only gets
// a log line. Game thread.
static void BlameProviderNow(Plugin& culprit, const char* where, uint32_t code) {
    if (culprit.state == State::Loaded) { MarkCrashed(culprit, where, code); return; }
    sco::Log("[plugin] %s faulted again in %s (0x%08X); it is already %s", IdOf(culprit), where, code,
             StateName(culprit.state));
}

struct Blame {
    Plugin*  culprit;
    uint32_t code;
    char     where[96];
};

static void BlameTask(void* c) {
    const std::unique_ptr<Blame> b(static_cast<Blame*>(c));
    BlameProviderNow(*b->culprit, b->where, b->code);
}

// MarkCrashed is game-thread only (it releases the owner and edits the Plugin), so off the game
// thread the verdict is queued as a task and applied there.
static void BlameProvider(Plugin& culprit, const Plugin& caller, uint32_t code) {
    Blame tmp{ &culprit, code, {} };
    std::snprintf(tmp.where, sizeof(tmp.where), "a service call from %s", IdOf(caller));
    if (sco::OnGameThread()) { BlameProviderNow(culprit, tmp.where, code); return; }
    Blame* b = new (std::nothrow) Blame(tmp);
    if (b && sco::Post(BlameTask, b, nullptr) == sco::Result::Ok) return;
    delete b;
    sco::Log("[plugin] %s faulted in %s (0x%08X); could not queue it, so it stays enabled", IdOf(culprit), tmp.where, code);
}

bool CallPlugin(Plugin& p, const char* where, void (*thunk)(void*), void* ctx) {
    if (p.state != State::Loaded || !thunk) return false;
    detail::FaultTrace trace;
    if (const uint32_t code = GuardedTraced(thunk, ctx, trace)) {
        if (Plugin* culprit = FindCulprit(p, trace)) BlameProvider(*culprit, p, code);   // p's callout was aborted; p is fine
        else MarkCrashed(p, where, code);
        return false;
    }
    return true;
}

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

// Unloads a loaded native (ops set: closes its module) or built-in (ops null: nothing to close).
static void UnloadCode(Plugin& p, const ModuleOps* ops) {
    if (const uint32_t code = Guarded(UnloadThunk, reinterpret_cast<void*>(p.exports.unload))) {
        Crash(p, "sco_plugin_unload", code);   // module stays mapped
        return;
    }
    if (p.state != State::Loaded) return;   // a nested fault in unload() marked it Crashed: keep that
    if (const Result r = ReleaseOwner(p); r != Result::Ok) { ReleaseFailed(p, r); return; }   // never unmap
    CloseOrKeep(p, ops);   // stays mapped when a service table of p's was handed out
    p.exports = {};
    p.state = State::Unloaded;
    sco::Log("[plugin] unloaded %s", IdOf(p));
}

void UnloadNative(Plugin& p, const ModuleOps& ops) {
    if (p.state != State::Loaded || p.manifest.kind != Kind::Native) return;
    UnloadCode(p, &ops);
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
        // Release first, then free the script (as UnloadScript does): until Release the runtime
        // holds callbacks into the script state.
        if (const Result r = ReleaseOwner(p); r != Result::Ok) return ReleaseFailed(p, r);   // script kept
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
        if (p.state == State::Loaded && p.manifest.kind != Kind::Data) loaded.push_back(&p);
    // Built-ins after every other plugin, then newest first.
    auto before = [](const Plugin* a, const Plugin* b) {
        const bool ab = a->manifest.kind == Kind::Builtin, bb = b->manifest.kind == Kind::Builtin;
        return ab != bb ? bb : a->loadOrder > b->loadOrder;
    };
    for (size_t i = 1; i < loaded.size(); ++i)         // insertion sort
        for (size_t j = i; j > 0 && before(loaded[j], loaded[j - 1]); --j) std::swap(loaded[j - 1], loaded[j]);
    for (Plugin* p : loaded) {
        if (p->state != State::Loaded) continue;       // crashed while an earlier one unloaded
        switch (p->manifest.kind) {
            case Kind::Native:  UnloadCode(*p, &ops); break;
            case Kind::Builtin: UnloadCode(*p, nullptr); break;
            case Kind::Lua:     UnloadScript(*p, runtime); break;
            case Kind::Data:    break;
        }
    }
}

// ---- status ---------------------------------------------------------------------------------

std::string Describe(const Plugin& p) {
    std::string s = IdOf(p);
    s += ' ';
    s += p.manifestOk && !p.manifest.version.empty() ? p.manifest.version : "?";
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
