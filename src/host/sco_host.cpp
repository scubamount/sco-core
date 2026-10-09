// The host's sco_api table: thin C-ABI wrappers over the runtime, caps, status and log.
// Plugin code is only ever reached through host trampolines, so no call goes through a function
// pointer of the wrong type (sco_command_fn vs CommandFn, sco_invoke_done vs InvokeDone).
#include "sco/host.h"
#include "sco/caps.h"
#include "sco/log.h"
#include "sco/runtime.h"
#include "sco/status.h"
#include "../api/internal.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

// The opaque handle from sco_api.h. Lives in a fixed table, never freed or reused.
struct sco_plugin {
    char id[sco::host::kMaxIdLen + 1];
};

namespace sco::host {

namespace {

static_assert(static_cast<uint32_t>(Result::TooMany) == SCO_TOO_MANY && static_cast<uint32_t>(Result::Failed) == SCO_FAILED, "Result != sco_result");

sco_result C(Result r) { return static_cast<sco_result>(static_cast<uint32_t>(r)); }

// ---- handles --------------------------------------------------------------------------------

std::mutex          g_handleLock;          // serializes NewPlugin
sco_plugin          g_handles[kMaxPlugins];
std::atomic<size_t> g_handleCount{ 0 };    // published after the handle is written

const sco_plugin* Valid(const sco_plugin* p) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(p);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(&g_handles[0]);
    const size_t n = g_handleCount.load(std::memory_order_acquire);
    if (at < lo || at >= lo + n * sizeof(sco_plugin) || (at - lo) % sizeof(sco_plugin)) return nullptr;
    return p;
}

bool ValidId(const char* id) {
    if (!id || !*id || strnlen(id, kMaxIdLen + 1) > kMaxIdLen) return false;
    for (const char* p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    for (const char* r : kReservedPrefixes)
        if (strcmp(id, r) == 0) return false;
    return true;
}

// ---- commands -------------------------------------------------------------------------------

// What a plugin's command runs through: the runtime calls Trampoline with the record as ctx.
struct CmdRecord { sco_command_fn fn; void* ctx; };

std::mutex g_cmdLock;                      // serializes register_command and the view table
CmdRecord  g_records[kMaxCommands];        // one per successful plugin registration, kept forever
size_t     g_recordCount = 0;

Result Trampoline(const Arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t replySize) {
    const CmdRecord* rec = static_cast<const CmdRecord*>(ctx);
    // Arg and sco_arg share a layout (static_asserts in src/api/sco_commands.cpp).
    const sco_result r = rec->fn(reinterpret_cast<const sco_arg*>(args), nargs, rec->ctx, reply, replySize);
    return static_cast<Result>(static_cast<uint32_t>(r));
}

// list_commands views, one per runtime Command, built on first listing and kept forever.
struct View { const Command* cmd; sco_command view; };
View   g_views[kMaxCommands];
size_t g_viewCount = 0;

const sco_command* ViewFor(const Command* c) {   // g_cmdLock held
    for (size_t i = 0; i < g_viewCount; ++i)
        if (g_views[i].cmd == c) return &g_views[i].view;
    if (g_viewCount == kMaxCommands) return nullptr;   // can't happen: one view per registration
    View& v = g_views[g_viewCount++];
    v.cmd = c;
    v.view = {};
    v.view.size = sizeof(sco_command);
    v.view.name = c->name;
    v.view.title = c->title;
    v.view.help = c->help;
    v.view.capability = c->capability;
    v.view.args = reinterpret_cast<const sco_arg_def*>(c->args);   // ArgDef == sco_arg_def layout
    v.view.nargs = c->nargs;
    v.view.arg_def_size = sizeof(sco_arg_def);
    return &v.view;
}

// The done callback, behind the runtime's InvokeDone.
struct DoneRecord { sco_invoke_done done; void* ctx; bool heap; };

void DoneTrampoline(Result r, const char* reply, void* ctx) {
    DoneRecord* rec = static_cast<DoneRecord*>(ctx);
    rec->done(C(r), reply, rec->ctx);
    if (rec->heap) delete rec;
}

void DropDone(void* ctx) { delete static_cast<DoneRecord*>(ctx); }

// ---- the table ------------------------------------------------------------------------------

std::atomic<const char*> g_version{ nullptr };
sco_api                  g_api{};
std::once_flag           g_apiOnce;

const char* HostVersion() { return g_version.load(); }

int HasCap(const char* name) { return caps::Has(name) ? 1 : 0; }

sco_result RunOnGameThread(sco_plugin* self, sco_task_fn fn, void* ctx) {
    if (!Valid(self)) return SCO_BAD_ARG;
    return C(Post(fn, ctx, self));
}

sco_result SubscribeC(sco_plugin* self, const char* event, sco_event_fn fn, void* ctx) {
    if (!Valid(self)) return SCO_BAD_ARG;
    return C(Subscribe(self, event, fn, ctx));
}

sco_result UnsubscribeC(sco_plugin* self, const char* event, sco_event_fn fn) {
    if (!Valid(self) || detail::Released(self)) return SCO_BAD_ARG;   // the runtime would say NOT_FOUND
    return C(Unsubscribe(self, event, fn));
}

void StatusC(sco_plugin* self, const char* message) {
    if (!Valid(self) || !message || detail::Released(self)) return;
    Status("%s: %s", self->id, message);
}

void LogC(sco_plugin* self, sco_log_level level, const char* message) {
    if (!Valid(self) || !message || detail::Released(self)) return;
    const char* tag = level == SCO_LOG_WARN ? "warning: " : level == SCO_LOG_ERROR ? "error: " : "";
    Log("[%s] %s%s", self->id, tag, message);
}

sco_result RegisterCommandC(sco_plugin* self, const sco_command* cmd) {
    if (!Valid(self) || !cmd || cmd->size < sizeof(sco_command)) return SCO_BAD_ARG;
    if (cmd->nargs > kMaxCommandArgs || (cmd->nargs && !cmd->args)) return SCO_BAD_ARG;
    if (cmd->nargs && (cmd->arg_def_size < sizeof(sco_arg_def) || cmd->arg_def_size % alignof(sco_arg_def)))
        return SCO_BAD_ARG;
    if (!cmd->fn) return SCO_BAD_ARG;
    ArgDef defs[kMaxCommandArgs];
    const unsigned char* at = reinterpret_cast<const unsigned char*>(cmd->args);
    for (uint32_t i = 0; i < cmd->nargs; ++i, at += cmd->arg_def_size) {
        const sco_arg_def* d = reinterpret_cast<const sco_arg_def*>(at);
        defs[i] = { d->name, static_cast<ArgType>(d->type), d->help };
    }
    std::lock_guard<std::mutex> hold(g_cmdLock);
    if (g_recordCount == kMaxCommands) return SCO_TOO_MANY;
    CmdRecord& rec = g_records[g_recordCount];
    rec = { cmd->fn, cmd->ctx };
    const Command c{ cmd->name, cmd->title, cmd->help, cmd->capability,
                     cmd->nargs ? defs : nullptr, cmd->nargs, Trampoline, &rec };
    const Result r = RegisterCommand(self, self->id, c);
    if (r == Result::Ok) ++g_recordCount;   // a failed registration leaves the record free
    return C(r);
}

sco_result InvokeC(sco_plugin* self, const char* name, const sco_arg* args, uint32_t nargs,
                   sco_invoke_done done, void* ctx) {
    if (!Valid(self)) return SCO_BAD_ARG;
    const Arg* a = reinterpret_cast<const Arg*>(args);
    if (!done) return C(Invoke(name, a, nargs, nullptr, nullptr, self));
    if (OnGameThread()) {   // runs now: done is called before Invoke returns, if at all
        DoneRecord rec{ done, ctx, false };
        return C(Invoke(name, a, nargs, DoneTrampoline, &rec, self));
    }
    DoneRecord* rec = new (std::nothrow) DoneRecord{ done, ctx, true };
    if (!rec) return SCO_TOO_MANY;
    const Result r = detail::InvokeOwned(name, a, nargs, DoneTrampoline, rec, self, DropDone);
    if (r != Result::Ok) delete rec;   // nothing queued: done never runs
    return C(r);
}

uint32_t ListCommandsC(const sco_command** out, uint32_t max) {
    const Command* live[kMaxCommands];
    const size_t n = ListCommands(live, kMaxCommands);
    if (out && max) {
        std::lock_guard<std::mutex> hold(g_cmdLock);
        for (size_t i = 0; i < n && i < max; ++i) out[i] = ViewFor(live[i]);
    }
    return static_cast<uint32_t>(n);
}

// ---- services and raw handlers (1.1) ----------------------------------------------------------

// A plugin publishes and registers under its own id.
sco_result ProvideServiceC(sco_plugin* self, const char* name, uint32_t version, const void* vtable) {
    if (!Valid(self)) return SCO_BAD_ARG;
    return C(ProvideService(self, self->id, name, version, vtable));
}

sco_result QueryServiceC(const char* name, uint32_t minVersion, const void** out) {
    return C(QueryService(name, minVersion, out));
}

sco_result ReleaseServiceC(sco_plugin* self, const char* name) {
    if (!Valid(self)) return SCO_BAD_ARG;
    return C(ReleaseService(self, name));
}

// What a plugin's raw handler runs through, like CmdRecord for commands: one per successful
// registration, kept for the life of the process.
struct RawRecord { sco_raw_fn fn; void* ctx; };
constexpr size_t kMaxRawRecords = 1024;
std::mutex g_rawLock;
RawRecord  g_rawRecords[kMaxRawRecords];
size_t     g_rawRecordCount = 0;

Result RawTrampoline(const void* in, uint32_t inSize, void* out, uint32_t* outSize, void* ctx) {
    const RawRecord* rec = static_cast<const RawRecord*>(ctx);
    return static_cast<Result>(static_cast<uint32_t>(rec->fn(in, inSize, out, outSize, rec->ctx)));
}

sco_result RegisterRawC(sco_plugin* self, const char* name, const char* capability, sco_raw_fn fn, void* ctx) {
    if (!Valid(self) || !fn) return SCO_BAD_ARG;
    std::lock_guard<std::mutex> hold(g_rawLock);
    if (g_rawRecordCount == kMaxRawRecords) return SCO_TOO_MANY;
    RawRecord& rec = g_rawRecords[g_rawRecordCount];
    rec = { fn, ctx };
    const Result r = RegisterRaw(self, self->id, name, capability, RawTrampoline, &rec);
    if (r == Result::Ok) ++g_rawRecordCount;   // a failed registration leaves the record free
    return C(r);
}

sco_result InvokeRawC(sco_plugin* self, const char* name, const void* in, uint32_t inSize, void* out,
                      uint32_t* outSize) {
    if (!Valid(self)) { if (outSize) *outSize = 0; return SCO_BAD_ARG; }
    return C(InvokeRaw(self, name, in, inSize, out, outSize));
}

}  // namespace

const sco_api* BuildApi(const HostInfo& info) {
    if (!info.version) return nullptr;
    g_version.store(info.version);
    std::call_once(g_apiOnce, [] {
        g_api.size = sizeof(sco_api);
        g_api.major = SCO_API_MAJOR;
        g_api.minor = SCO_API_MINOR;
        g_api.host_version = HostVersion;
        g_api.has = HasCap;
        g_api.run_on_game_thread = RunOnGameThread;
        g_api.subscribe = SubscribeC;
        g_api.unsubscribe = UnsubscribeC;
        g_api.status = StatusC;
        g_api.log = LogC;
        g_api.register_command = RegisterCommandC;
        g_api.invoke = InvokeC;
        g_api.list_commands = ListCommandsC;
        g_api.provide_service = ProvideServiceC;
        g_api.query_service = QueryServiceC;
        g_api.release_service = ReleaseServiceC;
        g_api.invoke_raw = InvokeRawC;
        g_api.register_raw = RegisterRawC;
        SetCapabilityCheck(caps::Has);
    });
    return &g_api;
}

sco_plugin* NewPlugin(const char* id) {
    if (!ValidId(id)) return nullptr;
    std::lock_guard<std::mutex> hold(g_handleLock);
    const size_t n = g_handleCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i)
        if (strcmp(g_handles[i].id, id) == 0 && !detail::Released(&g_handles[i])) return nullptr;
    if (n == kMaxPlugins) return nullptr;
    memcpy(g_handles[n].id, id, strlen(id) + 1);
    g_handleCount.store(n + 1, std::memory_order_release);
    return &g_handles[n];
}

const char* PluginId(const sco_plugin* p) { return Valid(p) ? p->id : nullptr; }

}  // namespace sco::host
