// Unit tests for capabilities (sco/caps.h) and the host's sco_api table (sco/host.h).
// Host build, no game needed. The "game thread" is this test's main thread.
//   tools/test.sh
#include "sco/caps.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/runtime.h"
#include "sco/signatures.h"
#include "sco/status.h"
#include "sco_api.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using sco::Result;

static std::mutex g_linesLock;
static std::vector<std::string> g_lines;
static void Capture(const char* line) {
    std::lock_guard<std::mutex> hold(g_linesLock);
    g_lines.emplace_back(line);
}
static bool Logged(const char* exact) {
    std::lock_guard<std::mutex> hold(g_linesLock);
    for (const auto& l : g_lines) if (l == exact) return true;
    return false;
}

// ---- capabilities ---------------------------------------------------------------------------

static sco::SigResult ResolveOk(const sco::Image&) { static uint8_t b; return sco::SigOk(&b); }
static sco::SigResult ResolveFail(const sco::Image&) { return sco::SigFail("layout check"); }
static const sco::SigDef kRows[] = {
    { "test.good", nullptr, 0, 0, ResolveOk, { nullptr } },
    { "test.good2", nullptr, 0, 0, ResolveOk, { "test.good", nullptr } },
    { "test.bad", nullptr, 0, 0, ResolveFail, { nullptr } },
    { "test.blocked", nullptr, 0, 0, ResolveOk, { "test.bad", nullptr } },
};

static std::string ReasonOf(const char* name) {
    sco::caps::Entry e[sco::caps::kMaxCaps];
    const size_t n = sco::caps::List(e, sco::caps::kMaxCaps);
    for (size_t i = 0; i < n; ++i) if (strcmp(e[i].name, name) == 0) return e[i].reason;
    return "<absent>";
}

