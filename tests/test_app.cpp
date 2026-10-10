// Tests for the host kit (sco/app.h) and built-in plugins: sco::app::Start / Tick / Stop with
// built-ins defined here, fake native plugins (the tests/out/plugins layout tools/test.sh and the
// CMake build make for test_plugins), the SDK's greeter Lua example and travel_pack data pack,
// all through the real loader, host table and sco-lua. Host build, no game; this test's main
// thread is the game thread. Off Windows the crash guard is a signal handler installed here (as
// in test_plugins.cpp); on Windows the real __try/__except guard.
//   tools/test.sh
#include "sco/app.h"
#ifndef SCO_KERNEL_ONLY
#include "sco/game/signatures.h"
#endif
#include "sco/caps.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/net/session.h"
#include "sco/plugins.h"
#include "sco/runtime.h"
#include "sco/scan.h"
#include "sco/settings.h"
#include "sco/ui.h"
#include "sco_api.h"
#include "sco_lua.h"
#ifdef SCO_GAME_SERVICES
#include "sc_actors.h"
#include "sc_spawn.h"
#include "sc_vehicles.h"
#include <thread>
#endif
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace P = sco::plugins;
using P::State;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<std::string> g_log;
static void Sink(const char* line) { g_log.emplace_back(line); }
// Index of the first log line containing needle, or -1.
static long LogIndex(const char* needle) {
    for (size_t i = 0; i < g_log.size(); ++i) if (g_log[i].find(needle) != std::string::npos) return static_cast<long>(i);
    return -1;
}
static bool Logged(const char* needle) { return LogIndex(needle) >= 0; }

// ---- crash guard (POSIX) --------------------------------------------------------------------

#ifndef _WIN32
static sigjmp_buf g_jump;
static volatile sig_atomic_t g_inGuard = 0;
static void OnFault(int sig) {
    if (g_inGuard) siglongjmp(g_jump, sig);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
static uint32_t SignalGuard(void (*thunk)(void*), void* ctx) {
    struct sigaction sa {}, oldSegv {}, oldBus {};
    sa.sa_handler = OnFault;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &oldSegv);
    sigaction(SIGBUS, &sa, &oldBus);
    uint32_t code = 0;
    if (sigsetjmp(g_jump, 1)) {
        code = 0xC0000005u;   // report like an access violation on Windows
    } else {
        g_inGuard = 1;
        thunk(ctx);
    }
    g_inGuard = 0;
    sigaction(SIGSEGV, &oldSegv, nullptr);
    sigaction(SIGBUS, &oldBus, nullptr);
    return code;
}
#endif

// A real fault in built-in code. Not instrumented, so the sanitizers don't report the store
// before the guard sees the signal.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline, no_sanitize("address", "undefined")))
#endif
static void Fault() {
    volatile int* volatile p = nullptr;
    *p = 1;
}

// ---- built-in "core1": a command, tick, game.ready and game.exit ------------------------------

static std::vector<std::string> g_events;   // what core1 saw, in order, ticks excluded
static int g_coreTicks = 0;

static sco_result CoreCount(const sco_arg*, uint32_t, void*, char* reply, uint32_t replySize) {
    std::snprintf(reply, replySize, "ticks=%d", g_coreTicks);
    return SCO_OK;
}
static void CoreEvent(const char* event, const void*, void*) {
    if (std::strcmp(event, "tick") == 0) ++g_coreTicks;
    else g_events.push_back(std::string("core1:") + event);
}
static const sco_plugin_info* CoreQuery() {
    static const sco_plugin_info info = { sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "core1", "1.0.0", "tests" };
    return &info;
}
static sco_result CoreLoad(const sco_api* api, sco_plugin* self) {
    g_coreTicks = 0;
    sco_command cmd{};
    cmd.size = sizeof(cmd);
    cmd.name = "core1.count";
    cmd.title = "Count";
    cmd.arg_def_size = sizeof(sco_arg_def);
    cmd.fn = CoreCount;
    sco_result r = api->register_command(self, &cmd);
    for (const char* e : { "game.ready", "tick", "game.exit" })
        if (r == SCO_OK) r = api->subscribe(self, e, CoreEvent, nullptr);
    g_events.push_back("core1:load");
    return r;
}
static void CoreUnload() { g_events.push_back("core1:unload"); }

// ---- built-in "faulty": its tick faults -----------------------------------------------------

static void FaultyTick(const char*, const void*, void*) { Fault(); }
static const sco_plugin_info* FaultyQuery() {
    static const sco_plugin_info info = { sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "faulty", "0.1.0", "tests" };
    return &info;
}
static sco_result FaultyLoad(const sco_api* api, sco_plugin* self) { return api->subscribe(self, "tick", FaultyTick, nullptr); }
static void FaultyUnload() {}

