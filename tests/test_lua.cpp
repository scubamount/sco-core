// Tests for sco-lua (plugins/lua) loaded through the real plugin loader and the real host table:
// Discover -> LoadScript -> sco_lua_load, with sco::host::BuildApi's sco_api underneath. Host
// build, no game. The "game thread" is this test's main thread.
//   tools/test.sh
#include "dcb_builder.h"
#include "sco/caps.h"
#include "sco/datacore_service.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/plugins.h"
#include "sco/runtime.h"
#include "sco/status.h"
#include "sco/ui.h"
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
        for i = 1, 64 do assert(sco.subscribe("game.exit", function() return i end)) end
        local full, why = sco.subscribe("game.exit", function() end)
        assert(not full and why == "too_many", "65th function on one event: " .. tostring(why))
        assert(sco.api_major == 1 and sco.api_minor == 1)
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
            local float, fw = sco.invoke("table.add", 2.0, 0.5)              -- int takes no float
            return table.concat({ tostring(ok), reply, tostring(bad), why, tostring(none), nf, tostring(float), fw }, " ")
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
    CHECK(Invoke(g_caller, "table.call").text == "true 2.5 false bad_arg false not_found false bad_arg");
    CHECK(Invoke(g_caller, "table.list").text == "Add 2 float second");
    CHECK(Invoke(g_caller, "table.print").r == SCO_OK && Logged("[table] a\t1\tnil"));
    Unload(l);
}

// ---- sco.ui hotkeys ---------------------------------------------------------------------------

static void TestHotkeys() {
    CHECK(sco::ui::Start() == Result::Ok);
    CHECK(sco::ui::ReserveChord("f6") == Result::Ok);
    Write("keys", R"(
        sco.register_command{ name = "keys.add", title = "Add",
          args = {{ name = "a", type = "int" }, { name = "b", type = "float" }},
          fn = function(a, b) return tostring(a + b) end }
        assert(sco.bind_hotkey("Ctrl+Alt+K", "keys.add", 40, 2))                 -- 2 is a float: keys.add says so
        assert(sco.bind_hotkey("ctrl+alt+l", "later.cmd", "x", true, 1.5))      -- not registered: Lua types
        local ok, why, msg = sco.bind_hotkey("f6", "keys.add", 1, 2)
        assert(not ok and why == "bad_arg" and msg == "f6 is reserved by the host", tostring(msg))
        ok, why, msg = sco.bind_hotkey("alt+ctrl+k", "keys.add", 1, 2)
        assert(not ok and msg == "ctrl+alt+k is bound by 'keys' to keys.add", tostring(msg))
        assert(select(2, sco.bind_hotkey("ctrl+alt+m", "keys.add", 1)) == "bad_arg")        -- arg count
        assert(select(2, sco.bind_hotkey("ctrl+alt+m", "keys.add", "1", 2)) == "bad_arg")  -- arg type
        assert(select(2, sco.bind_hotkey("ctrl+alt+m", "later.cmd", {})) == "bad_arg")    -- a table
        assert(select(2, sco.bind_hotkey("ctrl++", "keys.add", 1, 2)) == "bad_arg")
        assert(select(2, sco.unbind_hotkey("f12")) == "not_found")
        assert(sco.bind_hotkey("f12", "keys.add", 1, 2) and sco.unbind_hotkey("F12"))
    )");
    Loaded l = Load("keys");
    CHECK(l.p && l.p->state == State::Loaded);
    std::string reply;
    CHECK(sco::ui::Dispatch("alt+ctrl+k", &reply) == Result::Ok && reply == "42.0");
    const auto keys = sco::ui::Hotkeys();
    CHECK(keys.size() == 2 && keys[0].chord == "ctrl+alt+k" && keys[0].owner == "keys" && keys[1].nargs == 3);
    Unload(l);
    CHECK(sco::ui::Hotkeys().empty());   // withdrawn with the script
    sco::ui::Stop();
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

    // Disabled during load (three errors in its own command, reached through sco.invoke): the
    // load chunk keeps running, but sco.* mutators answer unavailable and the load fails.
    Write("selfkill", R"(
        sco.register_command{ name = "selfkill.boom", title = "Boom", fn = function() error("boom") end }
        for _ = 1, 3 do sco.invoke("selfkill.boom") end
        local _, a = sco.subscribe("tick", print)
        local _, b = sco.run_on_game_thread(print)
        local _, c = sco.register_command{ name = "selfkill.more", title = "More", fn = print }
        sco.log("info", table.concat({ tostring(a), tostring(b), tostring(c) }, " "))
    )");
    l = Load("selfkill");
    CHECK(l.p && l.p->state == State::Refused && l.p->reason.find("disabled") != std::string::npos);
    if (l.p && l.p->state != State::Refused) std::printf("  selfkill: loaded\n");
    CHECK(Logged("[selfkill] unavailable unavailable unavailable"));
    CHECK(!HasCommand("selfkill.more") && sco::SubscriptionCount() == subs);
}

// ---- limits -----------------------------------------------------------------------------------