static void TestCaps() {
    CHECK(sco::caps::List(nullptr, 0) == 0);
    CHECK(!sco::caps::Has("teleport") && !sco::caps::Has(nullptr));
    CHECK(sco::caps::Set("teleport", true) == Result::Ok);
    CHECK(sco::caps::Has("teleport"));
    CHECK(sco::caps::Set("spawn.ship", false, "signature spawn.fn MISSING") == Result::Ok);
    CHECK(!sco::caps::Has("spawn.ship"));
    CHECK(ReasonOf("spawn.ship") == "signature spawn.fn MISSING");
    CHECK(sco::caps::Set("spawn.ship", true) == Result::Ok);   // update in place
    CHECK(sco::caps::Has("spawn.ship") && ReasonOf("spawn.ship").empty());
    CHECK(sco::caps::Set("teleport", false, "zone lost") == Result::Ok);
    CHECK(!sco::caps::Has("teleport"));

    // Names: dotted lowercase segments, at most 63 characters.
    for (const char* bad : { "", "Teleport", "a..b", ".a", "a.", "a b", "a-b" })
        CHECK(sco::caps::Set(bad, true) == Result::BadArg);
    CHECK(sco::caps::Set(nullptr, true) == Result::BadArg);
    const std::string max63(63, 'a'), long64(64, 'a');
    CHECK(sco::caps::Set(max63.c_str(), true) == Result::Ok);
    CHECK(sco::caps::Set(long64.c_str(), true) == Result::BadArg);
    CHECK(!sco::caps::Has(long64.c_str()));
    const std::string longReason(300, 'r');
    CHECK(sco::caps::Set("x.reason", false, longReason.c_str()) == Result::Ok);
    CHECK(ReasonOf("x.reason") == std::string(sco::caps::kMaxReasonLen, 'r'));   // truncated

    // List: first-Set order; count when max is smaller.
    sco::caps::Entry two[2];
    CHECK(sco::caps::List(two, 2) == 4);
    CHECK(strcmp(two[0].name, "teleport") == 0 && !two[0].ready && strcmp(two[0].reason, "zone lost") == 0);
    CHECK(strcmp(two[1].name, "spawn.ship") == 0 && two[1].ready);

    // Backed by signature rows.
    CHECK(sco::RegisterSignatures(kRows, sizeof(kRows) / sizeof(kRows[0])));
    sco::ResolveAll(sco::Image{});
    const char* good[] = { "test.good", "test.good2" };
    const char* bad[] = { "test.good", "test.blocked", "test.bad" };
    const char* unknown[] = { "test.nope" };
    const char* withNull[] = { "test.good", nullptr };
    CHECK(sco::caps::SetFromSignatures("sig.good", good, 2) == Result::Ok && sco::caps::Has("sig.good"));
    CHECK(sco::caps::SetFromSignatures("sig.bad", bad, 3) == Result::Ok && !sco::caps::Has("sig.bad"));
    CHECK(ReasonOf("sig.bad") == "needs test.blocked (BLOCKED)");   // first row that isn't OK
    CHECK(sco::caps::SetFromSignatures("sig.unknown", unknown, 1) == Result::Ok && !sco::caps::Has("sig.unknown"));
    CHECK(ReasonOf("sig.unknown") == "unknown signature test.nope");
    CHECK(sco::caps::SetFromSignatures("sig.null", withNull, 2) == Result::BadArg);
    CHECK(sco::caps::SetFromSignatures("sig.none", good, 0) == Result::BadArg);
    CHECK(sco::caps::SetFromSignatures("sig.none", nullptr, 1) == Result::BadArg);
    CHECK(sco::caps::SetFromSignatures("Bad", good, 2) == Result::BadArg);
    CHECK(ReasonOf("sig.null") == "<absent>" && ReasonOf("sig.none") == "<absent>");

    // Concurrent writers and readers (ThreadSanitizer run). Names are fixed in advance so the
    // table fill below stays exact.
    std::atomic<bool> go{ true };
    std::thread reader([&] {
        sco::caps::Entry e[8];
        while (go.load()) { sco::caps::Has("conc.a"); sco::caps::List(e, 8); }
    });
    std::thread writer([] {
        for (int i = 0; i < 2000; ++i) sco::caps::Set("conc.a", i % 2 == 0, i % 3 ? "odd" : nullptr);
    });
    writer.join();
    go.store(false);
    reader.join();
    CHECK(!sco::caps::Has("conc.a"));   // i = 1999 was the last write

    // Bound: kMaxCaps names, then TooMany for new names; existing names still update.
    const size_t have = sco::caps::List(nullptr, 0);
    int ok = 0;
    for (size_t i = have; i < sco::caps::kMaxCaps; ++i)
        ok += sco::caps::Set(("fill.c" + std::to_string(i)).c_str(), true) == Result::Ok;
    CHECK(ok == static_cast<int>(sco::caps::kMaxCaps - have));
    CHECK(sco::caps::Set("fill.one_more", true) == Result::TooMany);
    CHECK(sco::caps::Set("teleport", true) == Result::Ok && sco::caps::Has("teleport"));
    CHECK(sco::caps::List(nullptr, 0) == sco::caps::kMaxCaps);
}

// ---- the table ------------------------------------------------------------------------------

struct Calls { int n = 0; std::vector<int64_t> ints; std::string str; bool onGame = false; };

static sco_result PluginCmd(const sco_arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t size) {
    Calls* c = static_cast<Calls*>(ctx);
    ++c->n;
    c->onGame = sco::OnGameThread();
    c->ints.clear();
    for (uint32_t i = 0; i < nargs; ++i)
        if (args[i].type == SCO_ARG_INT || args[i].type == SCO_ARG_BOOL) c->ints.push_back(args[i].v.i);
        else if (args[i].type == SCO_ARG_STRING) c->str = args[i].v.s;
    snprintf(reply, size, "waved %u", nargs);
    return SCO_OK;
}

static sco_result FailingCmd(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    snprintf(reply, size, "nope");
    return SCO_NOT_FOUND;
}

static Result HostCmd(const sco::Arg*, uint32_t, void* ctx, char* reply, uint32_t size) {
    ++*static_cast<int*>(ctx);
    snprintf(reply, size, "teleported");
    return Result::Ok;
}

