// Unit tests for the game-thread runtime: task queue, event bus, command registry.
// Host build, no game needed. The "game thread" is this test's main thread.
//   tools/test.sh
#include "sco/runtime.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using sco::Result;

// ---- game thread ----------------------------------------------------------------------------

static void TestBeforeGameThread() {
    // Nothing is the game thread until SetGameThread() runs.
    CHECK(!sco::OnGameThread());
    CHECK(sco::GameThreadTick(1) == Result::WrongThread);
    CHECK(sco::Dispatch("tick", nullptr) == Result::WrongThread);
    CHECK(sco::DrainTasks() == 0);
    sco::SetGameThread();
    CHECK(sco::OnGameThread());
    bool other = true;
    std::thread([&] { other = sco::OnGameThread(); }).join();
    CHECK(!other);
    CHECK(strcmp(sco::ResultName(Result::TooMany), "TOO_MANY") == 0);
}

// ---- task queue -----------------------------------------------------------------------------

static std::vector<int> g_order;
static void Record(void* ctx) { g_order.push_back(static_cast<int>(reinterpret_cast<intptr_t>(ctx))); }
static void* Tag(int i) { return reinterpret_cast<void*>(static_cast<intptr_t>(i)); }

static void PostAgain(void* ctx) {   // posts a follow-up while the queue drains
    Record(ctx);
    CHECK(sco::Post(Record, Tag(99)) == Result::Ok);
}

static size_t g_nestedDrain = 99;
static Result g_nestedTick = Result::Ok;
static void Nested(void*) {
    Record(Tag(1));
    g_nestedDrain = sco::DrainTasks();
    g_nestedTick = sco::GameThreadTick(1);
}
static char kTaskB;
static void ReleaseB(void*) { sco::Release(&kTaskB); }

static std::atomic<int> g_crossRan{ 0 };
static void CountCross(void*) { g_crossRan.fetch_add(1); }

static void TestTaskQueue() {
    g_order.clear();
    CHECK(sco::Post(nullptr, nullptr) == Result::BadArg);
    for (int i = 0; i < 5; ++i) CHECK(sco::Post(Record, Tag(i)) == Result::Ok);
    CHECK(sco::QueuedTasks() == 5);
    size_t offThread = 1;
    std::thread([&] { offThread = sco::DrainTasks(); }).join();
    CHECK(offThread == 0 && sco::QueuedTasks() == 5);   // off the game thread nothing runs
    CHECK(sco::DrainTasks() == 5);
    CHECK((g_order == std::vector<int>{ 0, 1, 2, 3, 4 }));
    CHECK(sco::QueuedTasks() == 0);

    // Bound: 256 fit, the 257th is refused and nothing is lost or reordered.
    g_order.clear();
    int ok = 0;
    for (size_t i = 0; i < sco::kMaxQueuedTasks; ++i) ok += sco::Post(Record, Tag(static_cast<int>(i))) == Result::Ok;
    CHECK(ok == static_cast<int>(sco::kMaxQueuedTasks));
    CHECK(sco::Post(Record, Tag(-1)) == Result::TooMany);
    CHECK(sco::DrainTasks() == sco::kMaxQueuedTasks);
    bool inOrder = g_order.size() == sco::kMaxQueuedTasks;
    for (size_t i = 0; inOrder && i < g_order.size(); ++i) inOrder = g_order[i] == static_cast<int>(i);
    CHECK(inOrder);
    CHECK(sco::Post(Record, Tag(7)) == Result::Ok);   // room again after the drain (ring wrapped)
    CHECK(sco::DrainTasks() == 1 && g_order.back() == 7);

    // A task posted while draining waits for the next drain.
    g_order.clear();
    CHECK(sco::Post(PostAgain, Tag(1)) == Result::Ok);
    CHECK(sco::DrainTasks() == 1 && (g_order == std::vector<int>{ 1 }));
    CHECK(sco::QueuedTasks() == 1);
    CHECK(sco::DrainTasks() == 1 && (g_order == std::vector<int>{ 1, 99 }));

    // A task can't drain or tick from inside the drain (regression: a nested drain used to
    // underflow the count and run a null task).
    g_order.clear();
    CHECK(sco::Post(Nested, nullptr) == Result::Ok);
    CHECK(sco::Post(Record, Tag(2)) == Result::Ok);
    CHECK(sco::DrainTasks() == 2 && (g_order == std::vector<int>{ 1, 2 }));
    CHECK(g_nestedDrain == 0 && g_nestedTick == Result::WrongThread && sco::QueuedTasks() == 0);

    // Release() from inside a task removes that owner's queued tasks; the drain stops cleanly.
    g_order.clear();
    CHECK(sco::Post(ReleaseB, nullptr) == Result::Ok);
    CHECK(sco::Post(Record, Tag(1), &kTaskB) == Result::Ok);
    CHECK(sco::Post(Record, Tag(2)) == Result::Ok);
    CHECK(sco::Post(Record, Tag(3), &kTaskB) == Result::Ok);
    CHECK(sco::DrainTasks() == 2 && (g_order == std::vector<int>{ 2 }) && sco::QueuedTasks() == 0);
    CHECK(sco::Release(nullptr) == Result::BadArg);
    Result off = Result::Ok;
    std::thread([&] { off = sco::Release(&kTaskB); }).join();
    CHECK(off == Result::WrongThread);

    // Posting from several threads at once: count in == count out.
    std::vector<std::thread> posters;
    std::atomic<int> accepted{ 0 };
    for (int t = 0; t < 4; ++t)
        posters.emplace_back([&] { for (int i = 0; i < 50; ++i) accepted += sco::Post(CountCross, nullptr) == Result::Ok; });
    for (auto& p : posters) p.join();
    CHECK(accepted == 200);
    CHECK(sco::DrainTasks() == 200 && g_crossRan == 200);
}

