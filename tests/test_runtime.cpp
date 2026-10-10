// Unit tests for the game-thread runtime: task queue, event bus, command registry.
// Host build, no game needed. The "game thread" is this test's main thread.
//   tools/test.sh
#include "sco/runtime.h"
#include "../src/api/internal.h"   // detail::InvokeOwned
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
    CHECK(strcmp(sco::ResultName(Result::Failed), "FAILED") == 0);
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

static char kTaskSelf;
static int g_selfRuns = 0;
static void SelfRepost(void*) {   // posts itself again every time it runs
    ++g_selfRuns;
    CHECK(sco::Post(SelfRepost, nullptr, &kTaskSelf) == Result::Ok);
}

static std::vector<int> g_dropped;
static void RecordDrop(void* ctx) { g_dropped.push_back(static_cast<int>(reinterpret_cast<intptr_t>(ctx))); }
static char kTaskC;

static int g_counted = 0;
static void Count(void*) { ++g_counted; }

// 4 threads post 2,500 tasks each while the game thread drains: every task runs exactly once and
// each thread's tasks run in the order that thread posted them.
static void TestTasksManyThreads() {
    constexpr int kThreads = 4, kEach = 2500;
    g_order.clear();
    std::atomic<int> done{ 0 }, refused{ 0 };
    std::vector<std::thread> posters;
    for (int t = 0; t < kThreads; ++t)
        posters.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i)
                if (sco::Post(Record, Tag(t * 100000 + i)) != Result::Ok) ++refused;
            ++done;
        });
    size_t ran = 0;
    while (done.load() < kThreads) ran += sco::DrainTasks();
    for (auto& p : posters) p.join();
    ran += sco::DrainTasks();
    CHECK(refused == 0);
    CHECK(ran == static_cast<size_t>(kThreads * kEach) && g_order.size() == ran && sco::QueuedTasks() == 0);
    int next[kThreads] = {};
    bool ordered = true;
    for (const int v : g_order) {
        const int t = v / 100000, i = v % 100000;
        if (t < 0 || t >= kThreads || i != next[t]) { ordered = false; break; }
        ++next[t];
    }
    CHECK(ordered);
    for (int t = 0; t < kThreads; ++t) CHECK(next[t] == kEach);
}

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

    // Past the ring: posts go to the overflow and run after the ring, in posting order.
    g_order.clear();
    const size_t kPast = sco::kMaxQueuedTasks + 100;
    int ok = 0;
    for (size_t i = 0; i < kPast; ++i) ok += sco::Post(Record, Tag(static_cast<int>(i))) == Result::Ok;
    CHECK(ok == static_cast<int>(kPast) && sco::QueuedTasks() == kPast);
    CHECK(sco::DrainTasks() == kPast);
    bool inOrder = g_order.size() == kPast;
    for (size_t i = 0; inOrder && i < g_order.size(); ++i) inOrder = g_order[i] == static_cast<int>(i);
    CHECK(inOrder);
    CHECK(sco::Post(Record, Tag(7)) == Result::Ok);   // back in the ring (wrapped)
    CHECK(sco::DrainTasks() == 1 && g_order.back() == 7);

    // FIFO across the boundary: a task posted mid-drain, when the ring has room again but the
    // overflow still holds older tasks, queues behind the overflow and waits for the next drain.
    g_order.clear();
    CHECK(sco::Post(PostAgain, Tag(-2)) == Result::Ok);
    for (int i = 0; i < static_cast<int>(sco::kMaxQueuedTasks) + 9; ++i) CHECK(sco::Post(Record, Tag(i)) == Result::Ok);
    CHECK(sco::DrainTasks() == sco::kMaxQueuedTasks + 10 && sco::QueuedTasks() == 1);
    inOrder = g_order.size() == sco::kMaxQueuedTasks + 10 && g_order[0] == -2;
    for (size_t i = 1; inOrder && i < g_order.size(); ++i) inOrder = g_order[i] == static_cast<int>(i - 1);
    CHECK(inOrder);
    CHECK(sco::DrainTasks() == 1 && g_order.back() == 99);

    // A task that re-posts itself runs once per drain, never twice in one.
    g_selfRuns = 0;
    CHECK(sco::Post(SelfRepost, nullptr, &kTaskSelf) == Result::Ok);
    CHECK(sco::DrainTasks() == 1 && g_selfRuns == 1 && sco::QueuedTasks() == 1);
    CHECK(sco::DrainTasks() == 1 && g_selfRuns == 2 && sco::QueuedTasks() == 1);

    // Release drops an owner's tasks from the ring and the overflow, running their drop
    // callbacks in posting order; the rest run in order.
    g_order.clear();
    g_dropped.clear();
    for (int i = 0; i < 200; ++i) CHECK(sco::Post(Record, Tag(i)) == Result::Ok);
    for (int i = 0; i < 100; ++i) {   // with the SelfRepost task: 28 in the ring, 72 in the overflow
        CHECK(sco::detail::PostOwned(Record, RecordDrop, Tag(1000 + i), &kTaskC) == Result::Ok);
        CHECK(sco::Post(Record, Tag(200 + i)) == Result::Ok);
    }
    CHECK(sco::QueuedTasks() == 401);
    size_t removed = 0;
    CHECK(sco::Release(&kTaskC, &removed) == Result::Ok && removed == 100);
    inOrder = g_dropped.size() == 100;
    for (size_t i = 0; inOrder && i < g_dropped.size(); ++i) inOrder = g_dropped[i] == 1000 + static_cast<int>(i);
    CHECK(inOrder);
    CHECK(sco::QueuedTasks() == 301);
    CHECK(sco::detail::PostOwned(Record, RecordDrop, Tag(1), &kTaskC) == Result::BadArg);
    CHECK(sco::DrainTasks() == 301 && g_selfRuns == 3);
    inOrder = g_order.size() == 300;
    for (size_t i = 0; inOrder && i < g_order.size(); ++i) inOrder = g_order[i] == static_cast<int>(i);
    CHECK(inOrder);
    CHECK(sco::Release(&kTaskSelf, &removed) == Result::Ok && removed == 1 && sco::QueuedTasks() == 0);

    // Hard cap: kMaxQueuedTasksHard waiting in all, then TooMany with nothing queued; one drain
    // runs them all.
    g_counted = 0;
    ok = 0;
    for (size_t i = 0; i < sco::kMaxQueuedTasksHard; ++i) ok += sco::Post(Count, nullptr) == Result::Ok;
    CHECK(ok == static_cast<int>(sco::kMaxQueuedTasksHard) && sco::QueuedTasks() == sco::kMaxQueuedTasksHard);
    CHECK(sco::Post(Count, nullptr) == Result::TooMany);
    CHECK(sco::QueuedTasks() == sco::kMaxQueuedTasksHard);
    CHECK(sco::DrainTasks() == sco::kMaxQueuedTasksHard && g_counted == static_cast<int>(sco::kMaxQueuedTasksHard));
    CHECK(sco::QueuedTasks() == 0);

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
    // A capability must be a capability name (caps::Set's rule), or it could never be granted.
    for (const char* cap : { "", "Teleport", "a..b", ".a", "a.", "tele port" }) {
        sco::Command badCap = cHello;
        badCap.capability = cap;
        CHECK(sco::RegisterCommand(&kOwnerH, "hello", badCap) == Result::BadArg);
    }
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

    // Off-thread Invoke on a full queue (the hard cap): refused, done never called, nothing
    // leaks (ASan).
    for (size_t i = 0; i < sco::kMaxQueuedTasksHard; ++i) sco::Post(CountCross, nullptr);
    Done full;
    std::thread([&] { r = sco::Invoke("hello.wave", nullptr, 0, OnDone, &full); }).join();
    CHECK(r == Result::TooMany);
    CHECK(sco::DrainTasks() == sco::kMaxQueuedTasksHard && full.calls == 0);

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

    // Registering from another thread while the game thread lists: up to the cap on LIVE commands.
    const size_t liveBefore = sco::ListCommands(nullptr, 0);   // spawn.ship, test.long, copy.me
    const size_t kBulkCount = sco::kMaxCommands - liveBefore;
    std::vector<std::string> names;
    for (size_t i = 0; i < kBulkCount; ++i) names.push_back("bulk.c" + std::to_string(i));
    std::atomic<int> ok{ 0 };
    std::thread reg([&] {
        for (size_t i = 0; i < kBulkCount; ++i)
            ok += sco::RegisterCommand(&kBulk, "bulk", named(names[i].c_str())) == Result::Ok;
    });
    const size_t want = sco::kMaxCommands;
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
    CHECK(ok == static_cast<int>(kBulkCount));
    CHECK(sco::RegisterCommand(&kBulk, "bulk", named("bulk.over")) == Result::TooMany);
    std::vector<const sco::Command*> all(sco::kMaxCommands);
    CHECK(sco::ListCommands(all.data(), all.size()) == want);
    CHECK(strcmp(all[want - 1]->name, names.back().c_str()) == 0);
    CHECK(sco::Invoke("bulk.c100", nullptr, 0, nullptr, nullptr) == Result::Ok);
    CHECK(sco::Release(&kBulk) == Result::Ok && sco::ListCommands(nullptr, 0) == liveBefore);
}