struct Done { std::atomic<int> calls{ 0 }; sco_result r = SCO_RESULT_FORCE32; std::string reply; bool onGame = false; };
static void OnDone(sco_result r, const char* reply, void* ctx) {
    Done* d = static_cast<Done*>(ctx);
    d->r = r;
    d->reply = reply;
    d->onGame = sco::OnGameThread();
    d->calls.fetch_add(1);
}

static void Ev(const char*, const void*, void* ctx) { ++*static_cast<int*>(ctx); }
static void Task(void* ctx) { ++*static_cast<int*>(ctx); }

// A future minor's arg def: one extra field at the end. The host must step over it.
struct ArgDefV2 { const char* name; uint32_t type; uint32_t _pad; const char* help; const char* extra; };

static sco_command MakeCmd(const char* name, sco_command_fn fn, void* ctx, const sco_arg_def* args = nullptr,
                           uint32_t nargs = 0, uint32_t argDefSize = sizeof(sco_arg_def)) {
    sco_command c{};
    c.size = sizeof(sco_command);
    c.name = name;
    c.title = "Title";
    c.args = args;
    c.nargs = nargs;
    c.arg_def_size = argDefSize;
    c.fn = fn;
    c.ctx = ctx;
    return c;
}

static const sco_command* FindView(const sco_api* api, const char* name) {
    const sco_command* all[sco::kMaxCommands];
    const uint32_t n = api->list_commands(all, sco::kMaxCommands);
    for (uint32_t i = 0; i < n; ++i) if (strcmp(all[i]->name, name) == 0) return all[i];
    return nullptr;
}

