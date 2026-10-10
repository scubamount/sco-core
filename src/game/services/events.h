#pragma once
// The game.* bus events inside the game pack: started and stopped by services::Start / Stop
// (spatial.cpp), after game.actors, whose state probe the death hook feeds. Game thread.
#include "sco/runtime.h"

namespace sco::game::services {

// Sets the game.events.* capabilities from the rows, installs the hooks whose rows are OK and
// subscribes the tick that publishes. Never fails on a missing row (that event's capability is
// off); a tick that can't be subscribed leaves nothing installed and returns the error.
Result StartEvents();
// Removes the hooks, unsubscribes the tick and turns the capabilities off.
void   StopEvents();

}  // namespace sco::game::services
