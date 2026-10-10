// Command registry and Invoke().
// Slots are pooled. A registration takes a free slot, or allocates one when none is free; a slot
// is never moved or freed, so a `const Command*` from ListCommands is always readable memory, but
// Release hands the owner's slots back to the pool and the next registration rewrites one: a
// pointer is a valid description of its command only while the owning plugin is loaded. Live
// registrations are capped at kMaxCommands, which also bounds the pool. Everything that reads or
// writes a slot's contents (lookup, list, invoke, register, release) holds g_regLock; Invoke copies
// what it needs out of the slot before it calls the command, so a command that releases its own
// owner leaves nothing dangling.
#include "sco/runtime.h"
#include "sco_api.h"
#include "internal.h"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>

namespace sco {

// Arg and ArgDef are the C structs under another name, so step 4 passes them straight through.
static_assert(sizeof(Arg) == sizeof(sco_arg) && offsetof(Arg, v) == offsetof(sco_arg, v), "Arg != sco_arg");
static_assert(sizeof(ArgDef) == sizeof(sco_arg_def) && offsetof(ArgDef, type) == offsetof(sco_arg_def, type) &&
              offsetof(ArgDef, help) == offsetof(sco_arg_def, help), "ArgDef != sco_arg_def");
static_assert(static_cast<uint32_t>(Result::TooMany) == SCO_TOO_MANY && static_cast<uint32_t>(ArgType::Bool) == SCO_ARG_BOOL,
              "enum values differ from sco_api.h");

struct Slot {
    Command view{};                 // what ListCommands hands out; points into the buffers below
    const void* owner = nullptr;
    size_t index = 0;               // fixed for the slot's life, < kMaxCommands
    uint64_t gen = 0;               // which registration the slot holds now; never repeats
    char name[kMaxNameLen + 1];
    char title[kMaxTitleLen + 1];
    char help[kMaxHelpLen + 1];
    char capability[kMaxCapabilityLen + 1];
    ArgDef args[kMaxCommandArgs];
    char argName[kMaxCommandArgs][kMaxArgNameLen + 1];
    char argHelp[kMaxCommandArgs][kMaxArgHelpLen + 1];
};

static std::mutex          g_regLock;      // guards everything below and every slot's contents
static Slot*               g_live[kMaxCommands];   // live commands, in registration order
static size_t              g_liveCount = 0;
static Slot*               g_free[kMaxCommands];   // released slots, ready for reuse
static size_t              g_freeCount = 0;
static size_t              g_created = 0;          // slots allocated so far: g_liveCount + g_freeCount
static uint64_t            g_nextGen = 1;
static std::atomic<CapabilityCheck> g_capCheck{ nullptr };

void SetCapabilityCheck(CapabilityCheck check) { g_capCheck.store(check); }

bool detail::CapabilityAvailable(const char* cap) {
    const CapabilityCheck check = g_capCheck.load();
    return check && cap && check(cap);
}

// Segments of lowercase letters, digits and '_' joined by '.'; no empty segment. The capability
// name rule (caps::Set). `dot` is set when there is more than one segment.
static bool Segments(const char* n, bool* dot) {
    *dot = false;
    if (!n || !*n) return false;
    char prev = '.';
    for (const char* p = n; *p; ++p) {
        const char c = *p;
        if (c == '.') {
            if (prev == '.') return false;
            *dot = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
        prev = c;
    }
    return prev != '.';
}

// "<x>.<y>": lowercase letters, digits, '_' and '.'; no empty part.
static bool ValidName(const char* n) {
    bool dot = false;
    return Segments(n, &dot) && dot;
}

static bool ValidCapability(const char* n) {
    bool dot = false;
    return Segments(n, &dot);
}

static bool Fits(const char* s, size_t max) { return !s || strnlen(s, max + 1) <= max; }

// nullptr in -> nullptr out; otherwise copies into buf. Bounded by buf even though the length was
// checked: a racing writer may lengthen the caller's string after the check.
template <size_t N>
static const char* Copy(char (&buf)[N], const char* s) {
    if (!s) return nullptr;
    const size_t len = strnlen(s, N - 1);
    memcpy(buf, s, len);
    buf[len] = 0;
    return buf;
}

static bool HasPrefix(const char* name, const char* prefix, size_t len) {
    return strncmp(name, prefix, len) == 0 && name[len] == '.';
}

static Slot* FindLive(const char* name) {   // g_regLock held
    for (size_t i = 0; i < g_liveCount; ++i)
        if (strcmp(g_live[i]->name, name) == 0) return g_live[i];
    return nullptr;
}

// Another owner holds a live command under "<prefix>.".
static bool PrefixTaken(const void* owner, const char* prefix, size_t len) {   // g_regLock held
    for (size_t i = 0; i < g_liveCount; ++i) {
        const Slot* s = g_live[i];
        if (s->owner != owner && HasPrefix(s->name, prefix, len)) return true;
    }
    return false;
}

Result RegisterCommand(const void* owner, const char* prefix, const Command& cmd) {
    if (!cmd.fn || !ValidName(cmd.name) || !Fits(cmd.name, kMaxNameLen)) return Result::BadArg;
    if (!Fits(cmd.title, kMaxTitleLen) || !Fits(cmd.help, kMaxHelpLen) || !Fits(cmd.capability, kMaxCapabilityLen))
        return Result::BadArg;
    if (cmd.capability && !ValidCapability(cmd.capability)) return Result::BadArg;   // could never be granted
    size_t prefixLen = 0;
    if (prefix) {
        prefixLen = strlen(prefix);
        if (!owner || prefixLen == 0 || strchr(prefix, '.') || !HasPrefix(cmd.name, prefix, prefixLen))
            return Result::BadArg;
        for (const char* r : kReservedPrefixes)
            if (strcmp(prefix, r) == 0) return Result::BadArg;
    }
    if (cmd.nargs > kMaxCommandArgs || (cmd.nargs && !cmd.args)) return Result::BadArg;
    for (uint32_t i = 0; i < cmd.nargs; ++i) {
        const ArgDef& d = cmd.args[i];
        if (!d.name || static_cast<uint32_t>(d.type) > static_cast<uint32_t>(ArgType::Bool)) return Result::BadArg;
        if (!Fits(d.name, kMaxArgNameLen) || !Fits(d.help, kMaxArgHelpLen)) return Result::BadArg;
    }

    std::lock_guard<std::mutex> hold(g_regLock);
    if (detail::Released(owner) || FindLive(cmd.name)) return Result::BadArg;
    if (prefix && PrefixTaken(owner, prefix, prefixLen)) return Result::BadArg;
    if (g_liveCount == kMaxCommands) return Result::TooMany;
    Slot* s = g_freeCount ? g_free[g_freeCount - 1] : new (std::nothrow) Slot;
    if (!s) return Result::TooMany;
    if (g_freeCount) --g_freeCount;
    else s->index = g_created++;
    s->view = Command{};
    s->owner = owner;
    s->gen = g_nextGen++;
    s->view.name = Copy(s->name, cmd.name);
    s->view.title = Copy(s->title, cmd.title);
    s->view.help = Copy(s->help, cmd.help);
    s->view.capability = Copy(s->capability, cmd.capability);
    for (uint32_t i = 0; i < cmd.nargs; ++i) {
        s->args[i].type = cmd.args[i].type;
        s->args[i].name = Copy(s->argName[i], cmd.args[i].name);
        s->args[i].help = Copy(s->argHelp[i], cmd.args[i].help);
    }
    s->view.args = cmd.nargs ? s->args : nullptr;
    s->view.nargs = cmd.nargs;
    s->view.fn = cmd.fn;
    s->view.ctx = cmd.ctx;
    g_live[g_liveCount++] = s;
    return Result::Ok;
}

long detail::ReleaseCommands(const void* owner) {
    std::lock_guard<std::mutex> hold(g_regLock);
    long removed = 0;
    size_t keep = 0;   // closes the gaps, so the live list stays in registration order
    for (size_t i = 0; i < g_liveCount; ++i) {
        Slot* s = g_live[i];
        if (s->owner == owner) { g_free[g_freeCount++] = s; ++removed; }
        else g_live[keep++] = s;
    }
    g_liveCount = keep;
    return removed;
}

size_t ListCommands(const Command** out, size_t max) {
    std::lock_guard<std::mutex> hold(g_regLock);
    if (out)
        for (size_t i = 0; i < g_liveCount && i < max; ++i) out[i] = &g_live[i]->view;
    return g_liveCount;
}

size_t detail::VisitCommands(CommandVisitor visit, void* ctx) {
    std::lock_guard<std::mutex> hold(g_regLock);
    for (size_t i = 0; i < g_liveCount; ++i) visit(i, g_live[i]->index, g_live[i]->gen, g_live[i]->view, ctx);
    return g_liveCount;
}

struct CommandCall { CommandFn fn; void* ctx; const Arg* args; uint32_t nargs; char* reply; uint32_t replySize; Result r; };
static void CommandThunk(void* p) {
    CommandCall* k = static_cast<CommandCall*>(p);
    k->r = k->fn(k->args, k->nargs, k->ctx, k->reply, k->replySize);
}

// Runs on the game thread. reply is NUL-terminated whatever fn writes; empty when fn faulted.
static Result RunNow(const char* name, const Arg* args, uint32_t nargs, char* reply, uint32_t replySize) {
    reply[0] = 0;
    // What the call needs, copied out under the lock: once it is released the slot may be rewritten
    // (and the command itself may release its owner while it runs).
    CommandFn fn;
    void* fnCtx;
    const void* owner;
    char cmdName[kMaxNameLen + 1];
    char capability[kMaxCapabilityLen + 1];
    bool gated;
    {
        std::lock_guard<std::mutex> hold(g_regLock);
        const Slot* s = FindLive(name);
        if (!s) return Result::NotFound;
        const Command& c = s->view;
        if (nargs != c.nargs) return Result::BadArg;
        for (uint32_t i = 0; i < nargs; ++i) {
            if (args[i].type != c.args[i].type) return Result::BadArg;
            if (args[i].type == ArgType::String && !args[i].v.s) return Result::BadArg;
            if (args[i].type == ArgType::Bool && args[i].v.i != 0 && args[i].v.i != 1) return Result::BadArg;
        }
        fn = c.fn;
        fnCtx = c.ctx;
        owner = s->owner;
        Copy(cmdName, c.name);
        gated = c.capability != nullptr;
        if (gated) Copy(capability, c.capability);
    }
    if (gated) {
        const CapabilityCheck check = g_capCheck.load();
        if (!check || !check(capability)) return Result::Unavailable;
    }
    CommandCall call{ fn, fnCtx, args, nargs, reply, replySize, Result::Ok };
    if (!detail::Callout(owner, cmdName, CommandThunk, &call)) {
        reply[0] = 0;
        return Result::Crashed;
    }
    reply[replySize - 1] = 0;
    return call.r;
}

struct DoneCall { InvokeDone done; Result r; const char* reply; void* ctx; };
static void DoneThunk(void* p) {
    DoneCall* k = static_cast<DoneCall*>(p);
    k->done(k->r, k->reply, k->ctx);
}

// owner: the invoking owner, the owner done runs as. If the command released that owner, done is
// not called (the owner's code may be gone): dropCtx(ctx) runs instead, and also when done
// faulted before it could free its ctx.
static void RunAndReport(const char* name, const Arg* args, uint32_t nargs, InvokeDone done, void* ctx,
                         const void* owner, TaskFn dropCtx, Result* out) {
    char reply[kReplySize];
    const Result r = RunNow(name, args, nargs, reply, sizeof(reply));
    if (done) {
        DoneCall call{ done, r, reply, ctx };
        if (detail::Released(owner) || !detail::Callout(owner, "invoke done", DoneThunk, &call)) {
            if (dropCtx) dropCtx(ctx);
        }
    }
    if (out) *out = r;
}

// An Invoke from another thread, copied so the caller's buffers can go away. No std::string:
// nothing here may throw.
struct Pending {
    char name[kMaxNameLen + 2];     // one spare byte: a longer name stays unmatched (NotFound)
    Arg args[kMaxCommandArgs];
    char* strings;                  // every String arg, back to back
    uint32_t nargs;
    InvokeDone done;
    void* ctx;
    const void* owner;              // the invoking owner
    TaskFn dropCtx;                 // frees ctx when Release drops the call; may be null
};

static void FreePending(void* p) {
    Pending* job = static_cast<Pending*>(p);
    delete[] job->strings;
    delete job;
}

static void DropPending(void* p) {
    Pending* job = static_cast<Pending*>(p);
    if (job->dropCtx) job->dropCtx(job->ctx);
    FreePending(job);
}

static void RunPending(void* p) {
    Pending* job = static_cast<Pending*>(p);
    RunAndReport(job->name, job->args, job->nargs, job->done, job->ctx, job->owner, job->dropCtx, nullptr);
    FreePending(job);
}

Result Invoke(const char* name, const Arg* args, uint32_t nargs, InvokeDone done, void* ctx, const void* owner) {
    return detail::InvokeOwned(name, args, nargs, done, ctx, owner, nullptr);
}

Result detail::InvokeOwned(const char* name, const Arg* args, uint32_t nargs, InvokeDone done, void* ctx,
                           const void* owner, TaskFn dropCtx) {
    if (!name || nargs > kMaxCommandArgs || (nargs && !args) || detail::Released(owner)) return Result::BadArg;
    if (OnGameThread()) {
        Result r = Result::Ok;
        RunAndReport(name, args, nargs, done, ctx, owner, dropCtx, &r);
        return r;
    }
    Pending* job = new (std::nothrow) Pending;
    if (!job) return Result::TooMany;
    const size_t nameLen = strnlen(name, sizeof(job->name) - 1);
    memcpy(job->name, name, nameLen);
    job->name[nameLen] = 0;
    job->nargs = nargs;
    job->done = done;
    job->ctx = ctx;
    job->owner = owner;
    job->dropCtx = dropCtx;
    job->strings = nullptr;
    size_t total = 0;
    for (uint32_t i = 0; i < nargs; ++i)
        if (args[i].type == ArgType::String && args[i].v.s) total += strlen(args[i].v.s) + 1;
    if (total) {
        job->strings = new (std::nothrow) char[total];
        if (!job->strings) { delete job; return Result::TooMany; }
    }
    char* at = job->strings;
    for (uint32_t i = 0; i < nargs; ++i) {
        job->args[i] = args[i];
        if (args[i].type == ArgType::String && args[i].v.s) {
            const size_t len = strlen(args[i].v.s) + 1;
            memcpy(at, args[i].v.s, len);
            job->args[i].v.s = at;
            at += len;
        }
    }
    const Result r = detail::PostOwned(RunPending, DropPending, job, owner);
    if (r != Result::Ok) FreePending(job);
    return r;
}

}  // namespace sco