// ---- event bus ------------------------------------------------------------------------------

struct Seen { std::vector<std::string> events; uint32_t lastNow = 0; int calls = 0; };
static void OnEvent(const char* event, const void* data, void* ctx) {
    Seen* s = static_cast<Seen*>(ctx);
    s->events.emplace_back(event);
    ++s->calls;
    if (strcmp(event, "tick") == 0 && data) s->lastNow = *static_cast<const uint32_t*>(data);
}
static void OnEvent2(const char* e, const void* d, void* ctx) { OnEvent(e, d, ctx); }

static char kOwnerA, kOwnerB;   // opaque owner handles: only their addresses matter
static Seen g_late;
static void SubscribeLate(const char*, const void*, void*) {   // subscribes during a dispatch
    sco::Subscribe(&kOwnerB, "late", OnEvent, &g_late);
    sco::Subscribe(&kOwnerB, "tick", OnEvent2, &g_late);
}
static void UnsubscribeSelf(const char* e, const void*, void* ctx) {
    ++*static_cast<int*>(ctx);
    sco::Unsubscribe(&kOwnerB, e, UnsubscribeSelf);
}

static void CutB(const char*, const void*, void*) { sco::Unsubscribe(&kOwnerB, "cut", OnEvent); }
static void ReleaseOwnerB(const char*, const void*, void*) { sco::Release(&kOwnerB); }

