// Unit tests for the C++20 SDK layer (include/scosdk/) over the real host table (sco/host.h).
// Host build, no game needed. The "game thread" is this test's main thread.
//
// Two SDK plugins share the process: "prov", exported with SCO_PLUGIN and driven through its
// sco_plugin_query / load / unload exports as the loader would, and "cons", a second Plugin object
// loaded with sco::sdk::LoadPlugin (one binary can only export one SCO_PLUGIN).
//   tools/test.sh   (ASan+UBSan and ThreadSanitizer)
#include "scosdk/scosdk.hpp"

#include "sco/host.h"
#include "sco/log.h"
#include "sco/runtime.h"
#include "sco_api.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0, g_pass = 0;
// Variadic: braced argument lists inside a check carry commas.
#define CHECK(...) do { if (__VA_ARGS__) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); } } while (0)

namespace sdk = sco::sdk;

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

static uint32_t g_now = 0;
static void Tick() { sco::GameThreadTick(++g_now); }

// ---- the contract between the two plugins ---------------------------------------------------

struct MathV1 { uint32_t size; int64_t (*add)(int64_t, int64_t); };
struct MathV2 { uint32_t size; int64_t (*add)(int64_t, int64_t); int64_t (*mul)(int64_t, int64_t); };
static int64_t Add(int64_t a, int64_t b) { return a + b; }
static int64_t Mul(int64_t a, int64_t b) { return a * b; }
static const MathV1 kMath = { sizeof(MathV1), Add };
static const MathV2 kMath2 = { sizeof(MathV2), Add, Mul };

struct PoseIn { uint32_t entity; uint32_t flags; };
struct PoseOut { double x, y, z; };
struct SmallIn { uint16_t v; };
struct SmallOut { double x; };
struct BigOut { double v[4]; };

// ---- "prov": the SCO_PLUGIN plugin ----------------------------------------------------------

class Provider;
static Provider* g_prov = nullptr;
static int g_provTicks = 0, g_provTicks2 = 0, g_poseCalls = 0, g_droppedRuns = 0;
static uint32_t g_lastMs = 0;
static bool g_provUnloaded = false;

class Provider : public sdk::Plugin {
public:
    sdk::Subscription tick, tick2, boom;

    sco_result OnLoad() override {
        g_prov = this;
        g_provUnloaded = false;
        sco_result r = sdk::CommandBuilder(*this, "prov.add")
                           .Title("Add")
                           .Help("a + b, labelled")
                           .Arg<int64_t>("a", "first")
                           .Arg<double>("b", "second")
                           .Arg<const char*>("label")
                           .Arg<bool>("loud", "adds a !")
                           .Handle([](const sdk::Args& args, sdk::Reply& reply) {
                               if (args.Count() != 4) return SCO_BAD_ARG;
                               const double sum = static_cast<double>(args.Int(0)) + args.Float(1);
                               reply.Printf("%s%s = %g", args.String(2), args.Bool(3) ? "!" : "", sum);
                               return SCO_OK;
                           })
                           .Register();
        if (r != SCO_OK) return r;
        r = sdk::CommandBuilder(*this, "prov.boom")
                .Handle([](const sdk::Args&, sdk::Reply&) -> sco_result { throw std::runtime_error("kaboom"); })
                .Register();
        if (r != SCO_OK) return r;
        r = sdk::CommandBuilder(*this, "prov.odd")
                .Handle([](const sdk::Args&, sdk::Reply&) -> sco_result { throw 42; })
                .Register();
        if (r != SCO_OK) return r;
        if ((r = sdk::Provide(*this, "prov.math", sdk::ServiceVersion(1, 2), &kMath)) != SCO_OK) return r;
        r = sdk::RegisterRaw<PoseIn, PoseOut>(*this, "prov.pose", nullptr, [](const PoseIn& in, PoseOut& out) {
            ++g_poseCalls;
            out.x = in.entity;
            out.y = in.entity * 2.0;
            out.z = -1.0;
            return SCO_OK;
        });
        if (r != SCO_OK) return r;
        r = sdk::RegisterRaw<void, PoseOut>(*this, "prov.origin", nullptr, [](PoseOut& out) {
            out = PoseOut{ 1.0, 2.0, 3.0 };
            return SCO_OK;
        });
        if (r != SCO_OK) return r;
        r = sdk::RegisterRawBytes(*this, "prov.echo", nullptr,
                                  [](std::span<const std::byte> in, std::span<std::byte> out, uint32_t& written) {
                                      written = static_cast<uint32_t>(in.size());
                                      if (out.size() < in.size()) return SCO_TOO_MANY;
                                      if (!in.empty()) std::memcpy(out.data(), in.data(), in.size());
                                      return SCO_OK;
                                  });
        if (r != SCO_OK) return r;
        r = sdk::RegisterRaw<PoseIn, PoseOut>(*this, "prov.rawboom", nullptr, [](const PoseIn&, PoseOut&) -> sco_result {
            throw std::runtime_error("raw kaboom");
        });
        if (r != SCO_OK) return r;
        tick = Subscribe("tick", [](const void* data) {
            ++g_provTicks;
            g_lastMs = *static_cast<const uint32_t*>(data);
        });
        tick2 = Subscribe("tick", [](const void*) { ++g_provTicks2; });
        boom = Subscribe("test.boom", [](const void*) { throw std::runtime_error("event kaboom"); });
        if (!tick || !tick2 || !boom) return SCO_FAILED;
        Info("loaded: %d%%", 100);
        Info("%s", "100% literal");
        return SCO_OK;
    }

