#pragma once
// Addresses behind teleport, player lookup and zones. Resolved by sco::ResolveAll().
//
//   teleport.to_camera      CmdTeleportToCamera's handler, found from its console-command name,
//                           with 19 layout checks plus the entity-position and zone vtable checks
//   teleport.client_mgr     global pointer read at to_camera+0x24  (gGame in sdk_dumper's catalog)
//   teleport.handle_from_id entity handle from id, called at to_camera+0x9F
//   teleport.entity_system  global pointer read at to_camera+0x2B0 (gEnv_pEntitySystem)
#include <cstdint>

namespace sco::game {

struct TeleportAddrs {
    uintptr_t* clientMgr    = nullptr;
    uintptr_t* entitySystem = nullptr;
    void*      handleFromId = nullptr;
};

// All three, or false (and the struct untouched) when any teleport.* row isn't OK.
bool TeleportAddresses(TeleportAddrs& out);

}  // namespace sco::game