static const P::Builtin kBuiltins[] = {
    { "core1", CoreQuery, CoreLoad, CoreUnload },
    { "faulty", FaultyQuery, FaultyLoad, FaultyUnload },
};
static const P::ScriptRuntime kLua{ sco_lua_load, sco_lua_unload };

static void SetCaps() {
    g_events.push_back("caps");
    sco::caps::Set("teleport", true);
}

// ---- helpers --------------------------------------------------------------------------------

struct Reply { bool called = false; sco::Result r = sco::Result::Ok; std::string text; };
static void OnDone(sco::Result r, const char* reply, void* ctx) {
    auto* d = static_cast<Reply*>(ctx);
    d->called = true; d->r = r; d->text = reply ? reply : "";
}
static Reply Invoke(const char* name, std::vector<sco::Arg> args = {}) {
    Reply d;
    sco::Invoke(name, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()), OnDone, &d);
    return d;
}
static sco::Arg Str(const char* s) { sco::Arg a{}; a.type = sco::ArgType::String; a.v.s = s; return a; }
static sco::Arg Bool(bool b) { sco::Arg a{}; a.type = sco::ArgType::Bool; a.v.i = b; return a; }

static const P::Plugin* Find(const char* id) {
    for (const auto& p : sco::app::Plugins()) if (p.folder == id) return &p;
    return nullptr;
}
static State StateOf(const char* id) { const P::Plugin* p = Find(id); return p ? p->state : State::Off; }
static int FakeTicks(const char* id) {
    const P::Plugin* p = Find(id);
    if (!p || !p->module) return -1;
    const int* t = static_cast<const int*>(P::PlatformModuleOps().symbol(p->module, "fake_ticks"));
    return t ? *t : -1;
}
static std::vector<std::string> Unloaded() {   // "[plugin] unloaded <id>" lines, in order
    std::vector<std::string> ids;
    for (const auto& l : g_log) if (l.rfind("[plugin] unloaded ", 0) == 0) ids.push_back(l.substr(18));
    return ids;
}

// ---- built-ins through the loader directly --------------------------------------------------

// One configurable built-in for the refusal cases: g_mode picks what it does.
enum Mode { kOk, kWrongName, kApi2, kLoadFails, kLoadCrashes, kSmallInfo };
static Mode g_mode = kOk;
static const char* g_name = "";
static int g_tUnloads = 0;
static void Noop(const char*, const void*, void*) {}
static const sco_plugin_info* TQuery() {
    static sco_plugin_info info;
    info = { sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, g_name, "2.3.4", "tests" };
    if (g_mode == kWrongName) info.name = "other";
    if (g_mode == kApi2) info.api_major = 2;
    if (g_mode == kSmallInfo) info.size = 8;
    return &info;
}
static sco_result TLoad(const sco_api* api, sco_plugin* self) {
    const sco_result r = api->subscribe(self, "tick", Noop, nullptr);
    if (g_mode == kLoadCrashes) Fault();
    return g_mode == kLoadFails ? SCO_UNAVAILABLE : r;
}
static void TUnload() { ++g_tUnloads; }

static P::Plugin TryLoad(const char* id, Mode mode, const sco_api* api) {
    g_mode = mode;
    g_name = id;
    P::Plugin p = P::FromBuiltin({ id, TQuery, TLoad, TUnload });
    P::LoadBuiltin(p, api, sco::host::NewPlugin(id), P::Options{});
    return p;
}