    void OnUnload() override { g_provUnloaded = true; }
};

SCO_PLUGIN(Provider, "prov", "1.2.3", "sco SDK tests");

// ---- the other plugins ----------------------------------------------------------------------

class Consumer : public sdk::Plugin {};

static int g_throwerTicks = 0, g_throwerTasks = 0;
class Thrower : public sdk::Plugin {
public:
    sdk::Subscription sub;
    sco_result OnLoad() override {
        sub = Subscribe("tick", [](const void*) { ++g_throwerTicks; });
        if (RunOnGameThread([] { ++g_throwerTasks; }) != SCO_OK) return SCO_FAILED;
        throw std::runtime_error("no config");
    }
};

static const sco_command* FindView(const sco_api* api, const char* name) {
    const sco_command* all[sco::kMaxCommands];
    const uint32_t n = api->list_commands(all, sco::kMaxCommands);
    for (uint32_t i = 0; i < n && i < sco::kMaxCommands; ++i) if (std::strcmp(all[i]->name, name) == 0) return all[i];
    return nullptr;
}

template <class T>
static std::span<const std::byte> BytesOf(const T& v) { return std::as_bytes(std::span<const T, 1>(&v, 1)); }

int main() {
    sco::SetGameThread();
    sco::SetLogSink(Capture);
    const sco_api* api = sco::host::BuildApi({ "sco-sdk test" });
    CHECK(api != nullptr);

    // ---- SCO_PLUGIN's exports, loaded as the loader does --------------------------------------
    const sco_plugin_info* info = sco_plugin_query();
    CHECK(info && info->size == sizeof(sco_plugin_info) && info->api_major == SCO_API_MAJOR &&
          info->api_minor == SCO_API_MINOR && std::strcmp(info->name, "prov") == 0 &&
          std::strcmp(info->version, "1.2.3") == 0 && std::strcmp(info->author, "sco SDK tests") == 0);
    CHECK(sco_plugin_query() == info);
    sco_plugin* provSelf = sco::host::NewPlugin("prov");
    sco_plugin* consSelf = sco::host::NewPlugin("cons");
    CHECK(provSelf && consSelf);
    const size_t baseSubs = sco::SubscriptionCount();
    CHECK(sco_plugin_load(nullptr, provSelf) == SCO_BAD_ARG);
    CHECK(sco_plugin_load(api, provSelf) == SCO_OK && g_prov != nullptr);
    CHECK(sco_plugin_load(api, provSelf) == SCO_BAD_ARG);   // already loaded
    CHECK(g_prov->Api() == api && g_prov->Self() == provSelf);
    CHECK(Logged("[prov] loaded: 100%") && Logged("[prov] 100% literal"));
    CHECK(sco::SubscriptionCount() == baseSubs + 2);   // tick (two handlers, one host subscription) + test.boom

    Consumer cons;
    CHECK(sdk::LoadPlugin(cons, api, consSelf) == SCO_OK);
    CHECK(sdk::LoadPlugin(cons, api, consSelf) == SCO_BAD_ARG);
    CHECK(cons.ApiCovers(offsetof(sco_api, register_raw)) && cons.Has("no.such") == false);

    // ---- commands: typed args, reply, exceptions --------------------------------------------
    std::string reply;
    CHECK(cons.Invoke("prov.add", { sdk::MakeArg(2), sdk::MakeArg(0.5), sdk::MakeArg("sum"), sdk::MakeArg(true) }, &reply) == SCO_OK);
    CHECK(reply == "sum! = 2.5");
    CHECK(cons.Invoke("prov.add", { sdk::MakeArg(int64_t{ -4 }), sdk::MakeArg(1.0), sdk::MakeArg("d"), sdk::MakeArg(false) }, &reply) == SCO_OK);
    CHECK(reply == "d = -3");
    CHECK(cons.Invoke("prov.add", { sdk::MakeArg(2) }, &reply) == SCO_BAD_ARG);                  // count, checked by the host
    CHECK(cons.Invoke("prov.add", { sdk::MakeArg(2.0), sdk::MakeArg(0.5), sdk::MakeArg("x"), sdk::MakeArg(false) }) == SCO_BAD_ARG);   // type
    const sco_command* view = FindView(api, "prov.add");
    CHECK(view && std::strcmp(view->title, "Add") == 0 && std::strcmp(view->help, "a + b, labelled") == 0 && view->capability == nullptr);
    CHECK(view && view->nargs == 4 && std::strcmp(view->args[1].name, "b") == 0 && view->args[1].type == SCO_ARG_FLOAT &&
          view->args[2].type == SCO_ARG_STRING && view->args[2].help == nullptr && view->args[3].type == SCO_ARG_BOOL &&
          std::strcmp(view->args[3].help, "adds a !") == 0);
    const sco_command* boomView = FindView(api, "prov.boom");
    CHECK(boomView && std::strcmp(boomView->title, "prov.boom") == 0 && boomView->nargs == 0);   // title defaults to the name

    CHECK(cons.Invoke("prov.boom", {}, &reply) == SCO_FAILED && reply == "prov.boom failed: kaboom");
    CHECK(Logged("[prov] error: exception in prov.boom: kaboom"));
    CHECK(cons.Invoke("prov.odd", {}, &reply) == SCO_FAILED && reply == "prov.odd failed: unknown exception");
    CHECK(Logged("[prov] error: exception in prov.odd: unknown exception"));

    auto ok = [](const sdk::Args&, sdk::Reply& r) { r.Set("pong"); return SCO_OK; };
    CHECK(sdk::CommandBuilder(*g_prov, "prov.nofn").Register() == SCO_BAD_ARG);
    CHECK(sdk::CommandBuilder(*g_prov, "prov.add").Handle(ok).Register() == SCO_BAD_ARG);     // taken
    CHECK(sdk::CommandBuilder(cons, "prov.mine").Handle(ok).Register() == SCO_BAD_ARG);       // prov's prefix
    CHECK(sdk::CommandBuilder(cons, nullptr).Handle(ok).Register() == SCO_BAD_ARG);
    CHECK(sdk::CommandBuilder(cons, "cons.badarg").Arg<int64_t>(nullptr).Handle(ok).Register() == SCO_BAD_ARG);
    CHECK(sdk::CommandBuilder(cons, std::string("cons.").append(70, 'x').c_str()).Handle(ok).Register() == SCO_BAD_ARG);
    CHECK(sdk::CommandBuilder(cons, "cons.gated").Capability("no.such").Handle(ok).Register() == SCO_OK);
    CHECK(cons.Invoke("cons.gated", {}, &reply) == SCO_UNAVAILABLE);
    CHECK(sdk::CommandBuilder(cons, "cons.ping").Handle(ok).Register() == SCO_OK);
    CHECK(cons.Invoke("cons.ping", {}, &reply) == SCO_OK && reply == "pong");
    CHECK(cons.Invoke("cons.ping") == SCO_OK);   // no reply wanted
    Consumer unloaded;
    CHECK(sdk::CommandBuilder(unloaded, "cons.never").Handle(ok).Register() == SCO_BAD_ARG);
    CHECK(unloaded.Invoke("cons.ping") == SCO_BAD_ARG && unloaded.RunOnGameThread([] {}) == SCO_BAD_ARG);
    CHECK(!unloaded.Subscribe("tick", [](const void*) {}) && unloaded.Subscribe("tick", [](const void*) {}).Result() == SCO_BAD_ARG);

    // InvokeAsync from another thread: queued, done once on the game thread.
    std::atomic<int> doneCalls{ 0 };
    sco_result doneR = SCO_RESULT_FORCE32, offR = SCO_RESULT_FORCE32;
    std::string doneText;
    bool doneOnGame = false;
    std::thread([&] {
        offR = cons.InvokeAsync("prov.add", { sdk::MakeArg(1), sdk::MakeArg(1.5), sdk::MakeArg("t"), sdk::MakeArg(false) },
                                [&](sco_result r, const char* text) {
                                    doneR = r;
                                    doneText = text;
                                    doneOnGame = sco::OnGameThread();
                                    doneCalls.fetch_add(1);
                                });
    }).join();
    CHECK(offR == SCO_OK && doneCalls.load() == 0);
    Tick();
    CHECK(doneCalls.load() == 1 && doneR == SCO_OK && doneText == "t = 2.5" && doneOnGame);
    CHECK(cons.InvokeAsync("prov.boom", {}, [&](sco_result r, const char* text) { doneR = r; doneText = text; doneCalls.fetch_add(1); }) == SCO_FAILED);
    CHECK(doneCalls.load() == 2 && doneR == SCO_FAILED && doneText == "prov.boom failed: kaboom");   // on the game thread: done before return
    std::thread([&] { offR = cons.Invoke("cons.ping", {}, &reply); }).join();
    CHECK(offR == SCO_OK && reply.empty());   // queued: no reply
    Tick();

    // ---- RunOnGameThread ----------------------------------------------------------------------
    std::atomic<int> runs{ 0 };
    bool ranOnGame = false;
    std::thread([&] { offR = cons.RunOnGameThread([&] { ranOnGame = sco::OnGameThread(); runs.fetch_add(1); }); }).join();
    CHECK(offR == SCO_OK && runs.load() == 0);
    Tick();
    CHECK(runs.load() == 1 && ranOnGame);
    Tick();
    CHECK(runs.load() == 1);   // ran once, then freed
    CHECK(cons.RunOnGameThread(nullptr) == SCO_BAD_ARG);
    CHECK(cons.RunOnGameThread([] { throw std::runtime_error("task kaboom"); }) == SCO_OK);
    Tick();
    CHECK(Logged("[cons] error: exception in task: task kaboom"));

    // ---- events ------------------------------------------------------------------------------
    int t1 = g_provTicks, t2 = g_provTicks2;
    sco::GameThreadTick(++g_now);
    CHECK(g_provTicks == t1 + 1 && g_provTicks2 == t2 + 1 && g_lastMs == g_now);
    size_t called = 0;
    CHECK(sco::Dispatch("test.boom", nullptr, &called) == sco::Result::Ok && called == 1);
    CHECK(Logged("[prov] error: exception in test.boom: event kaboom"));
    g_prov->tick2.Reset();
    CHECK(!g_prov->tick2 && sco::SubscriptionCount() == baseSubs + 2);   // tick still has a handler
    t1 = g_provTicks;
    t2 = g_provTicks2;
    Tick();
    CHECK(g_provTicks == t1 + 1 && g_provTicks2 == t2);

    // Subscribe and unsubscribe from another thread.
    int consTicks = 0;
    sdk::Subscription sub;
    std::thread([&] { sub = cons.Subscribe("tick", [&](const void*) { ++consTicks; }); }).join();
    CHECK(sub && sub.Result() == SCO_OK && sco::SubscriptionCount() == baseSubs + 3);
    Tick();
    CHECK(consTicks == 1);
    std::thread([&] { sub.Reset(); }).join();
    CHECK(!sub && sco::SubscriptionCount() == baseSubs + 2);
    Tick();
    CHECK(consTicks == 1);

    // A handler that resets its own handle; a moved handle.
    int selfTicks = 0;
    sdk::Subscription self;
    self = cons.Subscribe("tick", [&](const void*) { ++selfTicks; self.Reset(); });
    sdk::Subscription moved = std::move(self);
    CHECK(moved && !self && self.Result() == SCO_NOT_FOUND);
    self = std::move(moved);
    Tick();
    Tick();
    CHECK(selfTicks == 1 && !self && sco::SubscriptionCount() == baseSubs + 2);

    // Handlers come and go on another thread while the game thread dispatches (ThreadSanitizer).
    std::atomic<int> churnCalls{ 0 };
    std::atomic<bool> churnDone{ false };
    std::thread churn([&] {
        for (int i = 0; i < 200; ++i) {
            sdk::Subscription s = cons.Subscribe("tick", [&](const void*) { churnCalls.fetch_add(1); });
            sdk::Subscription s2 = cons.Subscribe("tick", [&](const void*) { churnCalls.fetch_add(1); });
            if (i % 2) s.Reset();
        }
        churnDone.store(true);
    });
    while (!churnDone.load()) Tick();
    churn.join();
    CHECK(sco::SubscriptionCount() == baseSubs + 2);

    // ---- services ----------------------------------------------------------------------------
    sdk::ServiceRef<MathV2> math;
    CHECK(!math && math.Get() == nullptr);
    CHECK(math.Query(cons, "prov.math", sdk::ServiceVersion(1, 0)) == SCO_OK && math && math->add(2, 3) == 5);
    CHECK(sdk::HasMember(math, &MathV2::add) && !sdk::HasMember(math, &MathV2::mul));   // a 1.x table without mul
    CHECK(math.Query(cons, "prov.math", sdk::ServiceVersion(1, 2)) == SCO_OK);
    CHECK(math.Query(cons, "prov.math", sdk::ServiceVersion(1, 3)) == SCO_UNAVAILABLE && !math);   // older than asked
    CHECK(math.Query(cons, "prov.math", sdk::ServiceVersion(2, 0)) == SCO_UNAVAILABLE && !math);   // another major
    CHECK(math.Query(api, "prov.nothing", sdk::ServiceVersion(1, 0)) == SCO_NOT_FOUND && !math);
    CHECK(sdk::Provide(cons, "prov.math2", sdk::ServiceVersion(1, 0), &kMath) == SCO_BAD_ARG);   // not cons's id
    CHECK(sdk::Provide(cons, "cons.math", sdk::ServiceVersion(1, 1), &kMath2) == SCO_OK);
    sdk::ServiceRef<MathV2> math2;
    CHECK(math2.Query(*g_prov, "cons.math", sdk::ServiceVersion(1, 0)) == SCO_OK && sdk::HasMember(math2, &MathV2::mul) &&
          math2->mul(6, 7) == 42);
    CHECK(sdk::Release(cons, "prov.math") == SCO_NOT_FOUND);
    CHECK(sdk::Release(*g_prov, "prov.math") == SCO_OK && math.Query(cons, "prov.math", sdk::ServiceVersion(1, 0)) == SCO_NOT_FOUND);
    CHECK(sdk::Provide(*g_prov, "prov.math", sdk::ServiceVersion(1, 2), &kMath) == SCO_OK);

    // ---- raw handlers --------------------------------------------------------------------------
    PoseOut pose{};
    const PoseIn in7{ 7, 0 };
    CHECK(sdk::InvokeRaw(cons, "prov.pose", in7, pose) == SCO_OK && pose.x == 7.0 && pose.y == 14.0 && pose.z == -1.0);
    CHECK(sdk::InvokeRaw(cons, "prov.origin", pose) == SCO_OK && pose.x == 1.0 && pose.z == 3.0);
    CHECK(sdk::InvokeRaw(cons, "prov.pose", SmallIn{ 1 }, pose) == SCO_BAD_ARG && pose.x == 1.0);   // input size checked
    CHECK(sdk::InvokeRaw(cons, "prov.origin", in7, pose) == SCO_BAD_ARG);                           // takes no input
    // The TOO_MANY size handshake.
    std::byte small[4] = {};
    uint32_t written = 0;
    CHECK(sdk::InvokeRawBytes(cons, "prov.pose", BytesOf(in7), small, written) == SCO_TOO_MANY && written == sizeof(PoseOut));
    written = 0;
    CHECK(sdk::InvokeRawBytes(cons, "prov.pose", BytesOf(in7), {}, written) == SCO_TOO_MANY && written == sizeof(PoseOut));   // asks the size
    SmallOut so{};
    uint32_t need = 0;
    CHECK(sdk::InvokeRaw(cons, "prov.pose", in7, so, &need) == SCO_TOO_MANY && need == sizeof(PoseOut));
    BigOut big{};
    CHECK(sdk::InvokeRaw(cons, "prov.pose", in7, big, &need) == SCO_BAD_ARG && need == sizeof(PoseOut) && big.v[0] == 0.0);
    // Bytes round trip.
    const char msg[] = "ping";
    std::byte buf[16] = {};
    CHECK(sdk::InvokeRawBytes(cons, "prov.echo", std::as_bytes(std::span<const char>(msg, 4)), buf, written) == SCO_OK &&
          written == 4 && std::memcmp(buf, "ping", 4) == 0);
    CHECK(sdk::InvokeRawBytes(cons, "prov.echo", std::as_bytes(std::span<const char>(msg, 4)), std::span<std::byte>(buf, 2), written) ==
              SCO_TOO_MANY && written == 4);
    // No output buffer: answered like a size query, without running the handler.
    const int poseCalls = g_poseCalls;
    CHECK(api->invoke_raw(consSelf, "prov.pose", &in7, sizeof in7, nullptr, nullptr) == SCO_TOO_MANY && g_poseCalls == poseCalls);
    // Exceptions, names, threads.
    CHECK(sdk::InvokeRaw(cons, "prov.rawboom", in7, pose) == SCO_FAILED);
    CHECK(Logged("[prov] error: exception in prov.rawboom: raw kaboom"));
    CHECK(sdk::InvokeRaw(cons, "prov.none", in7, pose) == SCO_NOT_FOUND);
    CHECK(sdk::RegisterRawBytes(cons, "prov.mine", nullptr, [](std::span<const std::byte>, std::span<std::byte>, uint32_t&) { return SCO_OK; }) ==
          SCO_BAD_ARG);
    std::thread([&] { offR = sdk::InvokeRaw(cons, "prov.pose", in7, pose); }).join();
    CHECK(offR == SCO_WRONG_THREAD);

    // ---- a 1.0 host: the 1.1 helpers answer SCO_UNAVAILABLE --------------------------------------
    sco_api old = *api;
    old.size = static_cast<uint32_t>(offsetof(sco_api, provide_service));
    sco_plugin* oldSelf = sco::host::NewPlugin("old");
    Consumer oldie;
    CHECK(sdk::LoadPlugin(oldie, &old, oldSelf) == SCO_OK);
    CHECK(sdk::Provide(oldie, "old.math", sdk::ServiceVersion(1, 0), &kMath) == SCO_UNAVAILABLE);
    CHECK(sdk::Release(oldie, "old.math") == SCO_UNAVAILABLE);
    CHECK(math.Query(oldie, "prov.math", sdk::ServiceVersion(1, 0)) == SCO_UNAVAILABLE && !math);
    CHECK(sdk::RegisterRaw<PoseIn, PoseOut>(oldie, "old.pose", nullptr, [](const PoseIn&, PoseOut&) { return SCO_OK; }) == SCO_UNAVAILABLE);
    CHECK(sdk::InvokeRaw(oldie, "prov.pose", in7, pose) == SCO_UNAVAILABLE);
    sdk::UnloadPlugin(oldie);
    CHECK(sco::Release(oldSelf) == sco::Result::Ok);
    old.size = 8;
    Consumer tiny;
    CHECK(sdk::LoadPlugin(tiny, &old, oldSelf) == SCO_UNAVAILABLE);

    // ---- an exception escaping OnLoad -----------------------------------------------------------
    sco_plugin* thSelf = sco::host::NewPlugin("thrower");
    const size_t subsBefore = sco::SubscriptionCount();
    {
        Thrower th;
        CHECK(sdk::LoadPlugin(th, api, thSelf) == SCO_FAILED);
        CHECK(Logged("[thrower] error: exception in OnLoad: no config"));
        CHECK(sco::SubscriptionCount() == subsBefore);   // the SDK released what OnLoad registered
        Tick();
        CHECK(g_throwerTicks == 0 && g_throwerTasks == 0);   // the queued task found nothing to run
        CHECK(sco::Release(thSelf) == sco::Result::Ok);
    }

    // ---- unload ---------------------------------------------------------------------------------
    CHECK(g_prov->RunOnGameThread([] { ++g_droppedRuns; }) == SCO_OK);   // queued at unload: dropped
    sco_plugin_unload();
    CHECK(g_provUnloaded && g_prov != nullptr);
    CHECK(sco::SubscriptionCount() == baseSubs);   // the SDK unsubscribed, before the host's Release
    CHECK(sco::Release(provSelf) == sco::Result::Ok);
    Tick();
    CHECK(g_droppedRuns == 0);
    CHECK(cons.Invoke("prov.add", { sdk::MakeArg(1), sdk::MakeArg(1.0), sdk::MakeArg("x"), sdk::MakeArg(true) }, &reply) == SCO_NOT_FOUND);
    CHECK(math.Query(cons, "prov.math", sdk::ServiceVersion(1, 0)) == SCO_NOT_FOUND);
    CHECK(sdk::InvokeRaw(cons, "prov.pose", in7, pose) == SCO_NOT_FOUND);
    sco_plugin_unload();   // a second unload does nothing

    // Load again under a new handle, as a reload would.
    g_prov = nullptr;
    sco_plugin* provSelf2 = sco::host::NewPlugin("prov");
    CHECK(provSelf2 && sco_plugin_load(api, provSelf2) == SCO_OK && g_prov != nullptr);
    CHECK(cons.Invoke("prov.add", { sdk::MakeArg(1), sdk::MakeArg(1.0), sdk::MakeArg("again"), sdk::MakeArg(false) }, &reply) == SCO_OK &&
          reply == "again = 2");
    sco_plugin_unload();
    CHECK(sco::Release(provSelf2) == sco::Result::Ok && sco::SubscriptionCount() == baseSubs);

    // The consumer: a queued invoke and a held handle at unload.
    int lateTicks = 0;
    sdk::Subscription late = cons.Subscribe("tick", [&](const void*) { ++lateTicks; });
    CHECK(late && sco::SubscriptionCount() == baseSubs + 1);
    std::atomic<int> droppedDone{ 0 };
    std::thread([&] { offR = cons.InvokeAsync("cons.ping", {}, [&](sco_result, const char*) { droppedDone.fetch_add(1); }); }).join();
    CHECK(offR == SCO_OK);
    sdk::UnloadPlugin(cons);
    CHECK(sco::SubscriptionCount() == baseSubs);
    CHECK(sco::Release(consSelf) == sco::Result::Ok);
    Tick();
    CHECK(droppedDone.load() == 0 && lateTicks == 0);
    std::thread([&] { late.Reset(); }).join();   // detached at unload: does nothing
    CHECK(!late);
    CHECK(cons.RunOnGameThread([] {}) == SCO_BAD_ARG && !cons.Subscribe("tick", [](const void*) {}));
    sdk::UnloadPlugin(cons);   // a second unload does nothing

    sco::SetLogSink(nullptr);
    std::printf("sco-core SDK tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