// ---- command slots are reused ---------------------------------------------------------------

static Result CmdTag(const sco::Arg*, uint32_t, void* ctx, char* reply, uint32_t size) {
    snprintf(reply, size, "%s", static_cast<const char*>(ctx));
    return Result::Ok;
}

static const sco::Command* ListedByName(const char* name) {
    std::vector<const sco::Command*> all(sco::kMaxCommands);
    const size_t n = sco::ListCommands(all.data(), all.size());
    const sco::Command* found = nullptr;
    for (size_t i = 0; i < n && i < all.size(); ++i)
        if (strcmp(all[i]->name, name) == 0) found = all[i];
    return found;
}

static char kCycle[2 * sco::kMaxCommands + 7];   // one owner per load: a released owner is final
static char kStale, kFresh, kOrdA, kOrdB, kOrdC, kOrdD;
static char kFill[2];

static void TestCommandRecycling() {
    const size_t base = sco::ListCommands(nullptr, 0);   // host features' commands, never released
    const auto nop = [](const char* name) { return sco::Command{ name, name, nullptr, nullptr, nullptr, 0, CmdNop, nullptr }; };

    // Load and unload for far more than kMaxCommands registrations in all: each round registers two
    // commands under names the previous round freed, runs one, and releases both.
    size_t goodRounds = 0;
    for (size_t i = 0; i < sizeof(kCycle); ++i) {
        size_t removed = 0;
        const bool ok = sco::RegisterCommand(&kCycle[i], "cyc", nop("cyc.a")) == Result::Ok &&
                        sco::RegisterCommand(&kCycle[i], "cyc", nop("cyc.b")) == Result::Ok &&
                        sco::Invoke("cyc.a", nullptr, 0, nullptr, nullptr) == Result::Ok &&
                        sco::ListCommands(nullptr, 0) == base + 2 &&
                        sco::Release(&kCycle[i], &removed) == Result::Ok && removed == 2;
        goodRounds += ok;
    }
    CHECK(2 * sizeof(kCycle) > sco::kMaxCommands && goodRounds == sizeof(kCycle));
    CHECK(sco::ListCommands(nullptr, 0) == base);

    // A released command's name answers NotFound; the same name registered again is a fresh
    // record, with none of the old command's help, args, ctx or title.
    static char kOldTag[] = "old", kNewTag[] = "new";
    const sco::ArgDef oldArgs[] = { { "count", sco::ArgType::Int, "how many" } };
    const sco::Command oldCmd{ "stale.cmd", "Old title", "old help", nullptr, oldArgs, 1, CmdTag, kOldTag };
    CHECK(sco::RegisterCommand(&kStale, "stale", oldCmd) == Result::Ok);
    const sco::Command* oldView = ListedByName("stale.cmd");
    CHECK(oldView && strcmp(oldView->title, "Old title") == 0 && oldView->nargs == 1);
    const sco::Arg one[] = { A(1) };
    Done d;
    CHECK(sco::Invoke("stale.cmd", one, 1, OnDone, &d) == Result::Ok && d.reply == "old");
    CHECK(sco::Release(&kStale) == Result::Ok);
    d = Done{};
    CHECK(sco::Invoke("stale.cmd", one, 1, OnDone, &d) == Result::NotFound && d.r == Result::NotFound);
    CHECK(ListedByName("stale.cmd") == nullptr && sco::ListCommands(nullptr, 0) == base);
    const sco::Command newCmd{ "stale.cmd", "New title", nullptr, nullptr, nullptr, 0, CmdTag, kNewTag };
    CHECK(sco::RegisterCommand(&kFresh, "stale", newCmd) == Result::Ok);
    const sco::Command* newView = ListedByName("stale.cmd");
    CHECK(newView && strcmp(newView->title, "New title") == 0 && newView->help == nullptr &&
          newView->capability == nullptr && newView->args == nullptr && newView->nargs == 0);
    d = Done{};
    CHECK(sco::Invoke("stale.cmd", nullptr, 0, OnDone, &d) == Result::Ok && d.reply == "new");
    CHECK(sco::Invoke("stale.cmd", one, 1, nullptr, nullptr) == Result::BadArg);   // the old arg list is gone
    CHECK(sco::Release(&kFresh) == Result::Ok);

    // The cap is on live commands: fill it, one more is TooMany, releasing frees every slot, and it
    // fills again (through reused slots).
    for (char& owner : kFill) {
        const size_t room = sco::kMaxCommands - base;
        std::vector<std::string> names;
        for (size_t i = 0; i < room; ++i) names.push_back("fill.c" + std::to_string(i));
        size_t ok = 0;
        for (const std::string& n : names) ok += sco::RegisterCommand(&owner, "fill", nop(n.c_str())) == Result::Ok;
        CHECK(ok == room && sco::ListCommands(nullptr, 0) == sco::kMaxCommands);
        CHECK(sco::RegisterCommand(&owner, "fill", nop("fill.over")) == Result::TooMany);
        size_t removed = 0;
        CHECK(sco::Release(&owner, &removed) == Result::Ok && removed == room && sco::ListCommands(nullptr, 0) == base);
    }

    // ListCommands keeps registration order across reuse: a later registration that lands in an
    // earlier slot still lists last.
    CHECK(sco::RegisterCommand(&kOrdA, "oa", nop("oa.x")) == Result::Ok);
    CHECK(sco::RegisterCommand(&kOrdB, "ob", nop("ob.x")) == Result::Ok);
    CHECK(sco::RegisterCommand(&kOrdC, "oc", nop("oc.x")) == Result::Ok);
    CHECK(sco::Release(&kOrdB) == Result::Ok);
    CHECK(sco::RegisterCommand(&kOrdD, "od", nop("od.x")) == Result::Ok);
    std::vector<const sco::Command*> all(sco::kMaxCommands);
    CHECK(sco::ListCommands(all.data(), all.size()) == base + 3);
    CHECK(strcmp(all[base]->name, "oa.x") == 0 && strcmp(all[base + 1]->name, "oc.x") == 0 &&
          strcmp(all[base + 2]->name, "od.x") == 0);
    CHECK(sco::Release(&kOrdA) == Result::Ok && sco::Release(&kOrdC) == Result::Ok && sco::Release(&kOrdD) == Result::Ok);
    CHECK(sco::ListCommands(nullptr, 0) == base);
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

// ---- callout guard --------------------------------------------------------------------------

struct GuardSeen { const void* owner; std::string where; };
static std::vector<GuardSeen> g_guardSeen;
static bool g_guardFaults = false;   // simulate a fault: report it without running the call
static bool TestGuard(const void* owner, const char* where, sco::TaskFn thunk, void* ctx) {
    g_guardSeen.push_back({ owner, where });
    if (g_guardFaults) return false;
    thunk(ctx);
    return true;
}
static bool GuardSaw(std::vector<GuardSeen> want) {
    if (want.size() != g_guardSeen.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (want[i].owner != g_guardSeen[i].owner || want[i].where != g_guardSeen[i].where) return false;
    return true;
}

static void TestCalloutGuard() {
    static char kGuarded, kCaller;
    Call cmd;
    const sco::Command c{ "guard.cmd", "Guarded", nullptr, nullptr, nullptr, 0, CmdNop, &cmd };
    CHECK(sco::RegisterCommand(&kGuarded, "guard", c) == Result::Ok);
    sco::SetCalloutGuard(TestGuard);

    // Commands run as their owner, done as the invoking owner.
    g_guardSeen.clear();
    Done d;
    CHECK(sco::Invoke("guard.cmd", nullptr, 0, OnDone, &d, &kCaller) == Result::Ok);
    CHECK(cmd.calls == 1 && d.calls == 1 && d.r == Result::Ok);
    CHECK(GuardSaw({ { &kGuarded, "guard.cmd" }, { &kCaller, "invoke done" } }));

    // A faulting command answers Crashed; done (no owner here) bypasses the guard and still runs.
    g_guardSeen.clear();
    g_guardFaults = true;
    CHECK(sco::Invoke("guard.cmd", nullptr, 0, OnDone, &d) == Result::Crashed);
    CHECK(cmd.calls == 1 && d.calls == 2 && d.r == Result::Crashed && d.reply.empty());
    CHECK(GuardSaw({ { &kGuarded, "guard.cmd" } }));

    // Tasks: owned ones through the guard, nullptr-owner ones straight through.
    g_guardSeen.clear();
    g_order.clear();
    CHECK(sco::Post(Record, Tag(1), &kGuarded) == Result::Ok);
    CHECK(sco::Post(Record, Tag(2)) == Result::Ok);
    CHECK(sco::DrainTasks() == 2 && (g_order == std::vector<int>{ 2 }));   // the owned one "faulted"
    CHECK(GuardSaw({ { &kGuarded, "task" } }));
    g_guardFaults = false;

    // Events: the event name is `where`.
    g_guardSeen.clear();
    Seen owned, host;
    CHECK(sco::Subscribe(&kGuarded, "guard.ev", OnEvent, &owned) == Result::Ok);
    CHECK(sco::Subscribe(nullptr, "guard.ev", OnEvent, &host) == Result::Ok);
    size_t called = 0;
    CHECK(sco::Dispatch("guard.ev", nullptr, &called) == Result::Ok && called == 2 && owned.calls == 1 && host.calls == 1);
    CHECK(GuardSaw({ { &kGuarded, "guard.ev" } }));

    // An off-thread invoke: the queued call itself is runtime code (no "task" callout); the
    // command and done are guarded as before.
    g_guardSeen.clear();
    Done q;
    Result r = Result::Crashed;
    std::thread([&] { r = sco::Invoke("guard.cmd", nullptr, 0, OnDone, &q, &kCaller); }).join();
    CHECK(r == Result::Ok && sco::DrainTasks() == 1 && q.calls == 1 && q.r == Result::Ok && cmd.calls == 2);
    CHECK(GuardSaw({ { &kGuarded, "guard.cmd" }, { &kCaller, "invoke done" } }));

    // Uninstalled: direct calls again.
    sco::SetCalloutGuard(nullptr);
    g_guardSeen.clear();
    CHECK(sco::Invoke("guard.cmd", nullptr, 0, OnDone, &d, &kCaller) == Result::Ok && cmd.calls == 3);
    CHECK(sco::Dispatch("guard.ev", nullptr, &called) == Result::Ok && owned.calls == 2);
    CHECK(g_guardSeen.empty());

    CHECK(sco::Unsubscribe(nullptr, "guard.ev", OnEvent) == Result::Ok);
    CHECK(sco::Release(&kGuarded) == Result::Ok && sco::ListCommands(nullptr, 0) == 0);
}

// ---- done after the invoking owner is released -----------------------------------------------

static const void* g_releaseMe = nullptr;
static Result CmdRelease(const sco::Arg*, uint32_t, void*, char* reply, uint32_t size) {
    CHECK(sco::Release(g_releaseMe) == Result::Ok);
    snprintf(reply, size, "released");
    return Result::Ok;
}
static int g_drops = 0;
static void* g_droppedCtx = nullptr;
static void CountDrop(void* ctx) { ++g_drops; g_droppedCtx = ctx; }

static void TestDoneAfterRelease() {
    static char kRel, kPlain, kGame, kOff, kKept, kBystander;
    const sco::Command c{ "rel.caller", "Release", nullptr, nullptr, nullptr, 0, CmdRelease, nullptr };
    CHECK(sco::RegisterCommand(&kRel, "rel", c) == Result::Ok);

    // The command releases the invoking owner: the result still comes back, done is not called.
    Done d;
    g_releaseMe = &kPlain;
    CHECK(sco::Invoke("rel.caller", nullptr, 0, OnDone, &d, &kPlain) == Result::Ok && d.calls == 0);
    // With a dropCtx (sco::host's heap DoneRecord), dropCtx runs instead so ctx is freed.
    g_releaseMe = &kGame;
    CHECK(sco::detail::InvokeOwned("rel.caller", nullptr, 0, OnDone, &d, &kGame, CountDrop) == Result::Ok);
    CHECK(d.calls == 0 && g_drops == 1 && g_droppedCtx == &d);
    // Off the game thread: the queued call runs, releases its owner, done is skipped.
    g_releaseMe = &kOff;
    Result r = Result::Crashed;
    std::thread([&] { r = sco::detail::InvokeOwned("rel.caller", nullptr, 0, OnDone, &d, &kOff, CountDrop); }).join();
    CHECK(r == Result::Ok && sco::DrainTasks() == 1 && d.calls == 0 && g_drops == 2);
    // Control: someone else released, the invoking owner is live: done runs, dropCtx doesn't.
    g_releaseMe = &kBystander;
    CHECK(sco::detail::InvokeOwned("rel.caller", nullptr, 0, OnDone, &d, &kKept, CountDrop) == Result::Ok);
    CHECK(d.calls == 1 && d.reply == "released" && g_drops == 2);
    CHECK(sco::Release(&kRel) == Result::Ok);
}

// ---- services -------------------------------------------------------------------------------

static void TestServices() {
    static int a, b;
    struct Table { int x; };
    static const Table ta{ 1 }, tb{ 2 };
    const void* out = &ta;
    CHECK(sco::QueryService("a.svc", 0x00010000, &out) == Result::NotFound && out == nullptr);

    // Names: [a-z0-9_.], 1-63, the owner's prefix; no duplicates.
    CHECK(sco::ProvideService(&a, "a", "a.svc", 0x00010002, &ta) == Result::Ok);
    CHECK(sco::ProvideService(&a, "a", "a", 0x00010000, &ta) == Result::Ok);           // the bare id
    CHECK(sco::ProvideService(&b, "b", "a.other", 0x00010000, &tb) == Result::BadArg);  // another's prefix
    CHECK(sco::ProvideService(&b, "b", "bb.svc", 0x00010000, &tb) == Result::BadArg);   // "bb" is not "b."
    CHECK(sco::ProvideService(&b, nullptr, "a.svc", 0x00010000, &tb) == Result::BadArg); // taken
    for (const char* bad : { "", ".b", "b.", "b..c", "B", "b c", "b-c" })
        CHECK(sco::ProvideService(&b, nullptr, bad, 1, &tb) == Result::BadArg);
    CHECK(sco::ProvideService(&b, nullptr, std::string(64, 'b').c_str(), 1, &tb) == Result::BadArg);
    CHECK(sco::ProvideService(&b, nullptr, std::string(63, 'b').c_str(), 1, &tb) == Result::Ok);
    CHECK(sco::ProvideService(&b, nullptr, nullptr, 1, &tb) == Result::BadArg);
    CHECK(sco::ProvideService(&b, nullptr, "b.null", 1, nullptr) == Result::BadArg);
    CHECK(sco::ProvideService(nullptr, nullptr, "b.noowner", 1, &tb) == Result::BadArg);
    CHECK(sco::ProvideService(&b, "b", "b.svc", 0x00020000, &tb) == Result::Ok);

    // Versions: same major, at least the minor asked for.
    CHECK(sco::QueryService("a.svc", 0x00010000, &out) == Result::Ok && out == &ta);
    CHECK(sco::QueryService("a.svc", 0x00010002, &out) == Result::Ok && out == &ta);
    CHECK(sco::QueryService("a.svc", 0x00010003, &out) == Result::Unavailable && out == nullptr);
    CHECK(sco::QueryService("b.svc", 0x00010000, &out) == Result::Unavailable && out == nullptr);
    CHECK(sco::QueryService("b.svc", 0x00020000, &out) == Result::Ok && out == &tb);
    CHECK(sco::QueryService(nullptr, 1, &out) == Result::BadArg);
    CHECK(sco::QueryService("a.svc", 1, nullptr) == Result::BadArg);

    // Release withdraws the owner's services and refuses new ones; others keep theirs.
    size_t removed = 0;
    CHECK(sco::Release(&a, &removed) == Result::Ok && removed == 2);
    CHECK(sco::QueryService("a.svc", 0x00010000, &out) == Result::NotFound);
    CHECK(sco::ProvideService(&a, "a", "a.again", 1, &ta) == Result::BadArg);
    CHECK(sco::QueryService("b.svc", 0x00020000, &out) == Result::Ok);
    CHECK(sco::Release(&b, &removed) == Result::Ok && removed == 2);
    CHECK(sco::QueryService("b.svc", 0x00020000, &out) == Result::NotFound);
}

// ---- raw handlers ---------------------------------------------------------------------------

static bool OnlyRawCap(const char* cap) { return std::strcmp(cap, "raw.ok") == 0; }
static bool FaultFor(const void* owner, const char*, sco::TaskFn thunk, void* ctx);
static const void* g_faultOwner = nullptr;
static bool FaultFor(const void* owner, const char*, sco::TaskFn thunk, void* ctx) {
    if (owner == g_faultOwner) return false;   // as if the callout faulted
    thunk(ctx);
    return true;
}

static Result Double(const void* in, uint32_t inSize, void* out, uint32_t* outSize, void*) {
    if (inSize != 4) return Result::BadArg;
    if (*outSize < 4) { *outSize = 4; return Result::TooMany; }
    int32_t v;
    std::memcpy(&v, in, 4);
    v *= 2;
    std::memcpy(out, &v, 4);
    *outSize = 4;
    return Result::Ok;
}
static Result Overstate(const void*, uint32_t, void*, uint32_t* outSize, void*) { *outSize += 8; return Result::Ok; }

static void TestRaw() {
    static int a, b, caller;
    sco::SetCapabilityCheck(OnlyRawCap);
    CHECK(sco::RegisterRaw(&a, "ra", "ra.double", nullptr, Double, nullptr) == Result::Ok);
    CHECK(sco::RegisterRaw(&a, "ra", "ra.gated", "raw.ok", Double, nullptr) == Result::Ok);
    CHECK(sco::RegisterRaw(&a, "ra", "ra.locked", "raw.no", Double, nullptr) == Result::Ok);
    CHECK(sco::RegisterRaw(&a, "ra", "ra.over", nullptr, Overstate, nullptr) == Result::Ok);
    CHECK(sco::RegisterRaw(&b, "rb", "ra.steal", nullptr, Double, nullptr) == Result::BadArg);    // another's prefix
    CHECK(sco::RegisterRaw(&b, nullptr, "ra.double", nullptr, Double, nullptr) == Result::BadArg); // taken
    for (const char* bad : { "", "nodot", ".x", "x.", "x..y", "X.y" })
        CHECK(sco::RegisterRaw(&b, nullptr, bad, nullptr, Double, nullptr) == Result::BadArg);
    CHECK(sco::RegisterRaw(&b, nullptr, "rb.x", "Bad Cap", Double, nullptr) == Result::BadArg);
    CHECK(sco::RegisterRaw(&b, nullptr, "rb.x", nullptr, nullptr, nullptr) == Result::BadArg);
    CHECK(sco::RegisterRaw(&b, "sco", "sco.x", nullptr, Double, nullptr) == Result::BadArg);      // reserved

    const int32_t in = 21;
    int32_t out = 0;
    uint32_t size = sizeof(out);
    CHECK(sco::InvokeRaw(&caller, "ra.double", &in, 4, &out, &size) == Result::Ok && out == 42 && size == 4);
    size = 2;
    CHECK(sco::InvokeRaw(&caller, "ra.double", &in, 4, &out, &size) == Result::TooMany && size == 4);
    size = 0;
    CHECK(sco::InvokeRaw(&caller, "ra.double", &in, 4, nullptr, &size) == Result::TooMany && size == 4);
    size = sizeof(out);
    CHECK(sco::InvokeRaw(&caller, "ra.gated", &in, 4, &out, &size) == Result::Ok);
    CHECK(sco::InvokeRaw(&caller, "ra.locked", &in, 4, &out, &size) == Result::Unavailable && size == 0);
    size = 4;
    CHECK(sco::InvokeRaw(&caller, "ra.over", nullptr, 0, &out, &size) == Result::TooMany && size == 12);
    CHECK(sco::InvokeRaw(&caller, "ra.none", nullptr, 0, nullptr, nullptr) == Result::NotFound);
    CHECK(sco::InvokeRaw(&caller, nullptr, nullptr, 0, nullptr, nullptr) == Result::BadArg);
    CHECK(sco::InvokeRaw(&caller, "ra.double", nullptr, 4, nullptr, nullptr) == Result::BadArg);   // in null, size 4
    size = 4;
    CHECK(sco::InvokeRaw(&caller, "ra.double", &in, 4, nullptr, &size) == Result::BadArg);        // out null, room claimed
    Result off = Result::Ok;
    std::thread([&] { off = sco::InvokeRaw(&caller, "ra.double", &in, 4, nullptr, nullptr); }).join();
    CHECK(off == Result::WrongThread);

    // A faulting handler is Crashed, with no output.
    g_faultOwner = &a;
    sco::SetCalloutGuard(FaultFor);
    size = sizeof(out);
    CHECK(sco::InvokeRaw(&caller, "ra.double", &in, 4, &out, &size) == Result::Crashed && size == 0);
    sco::SetCalloutGuard(nullptr);
    g_faultOwner = nullptr;

    // Release removes the owner's handlers; a released caller is refused.
    size_t removed = 0;
    CHECK(sco::Release(&a, &removed) == Result::Ok && removed == 4);
    CHECK(sco::InvokeRaw(&caller, "ra.double", &in, 4, nullptr, nullptr) == Result::NotFound);
    CHECK(sco::RegisterRaw(&a, "ra", "ra.again", nullptr, Double, nullptr) == Result::BadArg);
    CHECK(sco::Release(&caller) == Result::Ok);
    CHECK(sco::InvokeRaw(&caller, "ra.none", nullptr, 0, nullptr, nullptr) == Result::BadArg);
    sco::SetCapabilityCheck(nullptr);

    // ReleaseService: one service at a time, only the owner's.
    static int s;
    static const int tbl = 1;
    CHECK(sco::ProvideService(&s, "sv", "sv.one", 0x00010000, &tbl) == Result::Ok);
    CHECK(sco::ReleaseService(&b, "sv.one") == Result::NotFound);
    CHECK(sco::ReleaseService(&s, "sv.one") == Result::Ok && sco::ReleaseService(&s, "sv.one") == Result::NotFound);
    CHECK(sco::ReleaseService(nullptr, "sv.one") == Result::BadArg && sco::ReleaseService(&s, nullptr) == Result::BadArg);
}

int main() {
    TestBeforeGameThread();   // first: checks the "no game thread yet" state
    TestTaskQueue();
    TestTasksManyThreads();
    TestEvents();
    TestOverlap();
    TestCalloutGuard();
    TestDoneAfterRelease();
    TestServices();
    TestCommands();
    TestCommandRecycling();   // after TestCommands: it leaves only the host's commands live
    TestRaw();
    std::printf("sco-core runtime tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
