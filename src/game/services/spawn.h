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

// For game.actors (actors.cpp): spawns entityClass at pos in zone zoneId's local frame through
// the spawner spawn.entities uses, facing rot (x y z w, normalised; nullptr = the zone's axes).
// nullptr with id set on success, else the reason ("unknown entity class", "the spawner isn't
// available on this game build", ...). Game thread.
const char* SpawnEntityInZone(const char* entityClass, uint64_t zoneId, const double pos[3], uint64_t& id,
                              const double rot[4] = nullptr);

// For game.entities (entities.cpp): moves and turns entity id to pos and rot (x y z w, normalised)
// given in zone zoneId's frame, through the same mover as spawn.entities' set_entity_transform,
// but without its ownership rules (the caller applies its own). Game thread.
enum class MoveResult : uint8_t { Ok, NotStreamedIn, NoZone, ZoneConversion, Refused };
MoveResult MoveEntity(uint64_t id, uint64_t zoneId, const double pos[3], const double rot[4]);

// True while spawn.cpp's release hook is added: it also releases game.actors' NPCs
// (ReleaseActorsOwner), so game.actors spawns nothing without it.
bool SpawnReleaseHooked();

// For game.vehicles' ownership rules. Game thread. SpawnedBy: owner spawned id through spawn_as
// and is still loaded (false without the release hook, as the mover). IsPlayerVehicle: the product
// registered id (RegisterPlayerVehicle).
bool SpawnedBy(const void* owner, uint64_t id);
bool IsPlayerVehicle(uint64_t id);

}  // namespace sco::game::services