static void TestBuiltinLoader(const sco_api* api, const fs::path& plugins) {
    CHECK(std::strcmp(P::KindName(P::Kind::Builtin), "builtin") == 0);

    // Entries.
    P::Plugin bad = P::FromBuiltin({ "Bad-Id", TQuery, TLoad, TUnload });
    CHECK(bad.state == State::Refused && bad.reason == "built-in id 'Bad-Id' is not valid");
    CHECK(P::FromBuiltin({ "game", TQuery, TLoad, TUnload }).state == State::Refused);   // reserved
    CHECK(P::FromBuiltin({ nullptr, TQuery, TLoad, TUnload }).state == State::Refused);
    P::Plugin none = P::FromBuiltin({ "nothing", TQuery, nullptr, TUnload });
    CHECK(none.state == State::Refused && none.reason == "built-in nothing has no sco_plugin_load");
    P::Plugin fresh = P::FromBuiltin({ "fresh", TQuery, TLoad, TUnload });
    CHECK(fresh.state == State::Ready && fresh.manifest.kind == P::Kind::Builtin && fresh.manifest.id == "fresh");
    CHECK(fresh.dir.empty() && !fresh.module && P::Describe(fresh) == "fresh ? builtin ready");
    CHECK(!P::LoadBuiltin(fresh, nullptr, sco::host::NewPlugin("fresh"), P::Options{}) &&
          fresh.reason == "host passed no api or owner");

    // The same checks as a native plugin, without a module.
    const size_t subs = sco::SubscriptionCount();
    P::Plugin p = TryLoad("wrongname", kWrongName, api);
    CHECK(p.state == State::Refused && p.reason == "built-in name 'other' does not match id 'wrongname'");
    p = TryLoad("newapi", kApi2, api);
    CHECK(p.state == State::Refused && p.reason == "built-in built for api 2." + std::to_string(SCO_API_MINOR));
    p = TryLoad("small", kSmallInfo, api);
    CHECK(p.state == State::Refused && p.reason == "sco_plugin_info.size too small");
    p = TryLoad("fails", kLoadFails, api);
    CHECK(p.state == State::Refused && p.reason == "sco_plugin_load returned UNAVAILABLE");
    CHECK(sco::SubscriptionCount() == subs);                                // released
    p = TryLoad("crashes", kLoadCrashes, api);
    CHECK(p.state == State::Crashed && p.reason == "crashed in sco_plugin_load (0xC0000005)");
    CHECK(sco::SubscriptionCount() == subs);
    CHECK(Logged(("[plugin] refused newapi: built-in built for api 2." + std::to_string(SCO_API_MINOR) + "").c_str()));

    // A clean load fills the manifest from the query; UnloadAll unloads built-ins after every
    // other plugin, even one loaded before them.
    std::vector<P::Plugin> list;
    P::Options on;
    on.enabled = true;
    for (auto& n : P::Discover(plugins, on)) if (n.folder == "m0") list.push_back(std::move(n));
    list.push_back(P::FromBuiltin({ "late", TQuery, TLoad, TUnload }));
    CHECK(list.size() == 2);
    if (list.size() != 2) return;
    P::ContainCallouts(&list);
    CHECK(P::LoadNative(list[0], api, sco::host::NewPlugin("m0"), on));
    g_mode = kOk;
    g_name = "late";
    CHECK(P::LoadBuiltin(list[1], api, sco::host::NewPlugin("late"), on));
    const P::Plugin& late = list[1];
    CHECK(late.state == State::Loaded && late.loadOrder > list[0].loadOrder && !late.module);
    CHECK(late.manifest.version == "2.3.4" && late.manifest.author == "tests" && late.manifest.apiMajor == 1);
    CHECK(P::Describe(late) == "late 2.3.4 builtin loaded");
    CHECK(Logged(("[plugin] loaded late 2.3.4 (api 1." + std::to_string(SCO_API_MINOR) + ") built in").c_str()));
    CHECK(!P::LoadBuiltin(list[1], api, sco::host::NewPlugin("late2"), on));   // not Ready any more
    g_log.clear();
    g_tUnloads = 0;
    P::UnloadAll(list);
    CHECK((Unloaded() == std::vector<std::string>{ "m0", "late" }));
    CHECK(late.state == State::Unloaded && g_tUnloads == 1 && sco::SubscriptionCount() == subs);
    P::ContainCallouts(nullptr);
}

// ---- the host kit ---------------------------------------------------------------------------

// The product's signature tables: Start registers them through Platform::registerSignatures, and
// only with an image. Nothing in sco-core registers Star Citizen's tables on its own.
static int g_registerCalls = 0;
#ifndef SCO_KERNEL_ONLY
static bool RegisterForTest() { ++g_registerCalls; return sco::game::RegisterGameSignatures(); }
#else
static bool RegisterForTest() { ++g_registerCalls; return true; }   // kernel only: no game tables
#endif