static void TestTable() {
    CHECK(sco::host::BuildApi({ nullptr }) == nullptr);
    const sco_api* api = sco::host::BuildApi({ "sc-offline test" });
    CHECK(api && api->size == sizeof(sco_api) && api->major == SCO_API_MAJOR && api->minor == SCO_API_MINOR);
    CHECK(strcmp(api->host_version(), "sc-offline test") == 0);
    CHECK(sco::host::BuildApi({ "sc-offline 0.8.0" }) == api && strcmp(api->host_version(), "sc-offline 0.8.0") == 0);
    CHECK(api->host_version && api->has && api->run_on_game_thread && api->subscribe && api->unsubscribe &&
          api->status && api->log && api->register_command && api->invoke && api->list_commands);

    // has() answers from caps.
    CHECK(api->has("teleport") == 1 && api->has("sig.bad") == 0 && api->has("no.such") == 0 && api->has(nullptr) == 0);

    // Handles.
    sco_plugin* hello = sco::host::NewPlugin("hello");
    CHECK(hello && strcmp(sco::host::PluginId(hello), "hello") == 0);
    CHECK(sco::host::NewPlugin("hello") == nullptr);   // id held by a live handle
    for (const char* bad : { "", "Hello", "he.llo", "sco", "host", "menu", "game", "a-b" })
        CHECK(sco::host::NewPlugin(bad) == nullptr);
    CHECK(sco::host::NewPlugin(nullptr) == nullptr);
    CHECK(sco::host::NewPlugin(std::string(32, 'a').c_str()) == nullptr);
    sco_plugin* other = sco::host::NewPlugin(std::string(31, 'o').c_str());
    CHECK(other != nullptr);
    sco_plugin* forged = reinterpret_cast<sco_plugin*>(reinterpret_cast<char*>(hello) + 1);

    // Services (1.1): published under the plugin's own id, found by any plugin.
    CHECK(api->provide_service && api->query_service && api->release_service && api->invoke_raw && api->register_raw);
    static const int kTable = 7;
    CHECK(api->provide_service(hello, "hello.greeter", 0x00010000, &kTable) == SCO_OK);
    const void* tab = nullptr;
    CHECK(api->query_service("hello.greeter", 0x00010000, &tab) == SCO_OK && tab == &kTable);
    CHECK(api->query_service("hello.greeter", 0x00020000, &tab) == SCO_UNAVAILABLE && tab == nullptr);
    CHECK(api->query_service("hello.nothing", 0x00010000, &tab) == SCO_NOT_FOUND);
    CHECK(api->provide_service(hello, "teleport.spatial", 0x00010000, &kTable) == SCO_BAD_ARG);   // not hello's prefix
    CHECK(api->provide_service(hello, "hello.greeter", 0x00010000, &kTable) == SCO_BAD_ARG);      // taken
    CHECK(api->provide_service(forged, "hello.other", 0x00010000, &kTable) == SCO_BAD_ARG);
    CHECK(api->provide_service(hello, "hello.temp", 0x00010000, &kTable) == SCO_OK);
    CHECK(api->release_service(other, "hello.temp") == SCO_NOT_FOUND);   // not other's
    CHECK(api->release_service(hello, "hello.temp") == SCO_OK && api->query_service("hello.temp", 0x00010000, &tab) == SCO_NOT_FOUND);
    CHECK(api->release_service(forged, "hello.greeter") == SCO_BAD_ARG);

    // Raw handlers (1.1): bytes in, bytes out, through the plugin's own trampoline.
    struct Echo {
        static sco_result Fn(const void* in, uint32_t inSize, void* out, uint32_t* outSize, void* ctx) {
            ++*static_cast<int*>(ctx);
            if (*outSize < inSize) { *outSize = inSize; return SCO_TOO_MANY; }
            if (inSize) std::memcpy(out, in, inSize);
            *outSize = inSize;
            return SCO_OK;
        }
    };
    int echoCalls = 0;
    CHECK(api->register_raw(hello, "hello.echo", nullptr, Echo::Fn, &echoCalls) == SCO_OK);
    CHECK(api->register_raw(hello, "other.echo", nullptr, Echo::Fn, &echoCalls) == SCO_BAD_ARG);
    CHECK(api->register_raw(hello, "hello.echo", nullptr, Echo::Fn, &echoCalls) == SCO_BAD_ARG);
    CHECK(api->register_raw(hello, "hello.gated", "no.such", Echo::Fn, &echoCalls) == SCO_OK);
    CHECK(api->register_raw(forged, "hello.x", nullptr, Echo::Fn, &echoCalls) == SCO_BAD_ARG);
    const char msg[] = "ping";
    char got[16] = {};
    uint32_t gotSize = sizeof(got);
    CHECK(api->invoke_raw(other, "hello.echo", msg, 4, got, &gotSize) == SCO_OK && gotSize == 4 && std::memcmp(got, "ping", 4) == 0);
    gotSize = 2;
    CHECK(api->invoke_raw(other, "hello.echo", msg, 4, got, &gotSize) == SCO_TOO_MANY && gotSize == 4);
    gotSize = 0;
    CHECK(api->invoke_raw(other, "hello.echo", msg, 4, nullptr, &gotSize) == SCO_TOO_MANY && gotSize == 4);   // asks the size
    CHECK(api->invoke_raw(other, "hello.gated", msg, 4, nullptr, nullptr) == SCO_UNAVAILABLE);
    CHECK(api->invoke_raw(other, "hello.none", nullptr, 0, nullptr, nullptr) == SCO_NOT_FOUND);
    gotSize = sizeof(got);
    CHECK(api->invoke_raw(forged, "hello.echo", msg, 4, got, &gotSize) == SCO_BAD_ARG && gotSize == 0);
    sco_result offRaw = SCO_OK;
    std::thread([&] { uint32_t s = 0; offRaw = api->invoke_raw(other, "hello.echo", nullptr, 0, nullptr, &s); }).join();
    CHECK(offRaw == SCO_WRONG_THREAD);
    CHECK(echoCalls == 3);
    static char notAHandle[64];
    sco_plugin* fake = reinterpret_cast<sco_plugin*>(notAHandle);
    CHECK(sco::host::PluginId(forged) == nullptr && sco::host::PluginId(fake) == nullptr && sco::host::PluginId(nullptr) == nullptr);

    // Calls with a pointer the host didn't hand out are refused.
    int n = 0;
    Calls calls;
    const sco_command wave = MakeCmd("hello.wave", PluginCmd, &calls);
    CHECK(api->run_on_game_thread(fake, Task, &n) == SCO_BAD_ARG);
    CHECK(api->run_on_game_thread(nullptr, Task, &n) == SCO_BAD_ARG);
    CHECK(api->subscribe(forged, "tick", Ev, &n) == SCO_BAD_ARG);
    CHECK(api->unsubscribe(fake, "tick", Ev) == SCO_BAD_ARG);
    CHECK(api->register_command(fake, &wave) == SCO_BAD_ARG);
    CHECK(api->invoke(fake, "hello.wave", nullptr, 0, nullptr, nullptr) == SCO_BAD_ARG);

    // Tasks and events run as the plugin's owner.
    CHECK(api->run_on_game_thread(hello, Task, &n) == SCO_OK);
    CHECK(api->subscribe(hello, "tick", Ev, &n) == SCO_OK);
    CHECK(api->subscribe(hello, "tick", Ev, &n) == SCO_BAD_ARG);   // same key twice
    CHECK(sco::GameThreadTick(1) == Result::Ok && n == 2);
    CHECK(api->unsubscribe(hello, "tick", Ev) == SCO_OK && api->unsubscribe(hello, "tick", Ev) == SCO_NOT_FOUND);
    CHECK(sco::GameThreadTick(2) == Result::Ok && n == 2);

    // Status and log carry the plugin id.
    sco::SetLogSink(Capture);
    api->status(hello, "hi there");
    char st[64];
    CHECK(sco::GetStatus(st, sizeof(st)) && strcmp(st, "hello: hi there") == 0);
    api->log(hello, SCO_LOG_INFO, "one");
    api->log(hello, SCO_LOG_WARN, "two");
    api->log(hello, SCO_LOG_ERROR, "three");
    api->log(fake, SCO_LOG_INFO, "forged");
    api->log(hello, SCO_LOG_INFO, nullptr);
    CHECK(Logged("[hello] one") && Logged("[hello] warning: two") && Logged("[hello] error: three"));
    CHECK(!Logged("[status] : forged") && g_lines.size() == 4);   // the status line + 3 logs

    // register_command: the prefix is the plugin id.
    CHECK(api->register_command(hello, &wave) == SCO_OK);
    CHECK(api->register_command(hello, &wave) == SCO_BAD_ARG);                   // duplicate
    const sco_command foreign = MakeCmd("spawn.wave", PluginCmd, &calls);
    CHECK(api->register_command(hello, &foreign) == SCO_BAD_ARG);                // not its prefix
    CHECK(api->register_command(other, &wave) == SCO_BAD_ARG);                   // hello's prefix
    sco_command small = MakeCmd("hello.small", PluginCmd, &calls);
    small.size = sizeof(sco_command) - 8;
    CHECK(api->register_command(hello, &small) == SCO_BAD_ARG);                  // size too small
    CHECK(api->register_command(hello, nullptr) == SCO_BAD_ARG);
    const sco_command nofn = MakeCmd("hello.nofn", nullptr, nullptr);
    CHECK(api->register_command(hello, &nofn) == SCO_BAD_ARG);

    // Arg defs read with the caller's stride: a bigger future sco_arg_def still works.
    const ArgDefV2 v2[] = { { "count", SCO_ARG_INT, 0, "how many", "x" }, { "who", SCO_ARG_STRING, 0, nullptr, "y" },
                            { "loud", SCO_ARG_BOOL, 0, nullptr, "z" } };
    const sco_command greet = MakeCmd("hello.greet", PluginCmd, &calls, reinterpret_cast<const sco_arg_def*>(v2), 3,
                                      sizeof(ArgDefV2));
    CHECK(api->register_command(hello, &greet) == SCO_OK);
    const sco_command badStride = MakeCmd("hello.bad_stride", PluginCmd, &calls,
                                          reinterpret_cast<const sco_arg_def*>(v2), 3, sizeof(sco_arg_def) - 8);
    CHECK(api->register_command(hello, &badStride) == SCO_BAD_ARG);
    const sco_command oddStride = MakeCmd("hello.odd_stride", PluginCmd, &calls,
                                          reinterpret_cast<const sco_arg_def*>(v2), 3, sizeof(sco_arg_def) + 1);
    CHECK(api->register_command(hello, &oddStride) == SCO_BAD_ARG);
    const sco_command fails = MakeCmd("hello.fails", FailingCmd, nullptr);
    CHECK(api->register_command(hello, &fails) == SCO_OK);

    // A host feature's command, gated on a capability through caps (BuildApi wired the check).
    int hostRuns = 0;
    const sco::Command tp{ "teleport.save", "Save position", nullptr, "sig.bad", nullptr, 0, HostCmd, &hostRuns };
    CHECK(sco::RegisterCommand(nullptr, nullptr, tp) == Result::Ok);

    // list_commands: views for every live command; stable pointers; max = 0 counts.
    const uint32_t total = api->list_commands(nullptr, 0);
    CHECK(total == 4);
    const sco_command* vGreet = FindView(api, "hello.greet");
    const sco_command* vTp = FindView(api, "teleport.save");
    CHECK(vGreet && vTp && FindView(api, "hello.greet") == vGreet);
    CHECK(vGreet->size == sizeof(sco_command) && vGreet->nargs == 3 && vGreet->arg_def_size == sizeof(sco_arg_def));
    CHECK(vGreet->args && strcmp(vGreet->args[1].name, "who") == 0 && vGreet->args[1].type == SCO_ARG_STRING &&
          strcmp(vGreet->args[0].help, "how many") == 0 && vGreet->args[2].help == nullptr);
    CHECK(vGreet->name != greet.name && strcmp(vGreet->title, "Title") == 0);   // host copy
    CHECK(vGreet->fn == nullptr && vGreet->ctx == nullptr);                      // run with invoke
    CHECK(strcmp(vTp->capability, "sig.bad") == 0 && vTp->args == nullptr && vTp->nargs == 0);
    const sco_command* firstTwo[2] = {};
    CHECK(api->list_commands(firstTwo, 2) == total && firstTwo[0] == FindView(api, "hello.wave"));

    // invoke on the game thread: runs now through the trampoline, done before return.
    const sco_arg args[] = { { SCO_ARG_INT, 0, { 3 } }, { SCO_ARG_STRING, 0, { 0 } }, { SCO_ARG_BOOL, 0, { 1 } } };
    sco_arg greetArgs[3];
    memcpy(greetArgs, args, sizeof(args));
    greetArgs[1].v.s = "Ada";
    Done d;
    CHECK(api->invoke(hello, "hello.greet", greetArgs, 3, OnDone, &d) == SCO_OK);
    CHECK(d.calls == 1 && d.r == SCO_OK && d.reply == "waved 3" && d.onGame);
    CHECK(calls.n == 1 && calls.onGame && (calls.ints == std::vector<int64_t>{ 3, 1 }) && calls.str == "Ada");
    CHECK(api->invoke(hello, "hello.greet", greetArgs, 2, OnDone, &d) == SCO_BAD_ARG && d.calls == 2 && calls.n == 1);
    CHECK(api->invoke(hello, "hello.fails", nullptr, 0, OnDone, &d) == SCO_NOT_FOUND && d.reply == "nope");
    CHECK(api->invoke(hello, "hello.wave", nullptr, 0, nullptr, nullptr) == SCO_OK && calls.n == 2);   // no done
    CHECK(api->invoke(other, "teleport.save", nullptr, 0, OnDone, &d) == SCO_UNAVAILABLE && hostRuns == 0);
    CHECK(sco::caps::Set("sig.bad", true) == Result::Ok);
    CHECK(api->invoke(other, "teleport.save", nullptr, 0, OnDone, &d) == SCO_OK && hostRuns == 1 && d.reply == "teleported");
    CHECK(api->invoke(hello, "no.such", nullptr, 0, OnDone, &d) == SCO_NOT_FOUND);

    // invoke from another thread: queued, done once on the game thread at the next tick.
    Done od;
    sco_result offR = SCO_RESULT_FORCE32;
    std::thread([&] { offR = api->invoke(hello, "hello.greet", greetArgs, 3, OnDone, &od); }).join();
    CHECK(offR == SCO_OK && od.calls == 0);
    CHECK(sco::GameThreadTick(3) == Result::Ok);
    CHECK(od.calls == 1 && od.r == SCO_OK && od.onGame && od.reply == "waved 3" && calls.n == 3);
    std::thread([&] { offR = api->invoke(hello, nullptr, nullptr, 0, OnDone, &od); }).join();
    CHECK(offR == SCO_BAD_ARG);
    sco::GameThreadTick(4);
    CHECK(od.calls == 1);   // nothing queued, done never called

    // Release(self): queued work dropped (done not called; its record freed, checked by LSan on
    // Linux), commands gone, every later call refused and status/log dropped.
    Done dropped;
    std::thread([&] { offR = api->invoke(hello, "hello.wave", nullptr, 0, OnDone, &dropped); }).join();
    CHECK(offR == SCO_OK);
    CHECK(api->run_on_game_thread(hello, Task, &n) == SCO_OK);
    CHECK(api->subscribe(hello, "tick", Ev, &n) == SCO_OK);
    size_t removed = 0;
    CHECK(sco::Release(hello, &removed) == Result::Ok && removed == 9);   // 3 commands, 1 sub, 2 tasks, 1 service, 2 raw handlers
    const int nBefore = n;
    sco::GameThreadTick(5);
    CHECK(dropped.calls == 0 && n == nBefore);
    CHECK(api->list_commands(nullptr, 0) == 1 && FindView(api, "hello.greet") == nullptr);
    CHECK(api->run_on_game_thread(hello, Task, &n) == SCO_BAD_ARG);
    CHECK(api->subscribe(hello, "tick", Ev, &n) == SCO_BAD_ARG);
    CHECK(api->unsubscribe(hello, "tick", Ev) == SCO_BAD_ARG);
    CHECK(api->register_command(hello, &wave) == SCO_BAD_ARG);
    CHECK(api->invoke(hello, "teleport.save", nullptr, 0, OnDone, &d) == SCO_BAD_ARG);
    const size_t linesBefore = g_lines.size();
    api->log(hello, SCO_LOG_INFO, "after release");
    api->status(hello, "after release");
    CHECK(g_lines.size() == linesBefore);
    CHECK(strcmp(vGreet->name, "hello.greet") == 0);   // old views stay readable

    // The id is free again: a reload gets a new handle and can reuse the prefix.
    sco_plugin* hello2 = sco::host::NewPlugin("hello");
    CHECK(hello2 && hello2 != hello && sco::host::PluginId(hello) != nullptr);
    CHECK(api->register_command(hello2, &wave) == SCO_OK);
    CHECK(api->invoke(hello2, "hello.wave", nullptr, 0, OnDone, &d) == SCO_OK && d.reply == "waved 0");

    // Cross-thread invokes racing ticks (ThreadSanitizer run); bounded by a deadline.
    std::atomic<int> sent{ 0 };
    std::atomic<bool> spamDone{ false };
    Done conc;
    std::thread spammer([&] {
        for (int i = 0; i < 300; ++i)
            if (api->invoke(hello2, "hello.wave", nullptr, 0, OnDone, &conc) == SCO_OK) sent.fetch_add(1);
        spamDone.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    uint32_t now = 10;
    while (!spamDone.load() && std::chrono::steady_clock::now() < deadline) sco::GameThreadTick(++now);
    spammer.join();
    while (sco::QueuedTasks() && std::chrono::steady_clock::now() < deadline) sco::GameThreadTick(++now);
    CHECK(conc.calls.load() == sent.load() && sent.load() > 0);
    CHECK(sco::Release(hello2) == Result::Ok);
    sco::SetLogSink(nullptr);

    // Handle table bound: kMaxPlugins for the life of the process.
    size_t made = 0;
    for (size_t i = 0; i < sco::host::kMaxPlugins; ++i)
        made += sco::host::NewPlugin(("p" + std::to_string(i)).c_str()) != nullptr;
    CHECK(made == sco::host::kMaxPlugins - 3);   // hello, the 31-char id and hello2 used three
    CHECK(sco::host::NewPlugin("one_more") == nullptr);
}

int main() {
    sco::SetGameThread();
    TestCaps();
    TestTable();
    std::printf("sco-core host tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