static void TestEvents() {
    const size_t base = sco::SubscriptionCount();
    Seen a, b;
    CHECK(sco::Subscribe(&kOwnerA, nullptr, OnEvent, &a) == Result::BadArg);
    CHECK(sco::Subscribe(&kOwnerA, "", OnEvent, &a) == Result::BadArg);
    CHECK(sco::Subscribe(&kOwnerA, "tick", nullptr, &a) == Result::BadArg);
    CHECK(sco::Subscribe(&kOwnerA, "tick", OnEvent, &a) == Result::Ok);
    CHECK(sco::Subscribe(&kOwnerA, "tick", OnEvent, &a) == Result::BadArg);   // same key twice
    CHECK(sco::Subscribe(&kOwnerB, "tick", OnEvent, &b) == Result::Ok);       // other owner: fine
    CHECK(sco::Subscribe(&kOwnerA, "game.ready", OnEvent, &a) == Result::Ok);
    std::string name = "game.exit";                                            // event name is copied
    CHECK(sco::Subscribe(&kOwnerB, name.c_str(), OnEvent, &b) == Result::Ok);
    name = "xxxx.xxxx";
    CHECK(sco::SubscriptionCount() == base + 4);

    size_t called = 9;
    CHECK(sco::Dispatch("game.ready", nullptr, &called) == Result::Ok && called == 1);
    CHECK(a.events.back() == "game.ready" && b.calls == 0);
    CHECK(sco::Dispatch("game.exit", nullptr, &called) == Result::Ok && called == 1 && b.events.back() == "game.exit");
    CHECK(sco::Dispatch("nobody", nullptr, &called) == Result::Ok && called == 0);
    CHECK(sco::GameThreadTick(1234) == Result::Ok);
    CHECK(a.lastNow == 1234 && b.lastNow == 1234);

    // Tick drains queued tasks before dispatching.
    g_order.clear();
    CHECK(sco::Post(Record, Tag(5)) == Result::Ok);
    CHECK(sco::GameThreadTick(1300) == Result::Ok && (g_order == std::vector<int>{ 5 }) && a.lastNow == 1300);

    // Off the game thread: refused, nobody called.
    const int before = a.calls;
    Result offResult = Result::Ok;
    std::thread([&] { offResult = sco::Dispatch("tick", nullptr); }).join();
    CHECK(offResult == Result::WrongThread && a.calls == before);
    std::thread([&] { offResult = sco::GameThreadTick(1); }).join();
    CHECK(offResult == Result::WrongThread && a.calls == before);

    // Subscribing from another thread is allowed.
    Seen c;
    std::thread([&] { offResult = sco::Subscribe(&kOwnerB, "game.ready", OnEvent, &c); }).join();
    CHECK(offResult == Result::Ok);
    CHECK(sco::Dispatch("game.ready", nullptr, &called) == Result::Ok && called == 2 && c.calls == 1);

    // Unsubscribe.
    CHECK(sco::Unsubscribe(&kOwnerA, "tick", OnEvent) == Result::Ok);
    CHECK(sco::Unsubscribe(&kOwnerA, "tick", OnEvent) == Result::NotFound);
    CHECK(sco::Unsubscribe(&kOwnerA, "tick", OnEvent2) == Result::NotFound);
    CHECK(sco::Dispatch("tick", nullptr, &called) == Result::Ok && called == 1);   // b only

    // Changes made during a dispatch apply from the next one.
    CHECK(sco::Subscribe(&kOwnerA, "late", SubscribeLate, nullptr) == Result::Ok);
    CHECK(sco::Dispatch("late", nullptr, &called) == Result::Ok && called == 1 && g_late.calls == 0);
    CHECK(sco::Dispatch("late", nullptr, &called) == Result::Ok && called == 2 && g_late.calls == 1);
    int selfCalls = 0;
    CHECK(sco::Subscribe(&kOwnerB, "once", UnsubscribeSelf, &selfCalls) == Result::Ok);
    CHECK(sco::Dispatch("once", nullptr, &called) == Result::Ok && called == 1);
    CHECK(sco::Dispatch("once", nullptr, &called) == Result::Ok && called == 0 && selfCalls == 1);

    // Unsubscribe and Release apply at once: a dispatch already running skips the removed ones.
    Seen victim;
    CHECK(sco::Subscribe(&kOwnerA, "cut", CutB, nullptr) == Result::Ok);
    CHECK(sco::Subscribe(&kOwnerB, "cut", OnEvent, &victim) == Result::Ok);
    CHECK(sco::Dispatch("cut", nullptr, &called) == Result::Ok && called == 1 && victim.calls == 0);
    CHECK(sco::Subscribe(&kOwnerA, "cut2", ReleaseOwnerB, nullptr) == Result::Ok);
    CHECK(sco::Subscribe(&kOwnerB, "cut2", OnEvent, &victim) == Result::Ok);
    CHECK(sco::Subscribe(&kOwnerB, "cut2", OnEvent2, &victim) == Result::Ok);
    CHECK(sco::Dispatch("cut2", nullptr, &called) == Result::Ok && called == 1 && victim.calls == 0);
    sco::Unsubscribe(&kOwnerA, "cut", CutB);
    sco::Unsubscribe(&kOwnerA, "cut2", ReleaseOwnerB);

    // Clean up and check the bound.
    sco::Unsubscribe(&kOwnerA, "game.ready", OnEvent);
    sco::Unsubscribe(&kOwnerB, "tick", OnEvent);
    sco::Unsubscribe(&kOwnerB, "game.exit", OnEvent);
    sco::Unsubscribe(&kOwnerB, "game.ready", OnEvent);
    sco::Unsubscribe(&kOwnerA, "late", SubscribeLate);
    sco::Unsubscribe(&kOwnerB, "late", OnEvent);
    sco::Unsubscribe(&kOwnerB, "tick", OnEvent2);
    CHECK(sco::SubscriptionCount() == base);
    std::vector<std::string> names;
    for (size_t i = 0; i < sco::kMaxSubscriptions; ++i) names.push_back("e" + std::to_string(i));
    int ok = 0;
    for (size_t i = base; i < sco::kMaxSubscriptions; ++i) ok += sco::Subscribe(&kOwnerA, names[i].c_str(), OnEvent, &a) == Result::Ok;
    CHECK(ok == static_cast<int>(sco::kMaxSubscriptions - base));
    CHECK(sco::Subscribe(&kOwnerA, "one.more", OnEvent, &a) == Result::TooMany);
    CHECK(sco::Subscribe(&kOwnerA, names[base].c_str(), OnEvent, &a) == Result::BadArg);   // duplicate wins over full
    for (size_t i = base; i < sco::kMaxSubscriptions; ++i) sco::Unsubscribe(&kOwnerA, names[i].c_str(), OnEvent);
    CHECK(sco::SubscriptionCount() == base);
}

// ---- command registry -----------------------------------------------------------------------

struct Call { int64_t i = 0; double f = 0; std::string s; bool b = false; int calls = 0; bool onGame = false; };