static void TestApp(const fs::path& sdk, const fs::path& out) {
    const fs::path root = out / "app" / "plugins";
    std::error_code ec;
    fs::remove_all(out / "app", ec);
    fs::create_directories(root);
    for (const char* id : { "m0", "m11" }) {
        if (!fs::is_directory(out / "plugins" / id)) {
            std::printf("FAIL: %s missing (tools/test.sh or the CMake build makes it)\n", (out / "plugins" / id).string().c_str());
            ++g_fail;
            return;
        }
        fs::copy(out / "plugins" / id, root / id, fs::copy_options::recursive);
    }
    fs::copy(sdk / "examples" / "greeter", root / "greeter", fs::copy_options::recursive);
    fs::copy(sdk / "examples" / "travel_pack", root / "travel_pack", fs::copy_options::recursive);

    sco::app::Platform pf;
    pf.registerSignatures = RegisterForTest;
    pf.hostVersion = "test-app 1.0";
    pf.pluginRoot = root;
    pf.pluginsEnabled = true;
    pf.builtins = kBuiltins;
    pf.nBuiltins = 2;
    pf.scripts = &kLua;
    pf.setCapabilities = SetCaps;

    sco::app::Tick(1);                                                       // before Start: nothing
    CHECK(sco::app::Plugins().empty());
    g_log.clear();
    g_events.clear();
    CHECK(sco::app::Start(pf));

    // Built-ins first, then folders in name order; load order follows.
    const auto& list = sco::app::Plugins();
    std::vector<std::string> ids;
    for (const auto& p : list) ids.push_back(p.folder);
    CHECK((ids == std::vector<std::string>{ "core1", "faulty", "greeter", "m0", "m11", "travel_pack" }));
    for (const auto& p : list) {
        CHECK(p.state == State::Loaded);
        if (p.state != State::Loaded) std::printf("  %s\n", P::Describe(p).c_str());
    }
    CHECK(list.size() == 6);
    if (list.size() != 6) return;
    CHECK(list[0].loadOrder < list[1].loadOrder && list[1].loadOrder < list[2].loadOrder &&
          list[2].loadOrder < list[3].loadOrder && list[3].loadOrder < list[4].loadOrder && list[5].loadOrder == 0);
    CHECK(Logged(("[plugin] loaded core1 1.0.0 (api 1." + std::to_string(SCO_API_MINOR) + ") built in").c_str()));
    CHECK(Logged("[plugin] 6 found, 6 loaded (plugins = on)"));
    CHECK(Logged("[plugin] core1 1.0.0 builtin loaded"));
    CHECK(Logged("[m0] hello from m0"));
    CHECK(!Logged("[core] signatures"));                                     // no image: skipped
    CHECK(g_registerCalls == 0);                                             // nor registered

    // Capabilities before the plugins, game.ready after them.
    CHECK((g_events == std::vector<std::string>{ "caps", "core1:load", "core1:game.ready" }));
    CHECK(Logged("[greeter] teleport is available"));
    CHECK(LogIndex("[plugin] 6 found") < LogIndex("[greeter] teleport is available"));

    // The data pack is indexed.
    const auto content = sco::app::Content().FromPlugin("travel_pack");
    CHECK(content.size() == 1 && content[0]->name == "lists/locations.txt");

    // A second Start is refused and changes nothing.
    CHECK(!sco::app::Start(pf));
    CHECK(Logged("[app] start refused: already started") && sco::app::Plugins().size() == 6);

    // Commands: the built-in's and the Lua plugin's.
    Reply r = Invoke("core1.count");
    CHECK(r.called && r.r == sco::Result::Ok && r.text == "ticks=0");
    r = Invoke("greeter.greet", { Str("Pilot"), Bool(true) });
    CHECK(r.r == sco::Result::Ok && r.text == "HI, PILOT!");

    // Tick reaches every subscriber; the faulting built-in is contained and the rest keep going.
    sco::app::Tick(100);
    CHECK(g_coreTicks == 1 && FakeTicks("m0") == 1 && FakeTicks("m11") == 1);
    CHECK(StateOf("faulty") == State::Crashed && Find("faulty")->reason == "crashed in tick (0xC0000005)");
    CHECK(Logged("[plugin] faulty crashed in tick (0xC0000005) and was disabled"));
    sco::app::Tick(200);
    CHECK(g_coreTicks == 2 && FakeTicks("m0") == 2 && FakeTicks("m11") == 2);
    CHECK(StateOf("core1") == State::Loaded && StateOf("m0") == State::Loaded);
    CHECK(Invoke("core1.count").text == "ticks=2");

    // Stop: game.exit first, then unload newest first with built-ins last.
    const sco_plugin* firstSelf = Find("core1")->self;
    g_log.clear();
    g_events.clear();
    sco::app::Stop();
    CHECK((g_events == std::vector<std::string>{ "core1:game.exit", "core1:unload" }));
    CHECK(Logged("[greeter] greeted 1 times"));
    CHECK(LogIndex("[greeter] greeted 1 times") < LogIndex("[plugin] unloaded"));
    CHECK((Unloaded() == std::vector<std::string>{ "m11", "m0", "greeter", "core1" }));
    CHECK(StateOf("core1") == State::Unloaded && StateOf("m0") == State::Unloaded && StateOf("greeter") == State::Unloaded);
    CHECK(StateOf("faulty") == State::Crashed && StateOf("travel_pack") == State::Loaded);
    CHECK(Invoke("core1.count").r == sco::Result::NotFound);
    sco::app::Tick(300);                                                     // after Stop: nothing
    CHECK(g_coreTicks == 2);
    sco::app::Stop();                                                        // twice: no-op
    CHECK(g_events.size() == 2);

    // Start again, with a game image this time (a synthetic one: no row resolves). Fresh
    // handles; the signature report is logged and every plugin still loads.
    std::vector<uint8_t> mem(8192, 0xCC);
    sco::Image img{};
    img.base = mem.data();
    img.text = { mem.data(), 4096 };
    img.rdata = { mem.data() + 4096, 4096 };
    img.size = static_cast<uint32_t>(mem.size());
    pf.image = &img;
    g_log.clear();
    CHECK(sco::app::Start(pf));
    CHECK(Logged("[core] signatures: 0/"));
    CHECK(g_registerCalls == 1 && !Logged("signature tables did not all register"));
    CHECK(LogIndex("[core] signatures") < LogIndex("[plugin] 6 found, 6 loaded (plugins = on)"));
    CHECK(sco::app::Plugins().size() == 6 && Find("core1")->self && Find("core1")->self != firstSelf);
    for (const auto& p : sco::app::Plugins()) CHECK(p.state == State::Loaded);
    CHECK(Invoke("core1.count").text == "ticks=0");
    sco::app::Tick(1000);
    CHECK(g_coreTicks == 1 && StateOf("faulty") == State::Crashed);
    sco::app::Stop();
    CHECK(StateOf("core1") == State::Unloaded);

    // Plugins off: built-ins still load, no folder is read.
    pf.pluginsEnabled = false;
    pf.image = nullptr;
    g_log.clear();
    CHECK(sco::app::Start(pf));
    CHECK(sco::app::Plugins().size() == 2 && StateOf("core1") == State::Loaded);
    CHECK(Logged("[plugin] 2 found, 2 loaded (plugins = off)"));
    sco::app::Stop();

    // No script runtime: a lua plugin is refused, the rest load.
    pf.pluginsEnabled = true;
    pf.scripts = nullptr;
    CHECK(sco::app::Start(pf));
    CHECK(StateOf("greeter") == State::Refused && Find("greeter")->reason == "no script runtime");
    CHECK(StateOf("m0") == State::Loaded);
    sco::app::Stop();

    // A data folder gives plugins sco.storage, published before they load and withdrawn after
    // they unloaded; without one there is none.
    const sco_api* api = sco::host::BuildApi({ "test-app 1.0" });
    const void* table = nullptr;
    CHECK(api->query_service("sco.storage", 0x00010000, &table) == SCO_NOT_FOUND);
    pf.dataRoot = out / "app" / "data";
    CHECK(sco::app::Start(pf));
    CHECK(api->query_service("sco.storage", 0x00010000, &table) == SCO_OK && table);
    sco::app::Stop();
    CHECK(api->query_service("sco.storage", 0x00010000, &table) == SCO_NOT_FOUND);

    // sco.ui is always published, with the product's reserved chords, and withdrawn after unload.
    static const char* const kReserved[] = { "F6", "not a chord" };
    pf.reservedChords = kReserved;
    pf.nReservedChords = 2;
    CHECK(sco::app::Start(pf));
    CHECK(api->query_service("sco.ui", 0x00010000, &table) == SCO_OK && table == sco::ui::Table());
    CHECK((sco::ui::ReservedChords() == std::vector<std::string>{ "f6" }));
    CHECK(Logged("[app] hotkey 'not a chord' not reserved: BAD_ARG"));
    sco::app::Stop();
    CHECK(api->query_service("sco.ui", 0x00010000, &table) == SCO_NOT_FOUND && !sco::ui::Started());

    // sco.net is always published with its capability, so a plugin that requires it loads; nothing
    // listens until the product hosts or joins. Gone after Stop.
    pf.scripts = &kLua;
    fs::create_directories(root / "needsnet");
    {
        std::FILE* f = std::fopen((root / "needsnet" / "plugin.ini").string().c_str(), "wb");
        std::fputs("id = needsnet\nname = Needs sco.net\nversion = 1.0.0\napi = 1.0\nkind = lua\nentry = main.lua\n"
                   "requires = sco.net\n", f);
        std::fclose(f);
        f = std::fopen((root / "needsnet" / "main.lua").string().c_str(), "wb");
        std::fputs("-- requires sco.net\n", f);
        std::fclose(f);
    }
    CHECK(sco::app::Start(pf));
    CHECK(sco::caps::Has("sco.net") && sco::net::Started() && sco::net::BoundPort() == 0);
    CHECK(api->query_service(SCO_NET_NAME, SCO_NET_VERSION_1_0, &table) == SCO_OK && table == sco::net::Table());
    CHECK(StateOf("needsnet") == State::Loaded);
    sco::app::Stop();
    CHECK(!sco::caps::Has("sco.net") && !sco::net::Started());
    CHECK(api->query_service(SCO_NET_NAME, SCO_NET_VERSION_1_0, &table) == SCO_NOT_FOUND);

#ifdef SCO_GAME_SERVICES
    // gameServices: the game pack publishes teleport.spatial, spawn.entities and game.vehicles under "game" before
    // plugins load and withdraws them after. With no game image the teleport.* and spawn.* rows
    // aren't OK, so teleport.spatial answers 0 and spawn.entities says the spawner isn't available.
    pf.gameServices = true;
    CHECK(sco::app::Start(pf));
    CHECK(api->query_service("teleport.spatial", 0x00010000, &table) == SCO_OK && table);
    if (table) {
        struct Spatial { uint32_t size; int (*player_pose)(double*, double*, uint64_t*); };
        double pos[3], rot[4]; uint64_t zone = 0;
        CHECK(static_cast<const Spatial*>(table)->size >= sizeof(Spatial));
        CHECK(static_cast<const Spatial*>(table)->player_pose(pos, rot, &zone) == 0);
    }
    CHECK(api->query_service(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION, &table) == SCO_OK && table);
    if (table) {
        const auto* sp = static_cast<const sc_spawn_service_v1*>(table);
        const double offset[3] = {};
        const double rot[4] = { 0, 0, 0, 1 };
        uint64_t id = 7;
        CHECK(sp->size == sizeof(sc_spawn_service_v1));
        const char* err = sp->spawn_near_player("DRAK_Cutlass_Black", offset, &id);
        CHECK(err && std::strcmp(err, "the spawner isn't available on this game build") == 0 && id == 0);
        id = 7;
        err = sp->spawn_as(reinterpret_cast<sco_plugin*>(&table), "DRAK_Cutlass_Black", offset, &id);
        CHECK(err && std::strcmp(err, "the spawner isn't available on this game build") == 0 && id == 0);
        CHECK(sp->class_exists("DRAK_Cutlass_Black") == 0);
        CHECK(sp->local_player_id() == 0 && sp->player_ship_id() == 0 && sp->entity_alive(1) == 0);
        CHECK(sp->set_entity_transform(reinterpret_cast<sco_plugin*>(&table), 1, 0, offset, rot) == 0);
    }
    // game.vehicles: without the spawn.* rows every function answers SCO_UNAVAILABLE with a
    // reason, its capabilities aren't ready, and off the game thread it refuses before anything.
    CHECK(api->query_service(SC_VEHICLES_SERVICE_NAME, SC_VEHICLES_SERVICE_VERSION, &table) == SCO_OK && table);
    if (table) {
        const auto* v = static_cast<const sc_vehicles_v1*>(table);
        sco_plugin* self = reinterpret_cast<sco_plugin*>(&table);
        uint64_t ship = 7;
        uint32_t count = 9, more = 9;
        CHECK(v->size == sizeof(sc_vehicles_v1));
        CHECK(v->player_ship(&ship) == SCO_UNAVAILABLE && ship == 0);
        CHECK(v->seats(1, nullptr, 0, &count, &more) == SCO_UNAVAILABLE && count == 0 && more == 0);
        CHECK(v->seat_occupant(1, 0, &ship) == SCO_UNAVAILABLE && ship == 0);
        CHECK(v->seat(self, 1, 2, 0) == SCO_UNAVAILABLE && v->eject(self, 1) == SCO_UNAVAILABLE);
        CHECK(v->power_on(self, 2) == SCO_UNAVAILABLE);
        char msg[128] = {};
        uint32_t n = sizeof(msg);
        CHECK(v->last_error(self, msg, &n) == SCO_OK && std::strstr(msg, "game.vehicles.flight_ready") && n == std::strlen(msg) + 1);
        n = 0;
        CHECK(v->last_error(self, nullptr, &n) == SCO_TOO_MANY && n == std::strlen(msg) + 1);
        n = sizeof(msg);
        CHECK(v->last_error(nullptr, msg, &n) == SCO_OK && std::strstr(msg, "game.vehicles.seats"));
        CHECK(!sco::caps::Has("game.vehicles.seats") && !sco::caps::Has("game.vehicles.seat") &&
              !sco::caps::Has("game.vehicles.flight_ready"));
        sco_result off = SCO_OK, offErr = SCO_OK;
        char offMsg[64] = {};
        std::thread([&] {
            uint64_t id = 7;
            off = v->player_ship(&id);
            uint32_t size = sizeof(offMsg);
            offErr = v->last_error(self, offMsg, &size);
        }).join();
        CHECK(off == SCO_WRONG_THREAD && offErr == SCO_OK && std::strcmp(offMsg, "game thread only") == 0);
    }
    // game.actors: published with them. No game image, so none of its capabilities is ready:
    // every function answers SCO_UNAVAILABLE and last_error says why; off the game thread
    // SCO_WRONG_THREAD; a handle NewPlugin didn't return is SCO_BAD_ARG.
    CHECK(api->query_service(SC_ACTORS_NAME, SC_ACTORS_VERSION_1_0, &table) == SCO_OK && table);
    CHECK(!sco::caps::Has("game.actors.local_player") && !sco::caps::Has("game.actors.spawn_npc") &&
          !sco::caps::Has("game.actors.despawn"));
    if (table) {
        const auto* ga = static_cast<const sc_actors_v1*>(table);
        sco_plugin* me = sco::host::NewPlugin("actorstest");
        CHECK(me && ga->size == sizeof(sc_actors_v1));
        uint64_t actor = 7, entity = 7, id = 7;
        const double pos[3] = { 1, 2, 3 };
        char buf[256];
        uint32_t n = sizeof(buf);
        CHECK(ga->local_player(&actor, &entity) == SCO_UNAVAILABLE && actor == 0 && entity == 0);
        CHECK(ga->last_error(nullptr, buf, &n) == SCO_OK && n == std::strlen(buf) + 1 &&
              std::strstr(buf, "local_player: game.actors.local_player isn't available on this game build"));
        CHECK(ga->local_player(nullptr, nullptr) == SCO_BAD_ARG);
        sco_result off = SCO_OK;
        std::thread([&] { off = ga->local_player(&actor, &entity); }).join();
        CHECK(off == SCO_WRONG_THREAD);
        CHECK(ga->spawn_npc(me, "Human_NPC", 1, pos, &id) == SCO_UNAVAILABLE && id == 0);
        CHECK(ga->spawn_npc(reinterpret_cast<sco_plugin*>(&table), "Human_NPC", 1, pos, &id) == SCO_BAD_ARG);
        CHECK(ga->despawn(me, 1) == SCO_UNAVAILABLE);
        n = 4;   // the size handshake: too small answers the size needed
        CHECK(ga->last_error(me, buf, &n) == SCO_TOO_MANY && n > 4);
        const uint32_t need = n;
        CHECK(ga->last_error(me, buf, &n) == SCO_OK && n == need &&
              std::strstr(buf, "despawn: game.actors.despawn isn't available on this game build"));
        CHECK(ga->last_error(me, buf, nullptr) == SCO_BAD_ARG);
        CHECK(me && sco::Release(me) == sco::Result::Ok);   // through the release hook: no NPCs to remove
        n = sizeof(buf);
        CHECK(ga->last_error(me, buf, &n) == SCO_BAD_ARG);   // released
    }
    sco::app::Stop();
    CHECK(api->query_service(SC_VEHICLES_SERVICE_NAME, SC_VEHICLES_SERVICE_VERSION, &table) == SCO_NOT_FOUND);
    CHECK(api->query_service("teleport.spatial", 0x00010000, &table) == SCO_NOT_FOUND);
    CHECK(api->query_service(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION, &table) == SCO_NOT_FOUND);
    CHECK(api->query_service(SC_ACTORS_NAME, SC_ACTORS_VERSION_1_0, &table) == SCO_NOT_FOUND);
    pf.gameServices = false;
#endif
    fs::remove_all(out / "app", ec);
}

