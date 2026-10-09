// Tests for sco-lua (plugins/lua) loaded through the real plugin loader and the real host table:
// Discover -> LoadScript -> sco_lua_load, with sco::host::BuildApi's sco_api underneath. Host
// build, no game. The "game thread" is this test's main thread.
//   tools/test.sh
#include "sco/caps.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/plugins.h"
#include "sco/runtime.h"
#include "sco/status.h"
#include "../plugins/lua/sco_lua.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace P = sco::plugins;
using sco::Result;
using P::State;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<std::string> g_log;
static void Sink(const char* line) { g_log.emplace_back(line); }
static bool Logged(const char* needle) {
    for (const auto& l : g_log) if (l.find(needle) != std::string::npos) return true;
    return false;
}

static const P::ScriptRuntime kLua{ sco_lua_load, sco_lua_unload };
static const sco_api* g_api;
static fs::path g_root;

// Writes data/plugins/<id>/{plugin.ini,main.lua} under the test root.
static void Write(const std::string& id, const std::string& lua) {
    const fs::path dir = g_root / id;
    fs::create_directories(dir);
    std::ofstream(dir / "plugin.ini") << "id = " << id << "\nname = " << id << "\nversion = 1.0\napi = 1.0\nkind = lua\nentry = main.lua\n";
    std::ofstream(dir / "main.lua", std::ios::binary) << lua;
}

struct Loaded {
    std::vector<P::Plugin> list;
    P::Plugin* p = nullptr;
};

// Discovers the root and loads plugin `id` (a fresh handle each time).
static Loaded Load(const std::string& id) {
    Loaded l;
    P::Options opts;
    opts.enabled = true;
    l.list = P::Discover(g_root, opts);
    for (auto& p : l.list) if (p.folder == id) l.p = &p;
    if (l.p && l.p->state == State::Ready) P::LoadScript(*l.p, g_api, sco::host::NewPlugin(id.c_str()), kLua);
    return l;
}

static void Unload(Loaded& l) { P::UnloadAll(l.list, P::PlatformModuleOps(), &kLua); }

struct Reply { bool called = false; sco_result r = SCO_OK; std::string text; };
static void OnDone(sco_result r, const char* reply, void* ctx) {
    auto* d = static_cast<Reply*>(ctx);
    d->called = true; d->r = r; d->text = reply ? reply : "";
}
static Reply Invoke(sco_plugin* by, const char* name, std::vector<sco_arg> args = {}) {
    Reply d;
    g_api->invoke(by, name, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()), OnDone, &d);
    return d;
}
static sco_arg Str(const char* s) { sco_arg a{}; a.type = SCO_ARG_STRING; a.v.s = s; return a; }
static sco_arg Bool(bool b) { sco_arg a{}; a.type = SCO_ARG_BOOL; a.v.i = b; return a; }
static sco_arg Int(int64_t i) { sco_arg a{}; a.type = SCO_ARG_INT; a.v.i = i; return a; }

static bool HasCommand(const char* name) {
    const sco_command* list[512];
    const uint32_t n = g_api->list_commands(list, 512);
    for (uint32_t i = 0; i < n; ++i) if (strcmp(list[i]->name, name) == 0) return true;
    return false;
}

static sco_plugin* g_caller;   // a second plugin that invokes the scripts' commands

// ---- the SDK's own example, sdk/examples/greeter ---------------------------------------------