static Result CmdSpawn(const sco::Arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t replySize) {
    Call* c = static_cast<Call*>(ctx);
    ++c->calls;
    c->onGame = sco::OnGameThread();
    if (nargs == 4) { c->i = args[0].v.i; c->f = args[1].v.f; c->s = args[2].v.s; c->b = args[3].v.i != 0; }
    snprintf(reply, replySize, "Spawned %s", nargs == 4 ? args[2].v.s : "?");
    return Result::Ok;
}
static Result CmdLong(const sco::Arg*, uint32_t, void*, char* reply, uint32_t replySize) {
    memset(reply, 'x', replySize);   // fills the buffer with no NUL; the host must terminate it
    return Result::NotFound;
}
static Result CmdNop(const sco::Arg*, uint32_t, void* ctx, char*, uint32_t) {
    if (ctx) ++static_cast<Call*>(ctx)->calls;
    return Result::Ok;
}

static const sco::ArgDef kSpawnArgs[] = {
    { "count", sco::ArgType::Int, "how many" },
    { "scale", sco::ArgType::Float, nullptr },
    { "ship",  sco::ArgType::String, "class name" },
    { "armed", sco::ArgType::Bool, nullptr },
};

struct Done { Result r = Result::Crashed; std::string reply; int calls = 0; bool onGame = false; };
static void OnDone(Result r, const char* reply, void* ctx) {
    Done* d = static_cast<Done*>(ctx);
    d->r = r; d->reply = reply; ++d->calls; d->onGame = sco::OnGameThread();
}

static bool g_capSpawn = false;
static bool CapCheck(const char* cap) { return strcmp(cap, "spawn.ship") == 0 && g_capSpawn; }

static sco::Arg A(int64_t i) { sco::Arg a{}; a.type = sco::ArgType::Int; a.v.i = i; return a; }
static sco::Arg F(double f)  { sco::Arg a{}; a.type = sco::ArgType::Float; a.v.f = f; return a; }
static sco::Arg S(const char* s) { sco::Arg a{}; a.type = sco::ArgType::String; a.v.s = s; return a; }
static sco::Arg B(bool b)    { sco::Arg a{}; a.type = sco::ArgType::Bool; a.v.i = b; return a; }

static char kOwnerH, kBulk;   // plugin owners for "hello" and "bulk"

