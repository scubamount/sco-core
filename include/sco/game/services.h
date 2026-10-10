#pragma once
// The Star Citizen game pack's services (docs/game-services.md), published under the owner
// "game" with host::ProvideGameService. sco::app::Start calls Start when the product sets
// Platform::gameServices and the game pack's services are built (SCO_GAME_SC, Windows: the
// sco_game_services library, which defines SCO_GAME_SERVICES); Stop calls Stop after UnloadAll.
//
// Today: teleport.spatial 1.0 (sc_spatial.h) and spawn.entities 1.2 (sc_spawn.h). teleport.spatial
// answers 0 until the teleport.* rows are resolved (the product's ResolveAll before Start) and
// you've spawned; spawn.entities answers "the spawner isn't available on this game build" (and 0)
// unless the spawn.helpers rows (sco/game/actors.h) were OK at Start too.
#include "sco/runtime.h"
#include <cstdint>

namespace sco::game::services {

// Game thread. Publishes the services and subscribes their tick. BadArg when a name is taken
// (a product still provides teleport.spatial or spawn.entities itself); nothing is left published
// then.
Result Start();
void   Stop();
bool   Started();

// For the game pack's own code (spawn.entities' mover): the same frames as teleport.spatial.
// Game thread. 0 / false when the entity or a zone isn't streamed in.
uint64_t ZoneOfEntity(uint64_t entityId);
bool     PoseToZone(uint64_t from, uint64_t to, const double pos[3], const double rotXyzw[4],
                    double outPos[3], double outRotXyzw[4]);

// The player's own vehicle, retrieved or delivered by ATC: once the product registers its id, any
// plugin may move it with spawn.entities' set_entity_transform. Game thread. Registering an id
// twice or unregistering one that isn't registered changes nothing; ids stay registered across
// Stop and Start (the product unregisters them).
void RegisterPlayerVehicle(uint64_t entityId);
void UnregisterPlayerVehicle(uint64_t entityId);

}  // namespace sco::game::services
