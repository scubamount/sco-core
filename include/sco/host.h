#pragma once
// The host side of sco_api.h: the one sco_api table plugins get, built over the runtime
// (sco/runtime.h), capabilities (sco/caps.h), status and log, and the per-plugin sco_plugin
// handles. C++ and internal: sc-offline calls this; plugins only see the table.
//
// Startup, on the game thread:
//   const sco_api* api = sco::host::BuildApi({ "sc-offline 0.8.0" });
//   sco_plugin* self = sco::host::NewPlugin(manifest.id);   // one per plugin load
//   ... sco_plugin_load(api, self) ...
//   sco::Release(self);                                      // unload, failed load or crash
//
// A handle is also the plugin's runtime owner: everything the plugin adds through the table
// (tasks, subscriptions, commands, queued invokes) is owned by `self`, so sco::Release(self)
// removes it all, and every later table call naming `self` returns SCO_BAD_ARG (status and log
// are dropped).
//
// Table behavior beyond docs/api-v1.md:
//   - Every function that takes `self` returns SCO_BAD_ARG for a pointer NewPlugin didn't return.
//   - register_command reads the caller's sco_command only up to its `size` (v1.0 needs all of
//     it: a smaller size is SCO_BAD_ARG) and its arg defs with `arg_def_size` (at least
//     sizeof(sco_arg_def), a multiple of 8). The command prefix is the plugin's id.
//   - list_commands lists every live command, host features' and plugins' alike, as views the
//     host builds once per registration; they stay readable for the life of the process. In a
//     view fn and ctx are NULL and args has stride sizeof(sco_arg_def): run commands with invoke.
//   - log writes "[<id>] message", "[<id>] warning: message" or "[<id>] error: message".
#include "sco_api.h"
#include "sco/runtime.h"
#include <cstddef>

namespace sco::host {

struct HostInfo {
    const char* version;   // "sc-offline 0.8.0"; a static string, returned by host_version()
};

// Returns the process's one sco_api table and wires the command registry's capability check to
// sco::caps::Has. Call at startup, before any plugin loads. Calling again updates the version
// and returns the same table. nullptr (and nothing changes) when info.version is null.
const sco_api* BuildApi(const HostInfo& info);

constexpr size_t kMaxPlugins = 256;   // handles for the life of the process (reloads count)
constexpr size_t kMaxIdLen = 31;

// A new handle for plugin `id` ([a-z0-9_], 1-31 characters, not a reserved prefix: sco, host,
// menu, game). Handles are never freed or reused, so a released one stays refused. nullptr for
// a bad or reserved id, an id held by a handle not yet released, or kMaxPlugins handed out.
// Any thread.
sco_plugin* NewPlugin(const char* id);

// The id a handle was made with; nullptr for a pointer NewPlugin didn't return.
const char* PluginId(const sco_plugin* p);

// ---- host-owned services ----------------------------------------------------------------------
//
// Services the host itself publishes (docs/design/vfs-datacore.md decision 10): they live under
// the reserved plugin id "sco", so their names are "sco.<name>" ("sco.storage"), which no plugin
// can take (NewPlugin and plugin.ini refuse the id, discovery refuses a folder named sco).
// Plugins find them with the ordinary query_service; sco_api.h is unchanged. The owner is the
// host's own token (HostOwner()), never released, so a service withdrawn at shutdown can be
// published again by the next start. Host services outlive every plugin: sco::app::Stop
// withdraws them after UnloadAll. Any thread.

constexpr const char* kHostId = "sco";

// The runtime owner of every host-owned service. Never released.
const void* HostOwner();

// Publishes table under name, which must be "sco.<name>" (the service name rule of
// sco/runtime.h: [a-z0-9_.], at most 63 characters). BadArg: a null table, a name outside
// "sco.", a bad or taken name. TooMany: out of memory.
Result ProvideHostService(const char* name, uint32_t version, const void* table);

// Withdraws one host service. NotFound: the host publishes nothing by that name.
Result WithdrawHostService(const char* name);

// Withdraws every host service (host shutdown); returns how many.
size_t WithdrawHostServices();

}  // namespace sco::host
