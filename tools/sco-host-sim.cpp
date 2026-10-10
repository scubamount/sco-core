// sco-host-sim: the real host kit (sco::app), runtime, loader and sco-lua outside the game.
//
//   sco-host-sim <plugin root> [--exe StarCitizen.exe] [--cap NAME]... [--ticks N] [--no-lua]
//                [--invoke NAME [ARG]...]
//
// Loads the built-in demo plugin `sim` (command sim.ping) and every plugin under <plugin root>
// with plugins on, runs N ticks (default 1, 100 ms apart), invokes one command (arguments parsed
// against its arg defs: int, float, string, bool as 1/0/true/false), then stops: game.exit and
// unload. Log lines go to stdout as they would to mod.log. Every datacore/*.toml the content index
// holds is parsed as sco-dcb lint does (names need a Game2.dcb: sco-dcb check); a file that doesn't
// parse fails the run.
//   --exe    resolve the signature tables against a StarCitizen.exe on disk (as sco-sigcheck)
//   --cap    set a capability ready (after the signatures, as a product would)
//   --no-lua no script runtime: lua plugins are refused
// Exit 0 when every plugin ended loaded, off or disabled (none refused or crashed, at start or
// stop) and the invoke, if any, answered OK; 1 otherwise; 2 for bad arguments or an unreadable
// --exe.
#include "sco/app.h"
#include "sco/game/signatures.h"
#include "sco/caps.h"
#include "sco/datacore_pack.h"
#include "sco/log.h"
#include "sco/pe_file.h"
#include "sco/runtime.h"
#include "sco_api.h"
#include "sco_lua.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace P = sco::plugins;

