#pragma once
// The host kit: the startup order every product needs, once, over the game core, caps, the host
// table and the plugin loader. A product (sc-offline, sco-host-sim) supplies a Platform and calls
// Start once on the game thread, Tick from its main-thread hook and Stop when the game closes.
// C++ and internal: plugins only see sco_api.h.
//
//   static const sco::plugins::Builtin kBuiltins[] = { { "teleport", Query, Load, Unload } };
//   static const sco::plugins::ScriptRuntime kLua{ sco_lua_load, sco_lua_unload };
//   sco::SetLogSink(WriteModLog);
//   sco::app::Platform pf;
//   pf.hostVersion = "sc-offline 0.8.0";
//   pf.pluginRoot = "data/plugins";
//   pf.pluginsEnabled = opts.plugins;
//   pf.builtins = kBuiltins; pf.nBuiltins = std::size(kBuiltins);
//   pf.scripts = &kLua;
//   pf.image = &image;                         // sco::ModuleImage() in game
//   pf.setCapabilities = SetFeatureCaps;       // reads the resolved rows
//   sco::app::Start(pf);                       // game thread, after the product's own patches
//   ... sco::app::Tick(nowMs) on every main-thread tick ...
//   sco::app::Stop();                          // game thread, from the game's own quit path
//
// Start, in order:
//   1. SetGameThread (the caller's thread)
//   2. image set: RegisterGameSignatures + ResolveAll(*image)
//   3. setCapabilities() (after the rows are resolved, so it can use caps::SetFromSignatures)
//   4. host::BuildApi({ hostVersion })
//   5. the list: every built-in (FromBuiltin), then, with pluginsEnabled, Discover(pluginRoot)
//   6. ContainCallouts(list)
//   7. LoadBuiltin for each built-in; then, in list order, LoadNative / LoadScript (a lua plugin
//      is refused "no script runtime" when scripts is null); then the content index
//   8. image set: LogSignatureReport(false); then LogReport
//   9. Dispatch "game.ready"
// Startup problems are logged and reported, never fatal: a product with no image, no plugin
// folder or a refused plugin still starts, and every plugin that can load does.
// Stop: Dispatch "game.exit", UnloadAll (newest first, built-ins last), ContainCallouts(nullptr).
#include "sco/plugins.h"
#include "sco/scan.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace sco::app {

struct Platform {
    const char* hostVersion = nullptr;             // "sc-offline 0.8.0"; static string (host_version())
    std::filesystem::path pluginRoot;              // data/plugins
    bool pluginsEnabled = false;                   // plugins = on|off; built-ins load either way
    const plugins::Builtin* builtins = nullptr;    // the product's own features, loaded first
    size_t nBuiltins = 0;
    const plugins::ScriptRuntime* scripts = nullptr;   // sco-lua, or nullptr: lua plugins refused
    const Image* image = nullptr;                  // resolve signatures against this; nullptr skips
    void (*setCapabilities)() = nullptr;           // the product sets caps after ResolveAll
    plugins::ModuleOps moduleOps = plugins::PlatformModuleOps();
    // Phase 5 (docs/framework.md) adds the storage backend and host services here, as further
    // fields with defaults, so a Platform built today keeps compiling.
};

// Starts the host kit (see above). Game thread: it becomes the game thread. False, and nothing
// changes, when already started and not stopped; true otherwise, whatever failed to load.
// `platform` is copied; builtins, scripts and hostVersion must outlive Stop.
bool Start(const Platform& platform);

// GameThreadTick(nowMs): queued tasks, then "tick". No-op unless started.
void Tick(uint32_t nowMs);

// Dispatches "game.exit", then unloads every plugin (UnloadAll: newest first, built-ins last)
// and removes the callout guard. Game thread. No-op unless started. The list stays readable
// (final states) until the next Start.
// Call it from the game's own quit path, on the game thread (sc-offline: a hook on the
// system.quit row, CSystem::Quit). Not from a WM_QUIT hook: the game's Quit ends the process
// with "System Fast Shutdown (ExitOnQuit enabled)" and the message loop never sees WM_QUIT.
// Never from DLL_PROCESS_DETACH: that runs under the loader lock, at the wrong time.
// game.exit is best effort: a crash or a killed process never sends it (docs/framework.md,
// Lessons).
void Stop();

// Every plugin of the last Start: built-ins first, then discovered folders. Stable from Start
// to the next Start (never resized in between).
const std::vector<plugins::Plugin>& Plugins();

// The data-pack content index of the last Start.
const plugins::ContentIndex& Content();

}  // namespace sco::app