static void TestLimits() {
    // Load itself runs past the budget.
    Write("spin", "while true do end\n");
    Loaded l = Load("spin");
    CHECK(l.p && l.p->state == State::Refused && l.p->reason.find("step budget") != std::string::npos);

    Write("limits", R"(
        local n = 0
        local function big(len, c) local s = c or "x" while #s < len do s = s .. s end return s:sub(1, len) end
        sco.register_command{ name = "limits.spin", title = "Spin", fn = function() while true do end end }
        sco.register_command{ name = "limits.shield", title = "Shield",
          fn = function() while true do pcall(function() while true do end end) end end }
        sco.register_command{ name = "limits.rep", title = "Rep",
          fn = function() return tostring(#string.rep("x", 5000000)) end }
        sco.register_command{ name = "limits.find", title = "Find",
          fn = function() local s = string.rep(string.rep("a", 1000), 2000) return tostring(s:find(string.rep("a", 30) .. "b", 1, true)) end }
        -- One library call, many values: charged by size, not 1 step per call.
        sco.register_command{ name = "limits.unpack", title = "Unpack", fn = function()
          local t = table.pack(big(500000):byte(1, -1))
          for _ = 1, 1000 do table.unpack(t, 1, t.n) end
          return "unpacked" end }
        sco.register_command{ name = "limits.byte", title = "Byte", fn = function()
          local s = big(500000)
          for _ = 1, 1000 do s:byte(1, -1) end
          return "bytes" end }
        sco.register_command{ name = "limits.needle", title = "Needle", fn = function()
          local s = big(4194304)
          return tostring(s:find(s:sub(1, 2097152) .. "y", 1, true)) end }
        sco.register_command{ name = "limits.class", title = "Class", fn = function()
          return tostring(big(1000000):find("[" .. big(1048576, "y") .. "x]*")) end }
        sco.register_command{ name = "limits.sort", title = "Sort",
          fn = function() local t = {} for i = 1, 60000 do t[i] = -i end table.sort(t) return "sorted" end }
        sco.register_command{ name = "limits.ok", title = "OK",
          fn = function() local s = 0 for i = 1, 100000 do s = s + i end return tostring(s) end }
        sco.register_command{ name = "limits.count", title = "Count", fn = function() n = n + 1 return tostring(n) end }
    )");
    // Each budget case gets a fresh copy of the script: an overrun disables it.
    struct Case { const char* cmd; };
    for (const char* cmd : { "limits.spin", "limits.shield", "limits.rep", "limits.find", "limits.sort",
                             "limits.unpack", "limits.byte", "limits.needle", "limits.class" }) {
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

    // One budget per outermost entry: a script can't multiply it by calling another script's
    // command in a loop (each call alone fits the budget).
    Write("worker", R"(
        sco.register_command{ name = "worker.chunk", title = "Chunk",
          fn = function() local s = 0 for i = 1, 150000 do s = s + i end return "done" end }
    )");
    Write("boss", R"(
        sco.register_command{ name = "boss.run", title = "Run", fn = function()
          for i = 1, 100 do
            local ok, why = sco.invoke("worker.chunk")
            if not ok then return "stopped at " .. i .. ": " .. tostring(why) end
          end
          return "ran all 100"
        end }
    )");
    {
        Loaded w = Load("worker");
        Loaded bo = Load("boss");
        CHECK(Invoke(g_caller, "worker.chunk").text == "done");                // alone: fits
        const Reply r = Invoke(g_caller, "boss.run");
        CHECK(r.r == SCO_CRASHED && r.text.find("step budget") != std::string::npos);
        if (r.r != SCO_CRASHED) std::printf("  boss.run -> %d %s\n", r.r, r.text.c_str());
        CHECK(!sco_lua_alive(bo.p->self));
        Unload(bo);
        Unload(w);
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

    // A non-string error object raised with the heap at the cap: turning it into text must not
    // allocate outside protected mode ("42" would need a new string, the allocation fails with no
    // handler, and Lua aborts the process). fill() pins memory to the last few bytes, inside
    // pcall, then the callback raises 42.
    static const char* const kFill = R"(
        local slots, n = {}, 0
        for i = 1, 1024 do slots[i] = false end
        local big = "x"
        for _ = 1, 20 do big = big .. big end
        local bytes = {}
        for i = 128, 191 do bytes[#bytes + 1] = string.char(i) end
        local size, a, b, filled = 0, 1, 0, false
        local function long() n = n + 1; slots[n] = big:sub(1, size) end
        local function short()
          b = b + 1; if b > #bytes then a, b = a + 1, 1 end
          n = n + 1; slots[n] = bytes[a] .. bytes[b]
        end
        local function fill()
          if filled then return end
          filled = true
          for _, s in ipairs{ 1048576, 65536, 4096, 256, 48 } do
            size = s
            while pcall(long) do end
          end
          while pcall(short) do end
        end
    )";
    Write("full", std::string(kFill) +
          "sco.register_command{ name = 'full.boom', title = 'Boom', fn = function() fill() error(42) end }\n");
    l = Load("full");
    CHECK(l.p && l.p->state == State::Loaded);
    const Reply full = Invoke(g_caller, "full.boom");
    CHECK(full.r == SCO_BAD_ARG && full.text == "42");
    if (full.text != "42") std::printf("  full.boom -> %d %s\n", full.r, full.text.c_str());
    CHECK(Logged("[full] error: full.boom: 42"));
    Invoke(g_caller, "full.boom");
    Invoke(g_caller, "full.boom");
    CHECK(!sco_lua_alive(l.p->self));                                        // 3 errors (or out of memory)
    Unload(l);
    Write("fullevt", std::string(kFill) + "sco.subscribe('game.ready', function() fill() error(42) end)\n");
    l = Load("fullevt");
    CHECK(l.p && l.p->state == State::Loaded);
    sco::Dispatch("game.ready", nullptr);
    CHECK(Logged("[fullevt] error: event game.ready: 42"));
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

    // Each task gets a fresh budget, so a script may have at most 16 waiting: a task that queues
    // two more each tick levels off at 16 instead of filling the host's 256-slot queue.
    Write("fan", R"(
        local queued, most, refused = 0, 0, 0
        local function task()
          queued = queued - 1
          for _ = 1, 2 do
            local ok, why = sco.run_on_game_thread(task)
            if ok then queued = queued + 1; most = math.max(most, queued)
            elseif why == "too_many" then refused = refused + 1 end
          end
        end
        assert(sco.run_on_game_thread(task))
        queued, most = 1, 1
        sco.register_command{ name = "fan.state", title = "State",
          fn = function() return string.format("most=%d refused=%s", most, tostring(refused > 0)) end }
    )");
    l = Load("fan");
    CHECK(l.p && l.p->state == State::Loaded);
    for (uint32_t t = 1; t <= 10; ++t) sco::GameThreadTick(t * 100);
    const Reply fan = Invoke(g_caller, "fan.state");
    CHECK(fan.text == "most=16 refused=true");
    if (fan.text != "most=16 refused=true") std::printf("  fan.state -> %s\n", fan.text.c_str());
    CHECK(sco_lua_alive(l.p->self));
    Unload(l);
}

// ---- sco.datacore (published by the host only when the product enables it) --------------------

static void TestDataCore() {
    namespace svc = sco::datacore::service;
    Write("dcnone", "assert(sco.datacore == nil, 'not published')\n");
    Loaded none = Load("dcnone");
    CHECK(none.p && none.p->state == State::Loaded);
    Unload(none);

    const fs::path data = g_root.parent_path() / "lua_datacore";
    fs::remove_all(data);
    svc::Options o;
    o.dataRoot = data;
    CHECK(svc::Start(o) == Result::Ok);
    Write("dcmod", R"(
local dc = assert(sco.datacore, "sco.datacore")
assert(dc.state() == "open")
local p = assert(dc.begin())
assert(p:set("ShipA", "speed", 2.5))
local part = assert(p:add_instance("Part", "PartX"))
assert(p:set_pointer("ShipB", "engine", part))
assert(p:set("ShipB", "label", "lua"))
assert(p:set("ShipA", "kind", { enum = "Small" }))
assert(p:set("ShipA", "maker", { ref = "BaseOne" }))
assert(p:append("ShipA", "parts", part))
assert(p:append("ShipA", "parts", nil))
local ok, err = p:set("ShipA", "speed..x", 1)
assert(not ok and err == "bad_arg", "path syntax")
assert(select(2, p:set("ShipA", "flag", {})) == "bad_arg", "a table that is no value")
assert(select(2, p:add_record("Ship", "ShipC")) == "unavailable", "add_record")
assert(p:commit())
assert(select(2, p:commit()) == "bad_arg", "committed twice")
local r = p:report()
assert(#r == 9 and r[1].state == "queued" and r[1].op == 1 and r[9].op == 0, "report before the load")
local q = assert(dc.begin({ atomic = false }))
assert(q:discard())
assert(select(2, q:commit()) == "not_found", "discarded")
sco.register_command{ name = "dcmod.report", title = "Report", fn = function()
  local rr = p:report()
  return rr[#rr].state .. " " .. rr[1].state .. " " .. sco.datacore.state()
end }
)");
    Loaded l = Load("dcmod");
    CHECK(l.p && l.p->state == State::Loaded);
    if (l.p && l.p->state != State::Loaded) std::printf("  dcmod: %s\n", l.p->reason.c_str());

    const std::vector<uint8_t> file = dcb::ValuedFixture(36).Build();
    sco::datacore::Schema s;
    CHECK(s.Parse(file));
    P::ContentIndex index;
    const svc::LoadResult r = svc::Load(s, l.list, index, data);
    CHECK(r.result.status.ok() && r.result.packs.size() == 1 && r.result.packs[0].state == sco::datacore::PackState::Applied);
    CHECK(r.result.packs.size() == 1 && r.result.packs[0].applied == 8);
    const Reply rep = Invoke(g_caller, "dcmod.report");
    CHECK(rep.r == SCO_OK && rep.text == "applied applied loaded");
    Unload(l);
    svc::Stop();
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
    TestHotkeys();
    TestSandbox();
    TestLimits();
    TestDataCore();
    std::printf("sco-lua tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