namespace {

void Sink(const char* line) {
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// ---- the built-in demo plugin `sim` ---------------------------------------------------------

uint32_t g_ticks;

const sco_plugin_info kSimInfo = { sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "sim", "1.0.0", "sco-host-sim" };

sco_result Ping(const sco_arg*, uint32_t, void*, char* reply, uint32_t replySize) {
    std::snprintf(reply, replySize, "pong after %u ticks", static_cast<unsigned>(g_ticks));
    return SCO_OK;
}

void OnTick(const char*, const void*, void*) { ++g_ticks; }

const sco_plugin_info* SimQuery() { return &kSimInfo; }

sco_result SimLoad(const sco_api* api, sco_plugin* self) {
    g_ticks = 0;
    sco_command cmd{};
    cmd.size = sizeof(cmd);
    cmd.name = "sim.ping";
    cmd.title = "Ping";
    cmd.help = "Answers with the number of ticks so far";
    cmd.arg_def_size = sizeof(sco_arg_def);
    cmd.fn = Ping;
    if (const sco_result r = api->register_command(self, &cmd); r != SCO_OK) return r;
    return api->subscribe(self, "tick", OnTick, nullptr);
}

void SimUnload() {}

const P::Builtin kBuiltins[] = { { "sim", SimQuery, SimLoad, SimUnload } };
const P::ScriptRuntime kLua{ sco_lua_load, sco_lua_unload };

// ---- options --------------------------------------------------------------------------------

std::vector<const char*> g_caps;

void SetCaps() {
    for (const char* c : g_caps)
        if (sco::caps::Set(c, true) != sco::Result::Ok) std::printf("[sim] bad capability name '%s'\n", c);
}

bool ParseArg(const char* text, sco::ArgType type, sco::Arg& out) {
    out = {};
    out.type = type;
    char* end = nullptr;
    switch (type) {
        case sco::ArgType::Int:    out.v.i = std::strtoll(text, &end, 10); return *text && !*end;
        case sco::ArgType::Float:  out.v.f = std::strtod(text, &end);       return *text && !*end;
        case sco::ArgType::String: out.v.s = text; return true;
        case sco::ArgType::Bool:
            if (!std::strcmp(text, "1") || !std::strcmp(text, "true"))  { out.v.i = 1; return true; }
            if (!std::strcmp(text, "0") || !std::strcmp(text, "false")) { out.v.i = 0; return true; }
            return false;
    }
    return false;
}

struct Reply { bool called = false; sco::Result r = sco::Result::Ok; std::string text; };
void OnDone(sco::Result r, const char* reply, void* ctx) {
    auto* d = static_cast<Reply*>(ctx);
    d->called = true;
    d->r = r;
    d->text = reply ? reply : "";
}

// Runs the command on the game thread as the host (no owner). False unless it answered OK.
bool RunInvoke(const char* name, char** argv, int argc) {
    const sco::Command* cmds[sco::kMaxCommands];
    const size_t n = sco::ListCommands(cmds, sco::kMaxCommands);
    const sco::Command* cmd = nullptr;
    for (size_t i = 0; i < n; ++i)
        if (std::strcmp(cmds[i]->name, name) == 0) cmd = cmds[i];
    if (!cmd) { std::printf("[sim] invoke %s: no such command\n", name); return false; }
    if (static_cast<uint32_t>(argc) != cmd->nargs) {
        std::printf("[sim] invoke %s: needs %u argument(s), got %d\n", name, static_cast<unsigned>(cmd->nargs), argc);
        return false;
    }
    sco::Arg args[sco::kMaxCommandArgs];
    for (int i = 0; i < argc; ++i)
        if (!ParseArg(argv[i], cmd->args[i].type, args[i])) {
            std::printf("[sim] invoke %s: bad argument %d '%s' for %s\n", name, i + 1, argv[i], cmd->args[i].name);
            return false;
        }
    Reply reply;
    sco::Invoke(name, argc ? args : nullptr, static_cast<uint32_t>(argc), OnDone, &reply);
    std::printf("[sim] invoke %s -> %s \"%s\"\n", name, sco::ResultName(reply.r), reply.text.c_str());
    return reply.called && reply.r == sco::Result::Ok;
}

int Usage() {
    std::fprintf(stderr, "usage: sco-host-sim <plugin root> [--exe StarCitizen.exe] [--cap NAME]... [--ticks N] [--no-lua]\n"
                         "                    [--invoke NAME [ARG]...]\n");
    return 2;
}

// The pack lint for every indexed datacore/*.toml. False if one doesn't parse.
bool LintDataCore() {
    bool ok = true;
    for (const P::ContentItem* item : sco::app::Content().Items(P::ContentKind::DataCore)) {
        std::ifstream in(item->path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        sco::datacore::Pack pack;
        std::string error;
        if (in && sco::datacore::ParsePack(text.str(), pack, error)) {
            std::printf("[sim] datacore %s %s: OK, %zu operations\n", item->plugin.c_str(), item->name.c_str(), pack.ops.size());
        } else {
            std::printf("[sim] datacore %s %s: %s\n", item->plugin.c_str(), item->name.c_str(), in ? error.c_str() : "unreadable");
            ok = false;
        }
    }
    return ok;
}

// Every plugin loaded, off or disabled (at start); none refused or crashed (at stop: unloaded
// counts too). Prints the ones that aren't.
bool AllHealthy(bool stopped) {
    bool ok = true;
    for (const auto& p : sco::app::Plugins()) {
        const bool good = p.state == P::State::Loaded || p.state == P::State::Off || p.state == P::State::Disabled ||
                          (stopped && p.state == P::State::Unloaded);
        if (!good) { std::printf("[sim] not healthy: %s\n", P::Describe(p).c_str()); ok = false; }
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argv[1][0] == '-') return Usage();
    const char* root = argv[1];
    const char* exe = nullptr;
    const char* invokeName = nullptr;
    char** invokeArgs = nullptr;
    int nInvokeArgs = 0;
    long ticks = 1;
    bool lua = true;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!std::strcmp(argv[i], "--cap") && i + 1 < argc) g_caps.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--ticks") && i + 1 < argc) {
            char* end = nullptr;
            ticks = std::strtol(argv[++i], &end, 10);
            if (!*argv[i] || *end || ticks < 0 || ticks > 1000000) return Usage();
        } else if (!std::strcmp(argv[i], "--no-lua")) lua = false;
        else if (!std::strcmp(argv[i], "--invoke") && i + 1 < argc) {
            invokeName = argv[++i];
            invokeArgs = argv + i + 1;
            nInvokeArgs = argc - i - 1;
            break;
        } else return Usage();
    }

    sco::SetLogSink(Sink);
    sco::FileImage file;
    if (exe && !file.Load(exe)) {
        std::printf("[sim] cannot read %s: %s\n", exe, file.error.c_str());
        return 2;
    }

    sco::app::Platform pf;
    pf.hostVersion = "sco-host-sim 1.0";
    pf.pluginRoot = root;
    pf.pluginsEnabled = true;
    pf.builtins = kBuiltins;
    pf.nBuiltins = sizeof(kBuiltins) / sizeof(kBuiltins[0]);
    pf.scripts = lua ? &kLua : nullptr;
    pf.image = exe ? &file.img : nullptr;
    pf.registerSignatures = sco::game::RegisterGameSignatures;   // --exe: Star Citizen's tables
    pf.setCapabilities = SetCaps;
    if (!sco::app::Start(pf)) return 1;

    bool ok = AllHealthy(false);
    ok = LintDataCore() && ok;
    for (long t = 1; t <= ticks; ++t) sco::app::Tick(static_cast<uint32_t>(t * 100));
    if (invokeName && !RunInvoke(invokeName, invokeArgs, nInvokeArgs)) ok = false;
    sco::app::Stop();
    ok = AllHealthy(true) && ok;
    std::printf("[sim] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
