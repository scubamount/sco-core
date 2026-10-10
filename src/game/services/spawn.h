#pragma once
// spawn.entities inside the game pack: started and stopped by services::Start / Stop (spatial.cpp),
// after teleport.spatial, whose zone tree its mover converts through. Game thread.
#include "sco/runtime.h"

namespace sco::game::services {

// Publishes spawn.entities 1.2 and adds its release hook. BadArg when the name is taken; nothing
// is left published then.
Result StartSpawn();
void   StopSpawn();

}  // namespace sco::game::services
