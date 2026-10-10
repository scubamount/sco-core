#pragma once
// The Star Citizen game pack's services (docs/game-services.md), published under the owner
// "game" with host::ProvideGameService. sco::app::Start calls Start when the product sets
// Platform::gameServices and the game pack's services are built (SCO_GAME_SC, Windows: the
// sco_game_services library, which defines SCO_GAME_SERVICES); Stop calls Stop after UnloadAll.
//
// Today: teleport.spatial 1.0 (sc_spatial.h). Its functions answer 0 until the teleport.* rows
// are resolved (the product's ResolveAll before Start) and you've spawned.
#include "sco/runtime.h"
#include <cstdint>

namespace sco::game::services {

// Game thread. Publishes the services and subscribes their tick. BadArg when a name is taken
// (a product still provides teleport.spatial itself); nothing is left published then.
Result Start();
void   Stop();
bool   Started();

// For the game pack's own code (spawn.entities' mover): the same frames as teleport.spatial.
// Game thread. 0 / false when the entity or a zone isn't streamed in.
uint64_t ZoneOfEntity(uint64_t entityId);
bool     PoseToZone(uint64_t from, uint64_t to, const double pos[3], const double rotXyzw[4],
                    double outPos[3], double outRotXyzw[4]);

}  // namespace sco::game::services
