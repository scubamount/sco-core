// The host kit: Start / Tick / Stop over the game core, caps, the host table and the loader.
// Everything runs on the game thread; the state below is touched by nothing else.
#include "sco/app.h"
#include "sco/caps.h"
#include "sco/game/signatures.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/runtime.h"
#include "sco/signatures.h"
#include "sco/storage.h"
#include <utility>

namespace sco::app {

namespace {

bool                         g_started = false;
Platform                     g_platform;
std::vector<plugins::Plugin> g_list;     // never resized while ContainCallouts points at it
plugins::ContentIndex        g_index;

int HasCap(const char* name) { return caps::Has(name) ? 1 : 0; }

}  // namespace

bool Start(const Platform& platform) {
    if (g_started) {
        Log("[app] start refused: already started");
        return false;
    }
    SetGameThread();
    g_platform = platform;
    const Platform& pf = g_platform;

    if (pf.image) {
        if (!game::RegisterGameSignatures()) Log("[app] game signature tables did not all register");
        ResolveAll(*pf.image);
    }
    if (pf.setCapabilities) pf.setCapabilities();

    const char* version = pf.hostVersion ? pf.hostVersion : "sco-core host";
    const sco_api* api = host::BuildApi({ version });
    if (!pf.dataRoot.empty()) {   // before any plugin loads, built-ins included
        storage::Options so;
        so.dataRoot = pf.dataRoot;
        const Result sr = storage::Start(so);
        if (sr != Result::Ok) Log("[app] storage not started: %s", ResultName(sr));
    }

    plugins::Options opts;
    opts.enabled = pf.pluginsEnabled;
    opts.has = HasCap;
    opts.builtins = pf.builtins;
    opts.nBuiltins = pf.nBuiltins;

    // The whole list exists before ContainCallouts, so it never moves while the guard holds it.
    std::vector<plugins::Plugin> found;
    if (pf.pluginsEnabled && !pf.pluginRoot.empty()) found = plugins::Discover(pf.pluginRoot, opts);
    std::vector<plugins::Plugin> list;
    list.reserve((pf.builtins ? pf.nBuiltins : 0) + found.size());
    for (size_t i = 0; pf.builtins && i < pf.nBuiltins; ++i) {
        list.push_back(plugins::FromBuiltin(pf.builtins[i]));
        const plugins::Plugin& b = list.back();
        if (b.state == plugins::State::Refused) Log("[plugin] refused %s: %s", b.folder.c_str(), b.reason.c_str());
    }
    for (auto& p : found) list.push_back(std::move(p));
    g_list = std::move(list);
    g_index.Clear();
    plugins::ContainCallouts(&g_list);
    g_started = true;

    // Built-ins first: they are the product's features, and register before any plugin can.
    for (auto& p : g_list)
        if (p.state == plugins::State::Ready && p.manifest.kind == plugins::Kind::Builtin)
            plugins::LoadBuiltin(p, api, host::NewPlugin(p.manifest.id.c_str()), opts);
    for (auto& p : g_list) {
        if (p.state != plugins::State::Ready) continue;
        if (p.manifest.kind == plugins::Kind::Native) {
            plugins::LoadNative(p, api, host::NewPlugin(p.manifest.id.c_str()), opts, pf.moduleOps);
        } else if (p.manifest.kind == plugins::Kind::Lua) {
            if (pf.scripts) {
                plugins::LoadScript(p, api, host::NewPlugin(p.manifest.id.c_str()), *pf.scripts);
            } else {
                p.state = plugins::State::Refused;
                p.reason = "no script runtime";
                Log("[plugin] refused %s: %s", p.manifest.id.c_str(), p.reason.c_str());
            }
        }
    }
    g_index.Build(g_list);

    if (pf.image) LogSignatureReport(false);
    plugins::LogReport(g_list, pf.pluginsEnabled);

    const Result r = Dispatch("game.ready", nullptr);
    if (r != Result::Ok) Log("[app] game.ready: %s", ResultName(r));
    return true;
}

void Tick(uint32_t nowMs) {
    if (!g_started) return;
    const Result r = GameThreadTick(nowMs);
    if (r != Result::Ok) Log("[app] tick: %s", ResultName(r));
}

void Stop() {
    if (!g_started) return;
    const Result r = Dispatch("game.exit", nullptr);
    if (r != Result::Ok) Log("[app] game.exit: %s", ResultName(r));
    plugins::UnloadAll(g_list, g_platform.moduleOps, g_platform.scripts);
    storage::Stop();
    host::WithdrawHostServices();
    plugins::ContainCallouts(nullptr);
    g_started = false;
}

const std::vector<plugins::Plugin>& Plugins() { return g_list; }

const plugins::ContentIndex& Content() { return g_index; }

}  // namespace sco::app
