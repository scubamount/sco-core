#pragma once
// game.vehicles inside the game pack: started and stopped by services::Start / Stop (spatial.cpp),
// after spawn.entities, whose spawn_as records decide which actors a plugin may seat. Game thread.
#include "sco/runtime.h"

namespace sco::game::services {

// Sets the game.vehicles.* capabilities from the rows and publishes game.vehicles 1.0. BadArg when
// the name is taken; nothing is left published then.
Result StartVehicles();
void   StopVehicles();

}  // namespace sco::game::services
