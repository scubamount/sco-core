#pragma once
// spawn.entities inside the game pack: started and stopped by services::Start / Stop (spatial.cpp),
// after teleport.spatial, whose zone tree its mover converts through. Game thread.
#include "sco/runtime.h"
#include <cstdint>

namespace sco::game::services {

// Publishes spawn.entities 1.2 and adds its release hook. BadArg when the name is taken; nothing
// is left published then.
Result StartSpawn();
void   StopSpawn();

// For game.vehicles' ownership rules. Game thread. SpawnedBy: owner spawned id through spawn_as
// and is still loaded (false without the release hook, as the mover). IsPlayerVehicle: the product
// registered id (RegisterPlayerVehicle).
bool SpawnedBy(const void* owner, uint64_t id);
bool IsPlayerVehicle(uint64_t id);

}  // namespace sco::game::services
