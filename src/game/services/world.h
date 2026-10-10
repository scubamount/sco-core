#pragma once
// game.world inside the game pack: started and stopped by services::Start / Stop (spatial.cpp),
// after teleport.spatial, whose zone frames it casts through. Game thread.
#include "sco/runtime.h"

namespace sco::game::services {

// Publishes game.world 1.0 and sets its capabilities from the rows. BadArg when the name is
// taken; nothing is left published then.
Result StartWorld();
void   StopWorld();

}  // namespace sco::game::services