static void TestCommands() {
    Call spawn, nop;
    const sco::Command cSpawn{ "spawn.ship", "Spawn ship", "Spawns a ship", "spawn.ship", kSpawnArgs, 4, CmdSpawn, &spawn };
    const sco::Command cLong{ "test.long", "Long", nullptr, nullptr, nullptr, 0, CmdLong, nullptr };
    const sco::Command cHello{ "hello.wave", "Wave", nullptr, nullptr, nullptr, 0, CmdNop, &nop };

    // Name and shape rules.
    auto named = [](const char* n) { return sco::Command{ n, n, nullptr, nullptr, nullptr, 0, CmdNop, nullptr }; };
    CHECK(sco::RegisterCommand(nullptr, nullptr, named(nullptr)) == Result::BadArg);
    CHECK(sco::RegisterCommand(nullptr, nullptr, named("")) == Result::BadArg);
    CHECK(sco::RegisterCommand(nullptr, nullptr, named("nodot")) == Result::BadArg);
    CHECK(sco::RegisterCommand(nullptr, nullptr, named(".lead")) == Result::BadArg);
    CHECK(sco::RegisterCommand(nullptr, nullptr, named("trail.")) == Result::BadArg);
    CHECK(sco::RegisterCommand(nullptr, nullptr, named("a..b")) == Result::BadArg);
    CHECK(sco::RegisterCommand(nullptr, nullptr, named("Spawn.Ship")) == Result::BadArg);
    CHECK(sco::RegisterCommand(nullptr, nullptr, named("spawn ship.x")) == Result::BadArg);
    sco::Command noFn = cHello; noFn.fn = nullptr;
    CHECK(sco::RegisterCommand(nullptr, nullptr, noFn) == Result::BadArg);
    sco::Command tooMany = cSpawn; tooMany.nargs = sco::kMaxCommandArgs + 1;
    CHECK(sco::RegisterCommand(nullptr, nullptr, tooMany) == Result::BadArg);
    sco::Command noArgs = cSpawn; noArgs.args = nullptr;
    CHECK(sco::RegisterCommand(nullptr, nullptr, noArgs) == Result::BadArg);
    const sco::ArgDef badDef[] = { { nullptr, sco::ArgType::Int, nullptr } };
    sco::Command badArg = cHello; badArg.args = badDef; badArg.nargs = 1;
    CHECK(sco::RegisterCommand(nullptr, nullptr, badArg) == Result::BadArg);
    // Plugin owners may only use their own prefix.
    CHECK(sco::RegisterCommand(&kOwnerH, "hello", named("spawn.ship")) == Result::BadArg);
    CHECK(sco::RegisterCommand(&kOwnerH, "hell", cHello) == Result::BadArg);
    CHECK(sco::RegisterCommand(&kOwnerH, "hello.wave", cHello) == Result::BadArg);
    CHECK(sco::RegisterCommand(nullptr, "hello", cHello) == Result::BadArg);   // a prefix needs an owner
    CHECK(sco::ListCommands(nullptr, 0) == 0);   // nothing registered by the failures

    CHECK(sco::RegisterCommand(nullptr, nullptr, cSpawn) == Result::Ok);
    CHECK(sco::RegisterCommand(&kOwnerH, "hello", cHello) == Result::Ok);
    CHECK(sco::RegisterCommand(nullptr, nullptr, cLong) == Result::Ok);
    CHECK(sco::RegisterCommand(nullptr, nullptr, cSpawn) == Result::BadArg);   // duplicate
    const sco::Command* list[8] = {};
    CHECK(sco::ListCommands(list, 8) == 3);
    CHECK(strcmp(list[0]->name, "spawn.ship") == 0 && strcmp(list[1]->name, "hello.wave") == 0 && list[0]->nargs == 4);
    const sco::Command* one[1] = {};
    CHECK(sco::ListCommands(one, 1) == 3 && one[0] == list[0]);

    // Invoke on the game thread: runs now, done before return.
    const sco::Arg good[] = { A(2), F(1.5), S("Cutlass Black"), B(true) };
    Done d;
    CHECK(sco::Invoke("spawn.ship", good, 4, OnDone, &d) == Result::Unavailable);   // no capability check yet
    CHECK(d.calls == 1 && d.r == Result::Unavailable && spawn.calls == 0);
    sco::SetCapabilityCheck(CapCheck);
    CHECK(sco::Invoke("spawn.ship", good, 4, OnDone, &d) == Result::Unavailable);   // check says no
    g_capSpawn = true;
    CHECK(sco::Invoke("spawn.ship", good, 4, OnDone, &d) == Result::Ok);
    CHECK(d.calls == 3 && d.r == Result::Ok && d.reply == "Spawned Cutlass Black" && d.onGame);
    CHECK(spawn.calls == 1 && spawn.i == 2 && spawn.f == 1.5 && spawn.s == "Cutlass Black" && spawn.b && spawn.onGame);
    CHECK(sco::Invoke("hello.wave", nullptr, 0, nullptr, nullptr) == Result::Ok && nop.calls == 1);   // done optional

    // Argument checks happen before fn runs.
    const sco::Arg wrongType[] = { A(2), A(1), S("x"), B(true) };
    const sco::Arg nullStr[] = { A(2), F(1.0), S(nullptr), B(true) };
    CHECK(sco::Invoke("spawn.ship", good, 3, OnDone, &d) == Result::BadArg);
    CHECK(sco::Invoke("spawn.ship", wrongType, 4, OnDone, &d) == Result::BadArg);
    CHECK(sco::Invoke("spawn.ship", nullStr, 4, OnDone, &d) == Result::BadArg && d.reply.empty());
    CHECK(sco::Invoke("no.such", nullptr, 0, OnDone, &d) == Result::NotFound && d.r == Result::NotFound);
    CHECK(sco::Invoke(nullptr, nullptr, 0, OnDone, &d) == Result::BadArg);
    CHECK(sco::Invoke("spawn.ship", nullptr, 4, OnDone, &d) == Result::BadArg);
    CHECK(spawn.calls == 1);

    // fn's result passes through and the reply is always terminated.
    CHECK(sco::Invoke("test.long", nullptr, 0, OnDone, &d) == Result::NotFound);
    CHECK(d.reply.size() == sco::kReplySize - 1);

    // From another thread: queued, args copied, done runs once on the game thread.
    Done q;
    Result r = Result::Crashed;
    std::thread([&] {
        char ship[32] = "Gladius";
        const sco::Arg args[] = { A(1), F(2.0), S(ship), B(false) };
        r = sco::Invoke("spawn.ship", args, 4, OnDone, &q);
        strcpy(ship, "OVERWRITTEN");   // the caller's buffer may change once Invoke returns
    }).join();
    CHECK(r == Result::Ok && q.calls == 0 && spawn.calls == 1);
    CHECK(sco::GameThreadTick(5) == Result::Ok);
    CHECK(q.calls == 1 && q.r == Result::Ok && q.onGame && q.reply == "Spawned Gladius");
    CHECK(spawn.calls == 2 && spawn.s == "Gladius" && !spawn.b && spawn.onGame);
    Done qe;
    std::thread([&] { r = sco::Invoke("no.such", nullptr, 0, OnDone, &qe); }).join();
    CHECK(r == Result::Ok);   // the outcome arrives in done
    CHECK(sco::DrainTasks() == 1 && qe.calls == 1 && qe.r == Result::NotFound);
    std::thread([&] { r = sco::Invoke(nullptr, nullptr, 0, OnDone, &qe); }).join();
    CHECK(r == Result::BadArg && sco::QueuedTasks() == 0 && qe.calls == 1);

    // Off-thread Invoke on a full queue: refused, done never called, nothing leaks (ASan).
    for (size_t i = 0; i < sco::kMaxQueuedTasks; ++i) sco::Post(CountCross, nullptr);
    Done full;
    std::thread([&] { r = sco::Invoke("hello.wave", nullptr, 0, OnDone, &full); }).join();
    CHECK(r == Result::TooMany);
    CHECK(sco::DrainTasks() == sco::kMaxQueuedTasks && full.calls == 0);

    // Bool must be 0 or 1.
    sco::Arg badBool[] = { A(2), F(1.0), S("x"), B(true) };
    badBool[3].v.i = 2;
    CHECK(sco::Invoke("spawn.ship", badBool, 4, OnDone, &d) == Result::BadArg && spawn.calls == 2);

    // The registry copies every string and the arg defs.
    char nm[] = "copy.me", ti[] = "Copy me", an[] = "count";
    sco::ArgDef defs[] = { { an, sco::ArgType::Int, nullptr } };
    const sco::Command cCopy{ nm, ti, nullptr, nullptr, defs, 1, CmdNop, nullptr };
    CHECK(sco::RegisterCommand(nullptr, nullptr, cCopy) == Result::Ok);
    strcpy(nm, "xxxx.xx"); strcpy(ti, "garbage"); strcpy(an, "zzzzz"); defs[0].type = sco::ArgType::String;
    const sco::Arg one1[] = { A(1) };
    CHECK(sco::Invoke("copy.me", one1, 1, nullptr, nullptr) == Result::Ok);
    const sco::Command* cl[16] = {};
    const size_t nl = sco::ListCommands(cl, 16);
    bool foundCopy = false;
    for (size_t i = 0; i < nl && i < 16; ++i)
        if (strcmp(cl[i]->name, "copy.me") == 0)
            foundCopy = strcmp(cl[i]->title, "Copy me") == 0 && strcmp(cl[i]->args[0].name, "count") == 0 &&
                        cl[i]->args[0].type == sco::ArgType::Int && cl[i]->args != defs;
    CHECK(foundCopy);
    std::string longName = "too." + std::string(sco::kMaxNameLen, 'a');
    CHECK(sco::RegisterCommand(nullptr, nullptr, named(longName.c_str())) == Result::BadArg);

    // Reserved prefixes, and a prefix already in use by another owner.
    static char kOther;
    CHECK(sco::RegisterCommand(&kOther, "menu", named("menu.open")) == Result::BadArg);
    CHECK(sco::RegisterCommand(&kOther, "sco", named("sco.x")) == Result::BadArg);
    CHECK(sco::RegisterCommand(&kOther, "sco.x", named("sco.x.y")) == Result::BadArg);          // dotted prefix
    CHECK(sco::RegisterCommand(&kOther, "hello.evil", named("hello.evil.z")) == Result::BadArg);
    CHECK(sco::RegisterCommand(&kOther, "spawn", named("spawn.other")) == Result::BadArg);   // host owns spawn.*
    CHECK(sco::RegisterCommand(&kOther, "hello", named("hello.other")) == Result::BadArg);   // kOwnerH owns hello.*
    CHECK(sco::RegisterCommand(&kOwnerH, "hello", named("hello.again")) == Result::Ok);       // same owner: fine

    // Release: commands gone, the name is free again, a queued off-thread invoke is dropped
    // without running or calling done, and nothing leaks (ASan).
    Done dropped;
    std::thread([&] { r = sco::Invoke("hello.wave", nullptr, 0, OnDone, &dropped, &kOwnerH); }).join();
    CHECK(r == Result::Ok && sco::QueuedTasks() == 1);
    const size_t before = sco::ListCommands(nullptr, 0);
    size_t removed = 0;
    CHECK(sco::Release(&kOwnerH, &removed) == Result::Ok && removed == 3);   // 2 commands + 1 task
    CHECK(sco::ListCommands(nullptr, 0) == before - 2 && sco::QueuedTasks() == 0);
    CHECK(sco::DrainTasks() == 0 && dropped.calls == 0 && nop.calls == 1);
    CHECK(sco::Invoke("hello.wave", nullptr, 0, OnDone, &d) == Result::NotFound);
    CHECK(sco::RegisterCommand(&kOther, "hello", cHello) == Result::Ok);   // prefix free again
    CHECK(sco::Invoke("hello.wave", nullptr, 0, nullptr, nullptr) == Result::Ok && nop.calls == 2);
    CHECK(sco::Release(&kOther) == Result::Ok);

    // Release is final: the released owner can't add anything back, from any thread.
    CHECK(sco::Release(&kOwnerH) == Result::BadArg);
    CHECK(sco::Post(Record, Tag(1), &kOwnerH) == Result::BadArg);
    CHECK(sco::Subscribe(&kOwnerH, "tick", OnEvent, nullptr) == Result::BadArg);
    CHECK(sco::RegisterCommand(&kOwnerH, "hello", named("hello.back")) == Result::BadArg);
    CHECK(sco::Invoke("spawn.ship", good, 4, OnDone, &d, &kOwnerH) == Result::BadArg);
    std::thread([&] { r = sco::Post(Record, Tag(1), &kOwnerH); }).join();
    CHECK(r == Result::BadArg && sco::QueuedTasks() == 0);

    // Registering from another thread while the game thread lists. 23 slots are used so far
    // (TestOverlap's 17 and 6 here; released ones count too: slots never move).
    constexpr size_t kUsed = 23;
    const size_t liveBefore = sco::ListCommands(nullptr, 0);   // spawn.ship, test.long, copy.me
    std::vector<std::string> names;
    for (size_t i = 0; i < sco::kMaxCommands; ++i) names.push_back("bulk.c" + std::to_string(i));
    std::atomic<int> ok{ 0 };
    std::thread reg([&] {
        for (size_t i = kUsed; i < sco::kMaxCommands; ++i)
            ok += sco::RegisterCommand(&kBulk, "bulk", named(names[i].c_str())) == Result::Ok;
    });
    const size_t want = liveBefore + sco::kMaxCommands - kUsed;
    size_t seen = 0;
    bool monotonic = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (seen < want && std::chrono::steady_clock::now() < deadline) {
        const size_t n = sco::ListCommands(nullptr, 0);
        monotonic = monotonic && n >= seen;
        seen = n;
        std::this_thread::yield();
    }
    reg.join();
    CHECK(liveBefore == 3 && monotonic);
    CHECK(ok == static_cast<int>(sco::kMaxCommands - kUsed));
    CHECK(sco::RegisterCommand(&kBulk, "bulk", named("bulk.over")) == Result::TooMany);
    std::vector<const sco::Command*> all(sco::kMaxCommands);
    CHECK(sco::ListCommands(all.data(), all.size()) == want);
    CHECK(strcmp(all[want - 1]->name, names.back().c_str()) == 0);
    CHECK(sco::Invoke("bulk.c100", nullptr, 0, nullptr, nullptr) == Result::Ok);
    CHECK(sco::Release(&kBulk) == Result::Ok && sco::ListCommands(nullptr, 0) == liveBefore);
}


