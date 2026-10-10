#pragma once
// game.entities inside the game pack: started and stopped by services::Start / Stop (spatial.cpp),
// after game.actors, whose removal path it uses to despawn, and after spawn.entities, whose
// spawner, mover and release hook it uses. Game thread.
#include "sco/runtime.h"
#include <cstdint>

namespace sco::game::services {

// Publishes game.entities 1.0, sets its capabilities and subscribes its tick (the watch events and
// the diagnostics). BadArg when the name is taken; nothing is left published then.
Result StartEntities();
// Removes every entity still owned (a plugin whose release failed), removes the stream hooks and
// withdraws.
void   StopEntities();

// spawn.cpp's release hook: owner unloaded or crashed, so every entity it spawned through
// game.entities leaves the world (decision 6), its watches end and its last error is forgotten.
// Game thread; may run inside one of the owner's own watch callbacks.
void ReleaseEntitiesOwner(const void* owner);

}  // namespace sco::game::services