// ---- [settings]: declared in plugin.ini, read by a script, set by the product, kept across a restart ----

static std::vector<std::string> g_setEvents;
static void OnSettingsChanged(const char*, const void* data, void*) {
    const auto* c = static_cast<const sco_settings_changed*>(data);
    g_setEvents.push_back(std::string(c->plugin) + "." + c->name);
}

static void WriteSettingsPlugin(const fs::path& dir, const char* speedDecl) {
    fs::create_directories(dir);
    std::FILE* f = std::fopen((dir / "plugin.ini").string().c_str(), "wb");
    std::fputs("id = setdemo\nname = Settings demo\nversion = 1.0.0\napi = 1.0\nkind = lua\nentry = main.lua\n[settings]\n", f);
    std::fputs(speedDecl, f);
    std::fputs("\nmode = enum(easy,normal,hard) default normal\ngod_mode = bool\n", f);
    std::fclose(f);
    f = std::fopen((dir / "main.lua").string().c_str(), "wb");
    std::fputs("sco.register_command{ name = 'setdemo.show', title = 'Show', fn = function()\n"
               "  return tostring(sco.settings.get('speed')) .. ' ' .. sco.settings.get('mode') .. ' ' .. tostring(sco.settings.get('god_mode'))\n"
               "end }\n", f);
    std::fclose(f);
}