// ---- real overlap (meant for the ThreadSanitizer build) -------------------------------------

constexpr uint32_t kMagic = 0x5C0FFEE;
struct Box { std::atomic<uint32_t> magic{ kMagic }; std::atomic<int> calls{ 0 }; };
static std::atomic<int> g_badBox{ 0 };
static void UseBox(const char*, const void*, void* ctx) {
    Box* b = static_cast<Box*>(ctx);
    if (b->magic.load() != kMagic) g_badBox.fetch_add(1);
    std::this_thread::yield();   // widen the window in which ctx is in use
    if (b->magic.load() != kMagic) g_badBox.fetch_add(1);
    b->calls.fetch_add(1);
}
static void FreeBox(void* ctx) {
    Box* b = static_cast<Box*>(ctx);
    b->magic.store(0);
    delete b;
}
static std::atomic<int> g_concRan{ 0 };
static void CountConc(void*) { g_concRan.fetch_add(1); }
static Result CmdCount(const sco::Arg*, uint32_t, void*, char* reply, uint32_t size) {
    snprintf(reply, size, "ok");
    return Result::Ok;
}
static std::atomic<int> g_concDone{ 0 };
static void ConcDone(Result r, const char*, void*) { if (r == Result::Ok) g_concDone.fetch_add(1); }

