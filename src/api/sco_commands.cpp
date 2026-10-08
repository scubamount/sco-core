// Command registry and Invoke().
// Registered commands sit in a fixed array that only grows; a slot is written before the count
// that publishes it, so readers (ListCommands, lookup) need no lock and pointers never move.
#include "sco/runtime.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

namespace sco {

static std::mutex          g_regLock;      // serializes writers only
static Command             g_cmds[kMaxCommands];
static std::atomic<size_t> g_cmdCount{ 0 };
static std::atomic<CapabilityCheck> g_capCheck{ nullptr };

void SetCapabilityCheck(CapabilityCheck check) { g_capCheck.store(check); }

// "<x>.<y>": lowercase letters, digits, '_' and '.'; no empty part.
static bool ValidName(const char* n) {
    if (!n || !*n) return false;
    bool dot = false;
    char prev = '.';
    for (const char* p = n; *p; ++p) {
        const char c = *p;
        if (c == '.') {
            if (prev == '.') return false;
            dot = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
        prev = c;
    }
    return dot && prev != '.';
}

static const Command* Find(const char* name) {
    const size_t n = g_cmdCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i)
        if (strcmp(g_cmds[i].name, name) == 0) return &g_cmds[i];
    return nullptr;
}

Result RegisterCommand(const char* ownerName, const Command& cmd) {
    if (!cmd.fn || !ValidName(cmd.name)) return Result::BadArg;
    if (ownerName) {
        const size_t len = strlen(ownerName);
        if (len == 0 || strncmp(cmd.name, ownerName, len) != 0 || cmd.name[len] != '.') return Result::BadArg;
    }
    if (cmd.nargs > kMaxCommandArgs || (cmd.nargs && !cmd.args)) return Result::BadArg;
    for (uint32_t i = 0; i < cmd.nargs; ++i) {
        const uint32_t t = static_cast<uint32_t>(cmd.args[i].type);
        if (!cmd.args[i].name || t > static_cast<uint32_t>(ArgType::Bool)) return Result::BadArg;
    }
    std::lock_guard<std::mutex> hold(g_regLock);
    if (Find(cmd.name)) return Result::BadArg;
    const size_t n = g_cmdCount.load(std::memory_order_relaxed);
    if (n == kMaxCommands) return Result::TooMany;
    g_cmds[n] = cmd;
    g_cmdCount.store(n + 1, std::memory_order_release);
    return Result::Ok;
}

size_t ListCommands(const Command** out, size_t max) {
    const size_t n = g_cmdCount.load(std::memory_order_acquire);
    for (size_t i = 0; out && i < n && i < max; ++i) out[i] = &g_cmds[i];
    return n;
}

// Runs on the game thread. reply is NUL-terminated whatever fn writes.
static Result RunNow(const char* name, const Arg* args, uint32_t nargs, char* reply, uint32_t replySize) {
    reply[0] = 0;
    const Command* c = Find(name);
    if (!c) return Result::NotFound;
    if (nargs != c->nargs) return Result::BadArg;
    for (uint32_t i = 0; i < nargs; ++i) {
        if (args[i].type != c->args[i].type) return Result::BadArg;
        if (args[i].type == ArgType::String && !args[i].v.s) return Result::BadArg;
    }
    if (c->capability) {
        const CapabilityCheck check = g_capCheck.load();
        if (!check || !check(c->capability)) return Result::Unavailable;
    }
    const Result r = c->fn(args, nargs, c->ctx, reply, replySize);
    reply[replySize - 1] = 0;
    return r;
}

// An Invoke from another thread, copied so the caller's buffers can go away.
struct Pending {
    std::string name;
    Arg args[kMaxCommandArgs];
    std::string strings[kMaxCommandArgs];
    uint32_t nargs;
    InvokeDone done;
    void* ctx;
};

static void RunPending(void* p) {
    Pending* job = static_cast<Pending*>(p);
    char reply[kReplySize];
    const Result r = RunNow(job->name.c_str(), job->args, job->nargs, reply, sizeof(reply));
    if (job->done) job->done(r, reply, job->ctx);
    delete job;
}

Result Invoke(const char* name, const Arg* args, uint32_t nargs, InvokeDone done, void* ctx) {
    if (!name || nargs > kMaxCommandArgs || (nargs && !args)) return Result::BadArg;
    if (OnGameThread()) {
        char reply[kReplySize];
        const Result r = RunNow(name, args, nargs, reply, sizeof(reply));
        if (done) done(r, reply, ctx);
        return r;
    }
    Pending* job = new (std::nothrow) Pending;
    if (!job) return Result::TooMany;
    job->name = name;
    job->nargs = nargs;
    job->done = done;
    job->ctx = ctx;
    for (uint32_t i = 0; i < nargs; ++i) {
        job->args[i] = args[i];
        if (args[i].type == ArgType::String && args[i].v.s) {
            job->strings[i] = args[i].v.s;
            job->args[i].v.s = job->strings[i].c_str();
        }
    }
    const Result r = Post(RunPending, job);
    if (r != Result::Ok) delete job;
    return r;
}

}  // namespace sco