static void TestGreeterExample(const fs::path& sdk) {
    fs::copy(sdk / "examples/greeter", g_root / "greeter", fs::copy_options::recursive);
    Loaded l = Load("greeter");
    CHECK(l.p && l.p->state == State::Loaded);
    CHECK(Logged("[plugin] loaded greeter 1.0.0 (lua) from greeter/main.lua"));
    CHECK(HasCommand("greeter.greet"));
    Reply r = Invoke(g_caller, "greeter.greet", { Str("Pilot One"), Bool(true) });
    CHECK(r.called && r.r == SCO_OK && r.text == "HI, PILOT ONE!");
    r = Invoke(g_caller, "greeter.greet", { Str("Pilot Two"), Bool(false) });
    CHECK(r.r == SCO_OK && r.text == "Hi, Pilot Two");
    r = Invoke(g_caller, "greeter.greet", { Str("x") });                     // wrong arg count
    CHECK(r.r == SCO_BAD_ARG);
    char status[256];
    CHECK(sco::GetStatus(status, sizeof(status)) && strstr(status, "greeter: Greeter ready"));

    sco::Dispatch("game.ready", nullptr);
    CHECK(Logged("[greeter] warning: teleport is not available on this game build"));
    sco::caps::Set("teleport", true);
    sco::Dispatch("game.ready", nullptr);
    CHECK(Logged("[greeter] teleport is available"));
    sco::Dispatch("game.exit", nullptr);
    CHECK(Logged("[greeter] greeted 2 times"));

    Unload(l);
    CHECK(l.p->state == State::Unloaded && !HasCommand("greeter.greet"));
    CHECK(Invoke(g_caller, "greeter.greet", { Str("x"), Bool(false) }).r == SCO_NOT_FOUND);
}

// ---- the sco table ----------------------------------------------------------------------------

static void TestTable() {
    Write("table", R"(
        local ticks, last, tasks = 0, nil, 0
        local function on_tick(event, ms)
          ticks = ticks + 1; last = ms
          if ticks == 2 then sco.unsubscribe("tick", on_tick) end
        end
        assert(sco.subscribe("tick", on_tick))
        local ok, err = sco.subscribe("tick", on_tick)
        assert(not ok and err == "bad_arg", "same key twice")
        assert(sco.api_major == 1 and sco.api_minor == 0)
        assert(sco.host_version() == "sco-lua test 1.0")
        assert(sco.run_on_game_thread(function() tasks = tasks + 1 end))
        assert(select(2, sco.register_command{ name = "other.x", title = "X", fn = print }) == "bad_arg")
        assert(select(2, sco.register_command{ name = "table.y", title = "Y", fn = print,
                                               args = {{ name = "a", type = "table" }} }) == "bad_arg")
        sco.register_command{ name = "table.state", title = "State",
          fn = function() return string.format("ticks=%d last=%s tasks=%d", ticks, tostring(last), tasks) end }
        sco.register_command{ name = "table.add", title = "Add",
          args = {{ name = "a", type = "int" }, { name = "b", type = "float", help = "second" }},
          fn = function(a, b) return tostring(a + b) end }
        sco.register_command{ name = "table.call", title = "Call",
          fn = function()
            local ok, reply = sco.invoke("table.add", 2, 0.5)
            local bad, why = sco.invoke("table.add", "2", 0.5)
            local none, nf = sco.invoke("nope.nope")
            return table.concat({ tostring(ok), reply, tostring(bad), why, tostring(none), nf }, " ")
          end }
        sco.register_command{ name = "table.list", title = "List",
          fn = function()
            for _, c in ipairs(sco.list_commands()) do
              if c.name == "table.add" then return c.title .. " " .. #c.args .. " " .. c.args[2].type .. " " .. c.args[2].help end
            end
          end }
        sco.register_command{ name = "table.print", title = "Print", fn = function() print("a", 1, nil) end }
    )");
    Loaded l = Load("table");
    CHECK(l.p && l.p->state == State::Loaded);
    CHECK(Invoke(g_caller, "table.state").text == "ticks=0 last=nil tasks=0");
    sco::GameThreadTick(1000);
    CHECK(Invoke(g_caller, "table.state").text == "ticks=1 last=1000 tasks=1");
    sco::GameThreadTick(1100);
    sco::GameThreadTick(1200);                                                // unsubscribed after 2
    CHECK(Invoke(g_caller, "table.state").text == "ticks=2 last=1100 tasks=1");
    CHECK(Invoke(g_caller, "table.add", { Int(40), sco_arg{ SCO_ARG_FLOAT, 0, { .f = 2.5 } } }).text == "42.5");
    CHECK(Invoke(g_caller, "table.call").text == "true 2.5 false bad_arg false not_found");
    CHECK(Invoke(g_caller, "table.list").text == "Add 2 float second");
    CHECK(Invoke(g_caller, "table.print").r == SCO_OK && Logged("[table] a\t1\tnil"));
    Unload(l);
}