static void TestOverlap() {
    using namespace std::chrono_literals;
    static char kConc;
    const sco::Command cmd{ "conc.count", "Count", nullptr, nullptr, nullptr, 0, CmdCount, nullptr };
    CHECK(sco::RegisterCommand(&kConc, "conc", cmd) == Result::Ok);

    // A producer posts tasks and invokes while the game thread ticks; a second thread
    // subscribes, unsubscribes and frees ctx through the documented safe point.
    std::atomic<bool> stop{ false }, producerDone{ false }, churnDone{ false };
    std::atomic<int> posted{ 0 }, invoked{ 0 };
    std::thread producer([&] {
        for (int i = 0; i < 2000; ++i) {
            if (sco::Post(CountConc, nullptr) == Result::Ok) posted.fetch_add(1);
            if (sco::Invoke("conc.count", nullptr, 0, ConcDone, nullptr) == Result::Ok) invoked.fetch_add(1);
            if (i % 64 == 0) std::this_thread::yield();
        }
        producerDone.store(true);
    });
    static char kChurn;
    std::atomic<int> boxes{ 0 };
    std::thread churn([&] {
        for (int i = 0; i < 300; ++i) {
            Box* b = new Box;
            if (sco::Subscribe(&kChurn, "tick", UseBox, b) != Result::Ok) { delete b; continue; }
            std::this_thread::sleep_for(50us);
            sco::Unsubscribe(&kChurn, "tick", UseBox);
            while (sco::Post(FreeBox, b) != Result::Ok) std::this_thread::yield();   // the safe point
            boxes.fetch_add(1);
        }
        churnDone.store(true);
    });
    // A reader lists commands with a real buffer and reads their strings while a writer registers.
    static char kLate;
    std::atomic<int> readBad{ 0 };
    std::thread reader([&] {
        const sco::Command* out[64];
        while (!stop.load()) {
            const size_t n = sco::ListCommands(out, 64);
            for (size_t i = 0; i < n && i < 64; ++i)
                if (!out[i]->name || !strchr(out[i]->name, '.') || !out[i]->title) readBad.fetch_add(1);
        }
    });
    std::thread writer([&] {
        char name[32];
        for (int i = 0; i < 16; ++i) {
            snprintf(name, sizeof name, "late.c%d", i);
            const sco::Command c{ name, name, nullptr, nullptr, nullptr, 0, CmdCount, nullptr };
            sco::RegisterCommand(&kLate, "late", c);
            std::this_thread::yield();
        }
    });
    // The game thread keeps ticking until both workers finish (or 4 s pass), then drains.
    uint32_t now = 0;
    const auto until = std::chrono::steady_clock::now() + 4s;
    while ((!producerDone.load() || !churnDone.load()) && std::chrono::steady_clock::now() < until)
        sco::GameThreadTick(++now);
    producer.join();
    churn.join();
    stop.store(true);
    reader.join();
    writer.join();
    while (sco::QueuedTasks()) sco::GameThreadTick(++now);
    CHECK(g_concRan.load() == posted.load() && posted.load() > 0);
    CHECK(g_concDone.load() == invoked.load() && invoked.load() > 0);
    CHECK(boxes.load() == 300 && g_badBox.load() == 0);
    CHECK(readBad.load() == 0);
    size_t removed = 0;
    CHECK(sco::Release(&kLate, &removed) == Result::Ok && removed == 16);

    // A thread keeps posting and invoking for an owner while the game thread releases it:
    // nothing of that owner runs after Release returns.
    static char kDoomed;
    g_concRan.store(0);
    g_concDone.store(0);
    std::atomic<bool> go{ true };
    std::thread spammer([&] {
        while (go.load()) {
            sco::Post(CountConc, nullptr, &kDoomed);
            sco::Invoke("conc.count", nullptr, 0, ConcDone, nullptr, &kDoomed);
            std::this_thread::yield();
        }
    });
    for (const auto stopAt = std::chrono::steady_clock::now() + 2s;
         g_concRan.load() < 50 && std::chrono::steady_clock::now() < stopAt;)
        sco::GameThreadTick(++now);
    CHECK(sco::Release(&kDoomed) == Result::Ok);
    const int ranAtRelease = g_concRan.load(), doneAtRelease = g_concDone.load();
    for (int i = 0; i < 200; ++i) sco::GameThreadTick(++now);
    go.store(false);
    spammer.join();
    while (sco::QueuedTasks()) sco::GameThreadTick(++now);
    CHECK(g_concRan.load() == ranAtRelease && g_concDone.load() == doneAtRelease);

    // No cap on released owners: the 200th release still removes its owner's subscription.
    static char kMany[200];
    int manyOk = 0;
    Box* box = new Box;
    for (int i = 0; i < 200; ++i) {
        sco::Subscribe(&kMany[i], "tick", UseBox, box);
        manyOk += sco::Release(&kMany[i]) == Result::Ok;
    }
    const int boxCalls = box->calls.load();
    sco::GameThreadTick(++now);
    CHECK(manyOk == 200 && box->calls.load() == boxCalls);
    delete box;
    CHECK(sco::Release(&kConc) == Result::Ok);
}

int main() {
    TestBeforeGameThread();   // first: checks the "no game thread yet" state
    TestTaskQueue();
    TestEvents();
    TestOverlap();    // before TestCommands, which fills every command slot
    TestCommands();
    std::printf("sco-core runtime tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
