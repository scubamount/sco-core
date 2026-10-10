#pragma once
// game.actors inside the game pack: started and stopped by services::Start / Stop (spatial.cpp),
// after spawn.entities, whose spawner and release hook it uses. Game thread.
#include "sco/runtime.h"
#include <cstdint>

namespace sco::game::services {

// Publishes game.actors 1.0, sets its capabilities and subscribes its tick (the removal checks).
// BadArg when the name is taken; nothing is left published then.
Result StartActors();
// Starts the removal of every NPC still owned (a plugin whose release failed), then withdraws.
void   StopActors();

// spawn.cpp's release hook: owner unloaded or crashed, so every NPC it spawned through
// game.actors leaves the world, and its last error is forgotten. Game thread.
void ReleaseActorsOwner(const void* owner);

// For game.vehicles' ownership rule: owner spawned NPC id through game.actors' spawn_npc and still
// owns it (not despawned, owner not released). Game thread.
bool NpcOwnedBy(const void* owner, uint64_t id);

}  // namespace sco::game::services
