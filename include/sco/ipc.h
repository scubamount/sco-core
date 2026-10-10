#pragma once
// sco::ipc: the host side of the "sco.ipc" service (include/sco_ipc.h): named shared-memory
// channels between a plugin and another program on the same PC, laid out with the MIT wire
// include/sc_ipc.h. C++ and internal: the host kit starts it; plugins only see the C table.
//
//   sco::ipc::Start();                      // publishes sco.ipc 1.0 (a host-owned service)
//   ... plugins query_service("sco.ipc", 0x00010000, ...) ...
//   sco::ipc::Tick();                       // every host tick: the owner heartbeat of each channel
//   sco::ipc::Stop();                       // after every plugin unloaded: withdraws, closes all
//
// A channel is the mapping <prefix><plugin id>.<name> (Local\SCO_ on Windows, created with a DACL
// for the current user only; /SCO_ through shm_open elsewhere, mode 0600, for the tests). The
// platform half is src/ipc/shm_win.cpp / shm_posix.cpp; everything else, the ring and block
// logic included, is sc_ipc.h over plain memory. Release(owner) (a runtime release hook) marks
// each of the plugin's channels closed and unmaps it. Reference: docs/ipc.md.
#include "sco_ipc.h"
#include "sco/runtime.h"
#include <string>

namespace sco::ipc {

#ifdef _WIN32
inline constexpr const char* kPrefix = "Local\\SCO_";
#else
inline constexpr const char* kPrefix = "/SCO_";
#endif

struct Options {
    // Mapping names start with this. It must start with kPrefix; a test adds a unique tail
    // ("Local\\SCO_t1234_") so runs never meet. Products leave it alone.
    std::string prefix = kPrefix;
};

// Publishes "sco.ipc" 1.0 under the host's id and installs the release hook. Any thread.
// BadArg: a prefix that doesn't start with kPrefix (or is too long), already started, or the name
// already published. TooMany: no release hook slot or out of memory. Nothing changes unless Ok.
Result Start(const Options& opts = Options());

// Withdraws "sco.ipc" and closes every channel; later calls through the table answer
// SCO_UNAVAILABLE. No-op unless started. Call after every plugin has unloaded. Any thread.
void Stop();

bool Started();

// Writes the owner heartbeat of every open channel (the shared monotonic clock: GetTickCount64
// on Windows). The host kit calls it from Tick. Any thread.
void Tick();

// The service table (what query_service hands out). Valid for the life of the process.
const sco_ipc_v1* Table();

// <prefix><pluginId>.<channel> for the current Start; empty when not started or either part is
// not a valid id / channel name.
std::string MappingName(const char* pluginId, const char* channel);

}  // namespace sco::ipc