static void TestAppSettings(const fs::path& out) {
    const fs::path base = out / "app_settings";
    std::error_code ec;
    fs::remove_all(base, ec);
    WriteSettingsPlugin(base / "plugins" / "setdemo", "speed = int default 5 min 1 max 10");

    sco::app::Platform pf;
    pf.hostVersion = "test-app 1.0";
    pf.pluginRoot = base / "plugins";
    pf.dataRoot = base / "data";
    pf.pluginsEnabled = true;
    pf.scripts = &kLua;
    static int token;   // the owner of the test's own subscription

    // First run: the script reads the declared defaults; the product's sets change them, once each.
    g_log.clear();
    g_setEvents.clear();
    CHECK(sco::app::Start(pf));
    CHECK(StateOf("setdemo") == State::Loaded);
    CHECK(sco::Subscribe(&token, SCO_SETTINGS_CHANGED_EVENT, OnSettingsChanged, nullptr) == sco::Result::Ok);
    CHECK(Invoke("setdemo.show").text == "5 normal false");
    const auto pages = sco::settings::Pages();
    CHECK(pages.size() == 1 && pages[0].plugin == "setdemo" && pages[0].entries.size() == 3 && pages[0].tab.empty());
    CHECK(sco::settings::SetInt("setdemo", "speed", 8) == sco::Result::Ok);
    CHECK(sco::settings::SetString("setdemo", "mode", "hard") == sco::Result::Ok);
    CHECK(sco::settings::SetBool("setdemo", "god_mode", true) == sco::Result::Ok);
    CHECK(sco::settings::SetInt("setdemo", "speed", 8) == sco::Result::Ok);          // unchanged: silent
    CHECK(sco::settings::SetInt("setdemo", "speed", 99) == sco::Result::BadArg);     // refused: silent
    CHECK((g_setEvents == std::vector<std::string>{ "setdemo.speed", "setdemo.mode", "setdemo.god_mode" }));
    CHECK(Invoke("setdemo.show").text == "8 hard true");
    sco::app::Stop();
    CHECK(sco::settings::Entries("setdemo").empty() && !sco::settings::Started());

    // Second run over the same data folder: the values are still there.
    g_log.clear();
    CHECK(sco::app::Start(pf));
    CHECK(Invoke("setdemo.show").text == "8 hard true");
    CHECK(!Logged("kept value dropped"));
    sco::app::Stop();

    // The plugin updates and speed becomes a string: its kept int is dropped, with a log line,
    // the other two settings are kept.
    WriteSettingsPlugin(base / "plugins" / "setdemo", "speed = string default fast");
    g_log.clear();
    CHECK(sco::app::Start(pf));
    CHECK(Invoke("setdemo.show").text == "fast hard true");
    CHECK(Logged("[settings] setdemo.speed: kept value dropped (it was saved as int, the setting is string now); the default applies"));
    sco::app::Stop();
    g_log.clear();
    CHECK(sco::app::Start(pf));                    // the dropped value was deleted: nothing to say twice
    CHECK(Invoke("setdemo.show").text == "fast hard true" && !Logged("kept value dropped"));
    sco::app::Stop();
    CHECK(sco::Release(&token) == sco::Result::Ok);
    fs::remove_all(base, ec);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: test_app <sdk dir> <out dir>\n"); return 2; }
    sco::SetLogSink(Sink);
    sco::SetGameThread();   // Start sets it too; the loader tests below need it either way
#ifndef _WIN32
    P::SetCallGuard(SignalGuard);
#endif
    TestApp(argv[1], argv[2]);
    TestAppSettings(argv[2]);
    TestBuiltinLoader(sco::host::BuildApi({ "test-app 1.0" }), fs::path(argv[2]) / "plugins");
    P::SetCallGuard(nullptr);
    std::printf("sco-core app tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