// ---- the sandbox ------------------------------------------------------------------------------

static void TestSandbox() {
    Write("box", R"(
        local gone = {}
        for _, n in ipairs{ "io", "os", "package", "require", "debug", "dofile", "loadfile", "load",
                            "loadstring", "collectgarbage", "coroutine", "module" } do
          if _G[n] ~= nil then gone[#gone + 1] = n end
        end
        assert(#gone == 0, "present: " .. table.concat(gone, ","))
        assert(string.dump == nil)
        assert(string and table and math and utf8)
        local ok, err = pcall(setmetatable, {}, { __gc = function() end })
        assert(not ok and err:find("__gc is not allowed"), tostring(err))
        assert(getmetatable(setmetatable({}, { __index = {} })) ~= nil)
    )");
    Loaded l = Load("box");
    CHECK(l.p && l.p->state == State::Loaded);
    if (l.p && l.p->state != State::Loaded) std::printf("  box: %s\n", l.p->reason.c_str());
    Unload(l);

    // Precompiled chunks are refused: the loader passes mode "t".
    Write("bytecode", std::string("\x1bLua\x54\x00", 6) + "junk");
    l = Load("bytecode");
    CHECK(l.p && l.p->state == State::Refused && l.p->reason.find("binary") != std::string::npos);

    Write("syntax", "local x = = 1\n");
    l = Load("syntax");
    CHECK(l.p && l.p->state == State::Refused && l.p->reason.find("main.lua:1:") != std::string::npos);

    // A script that fails at load leaves nothing registered.
    Write("halfway", "sco.register_command{ name = 'halfway.a', title = 'A', fn = print }\n"
                     "sco.subscribe('tick', print)\nerror('stop here')\n");
    const size_t subs = sco::SubscriptionCount();
    l = Load("halfway");
    CHECK(l.p && l.p->state == State::Refused && l.p->reason.find("stop here") != std::string::npos);
    CHECK(!HasCommand("halfway.a") && sco::SubscriptionCount() == subs);
}

// ---- limits -----------------------------------------------------------------------------------

static void TestLimits() {
    // Load itself runs past the budget.
    Write("spin", "while true do end\n");
    Loaded l = Load("spin");
    CHECK(l.p && l.p->state == State::Refused && l.p->reason.find("step budget") != std::string::npos);

    Write("limits", R"(
        local n = 0
        sco.register_command{ name = "limits.spin", title = "Spin", fn = function() while true do end end }
        sco.register_command{ name = "limits.shield", title = "Shield",
          fn = function() while true do pcall(function() while true do end end) end end }
        sco.register_command{ name = "limits.rep", title = "Rep",
          fn = function() return tostring(#string.rep("x", 5000000)) end }
        sco.register_command{ name = "limits.find", title = "Find",
          fn = function() local s = string.rep(string.rep("a", 1000), 2000) return tostring(s:find(string.rep("a", 30) .. "b", 1, true)) end }
        sco.register_command{ name = "limits.sort", title = "Sort",
          fn = function() local t = {} for i = 1, 60000 do t[i] = -i end table.sort(t) return "sorted" end }
        sco.register_command{ name = "limits.ok", title = "OK",
          fn = function() local s = 0 for i = 1, 100000 do s = s + i end return tostring(s) end }
        sco.register_command{ name = "limits.count", title = "Count", fn = function() n = n + 1 return tostring(n) end }
    )");
    // Each budget case gets a fresh copy of the script: an overrun disables it.
    struct Case { const char* cmd; };
    for (const char* cmd : { "limits.spin", "limits.shield", "limits.rep", "limits.find", "limits.sort" }) {
        l = Load("limits");
        CHECK(l.p && l.p->state == State::Loaded);
        CHECK(Invoke(g_caller, "limits.ok").text == "5000050000");             // fits the budget
        const Reply r = Invoke(g_caller, cmd);
        CHECK(r.r == SCO_CRASHED && r.text.find("step budget") != std::string::npos);
        if (r.r != SCO_CRASHED) std::printf("  %s -> %d %s\n", cmd, r.r, r.text.c_str());
        CHECK(!sco_lua_alive(l.p->self));
        CHECK(Invoke(g_caller, "limits.count").r == SCO_UNAVAILABLE);         // disabled
        CHECK(Logged("[limits] error: script disabled: ran past its step budget"));
        Unload(l);
    }

    // Memory: the 64 MiB cap fails the allocation; the script is disabled.
    Write("hog", R"(
        sco.register_command{ name = "hog.eat", title = "Eat", fn = function()
          local mb = string.rep(string.rep("x", 1000), 1000) local t = {} for i = 1, 100 do t[i] = mb .. i end end }
        sco.register_command{ name = "hog.ok", title = "OK", fn = function() return "fine" end }
    )");
    l = Load("hog");
    CHECK(Invoke(g_caller, "hog.ok").text == "fine");
    const Reply r = Invoke(g_caller, "hog.eat");
    CHECK(r.r == SCO_TOO_MANY && r.text.find("not enough memory") != std::string::npos);
    CHECK(!sco_lua_alive(l.p->self) && Logged("[hog] error: script disabled: out of memory"));
    Unload(l);

    // Three errors in callbacks disable the script; the first two only log.
    Write("flaky", R"(
        sco.register_command{ name = "flaky.boom", title = "Boom", fn = function() error("boom") end }
        sco.register_command{ name = "flaky.ok", title = "OK", fn = function() return "ok" end }
        sco.register_command{ name = "flaky.badreply", title = "Bad", fn = function() return {} end }
    )");
    l = Load("flaky");
    Reply b = Invoke(g_caller, "flaky.boom");
    CHECK(b.r == SCO_BAD_ARG && b.text.find("main.lua:2: boom") != std::string::npos);
    CHECK(Invoke(g_caller, "flaky.badreply").text.find("reply must be a string or nil, not table") != std::string::npos);
    CHECK(Invoke(g_caller, "flaky.ok").text == "ok");
    CHECK(Invoke(g_caller, "flaky.boom").r == SCO_BAD_ARG);
    CHECK(Invoke(g_caller, "flaky.ok").r == SCO_UNAVAILABLE && Logged("[flaky] error: script disabled: 3 errors"));
    Unload(l);

    // An event callback error is logged with the event name.
    Write("evt", "sco.subscribe('game.ready', function() error('nope') end)\n");
    l = Load("evt");
    sco::Dispatch("game.ready", nullptr);
    CHECK(Logged("[evt] error: event game.ready: main.lua:1: nope"));
    Unload(l);
}

#ifndef _WIN32
#include <unistd.h>
// A broken step budget turns the spin tests into endless loops: fail instead of hanging.
static void Deadline(int) {
    static const char msg[] = "FAIL test_lua: still running after 120 s (step budget not enforced?)\n";
    (void)!write(1, msg, sizeof(msg) - 1);
    _exit(1);
}
#endif

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: test_lua <sdk dir> <out dir>\n"); return 2; }
#ifndef _WIN32
    std::signal(SIGALRM, Deadline);
    alarm(120);
#endif
    sco::SetLogSink(Sink);
    sco::SetGameThread();
    g_api = sco::host::BuildApi({ "sco-lua test 1.0" });
    g_caller = sco::host::NewPlugin("caller");
    g_root = fs::path(argv[2]) / "lua-plugins";
    fs::remove_all(g_root);
    fs::create_directories(g_root);
    TestGreeterExample(argv[1]);
    TestTable();
    TestSandbox();
    TestLimits();
    std::printf("sco-lua tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
