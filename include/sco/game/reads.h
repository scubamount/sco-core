#pragma once
// Game reads behind the game services (docs/game-services.md): the local player, zones and
// entity poses, read through the teleport.* rows (sco/game/teleport.h). Moved from sc-offline's
// teleport.cpp and build.cpp (GetLocalPlayer, ZoneId, ZoneFromId, ReadZoneChain, EntityRotation,
// MoveEntityLocal),
// same offsets. Game pack, Windows only (sco_game_services).
//
// Game thread. Every function reads game memory, so callers run them under __try, as the
// services do; Init() and Ready() don't.
#include <cstddef>
#include <cstdint>

namespace sco::game::reads {

// Takes the teleport.* addresses from the resolved rows. False (and Ready() false) when any
// teleport.* row isn't OK. Call after sco::ResolveAll.
bool Init();
bool Ready();

// Your actor and its entity; false before you've spawned.
bool LocalPlayer(uintptr_t& actor, uintptr_t& entity);
// The same, plus the id the game's client player record names your actor by (the id
// teleport.handle_from_id turns into the actor): game.actors' local_player.
bool LocalPlayer(uintptr_t& actor, uintptr_t& entity, uint64_t& actorId);

uintptr_t   ZoneParent(uintptr_t zone);
const char* ZoneName(uintptr_t zone);
uint64_t    ZoneId(uintptr_t zone);
uintptr_t   ZoneFromId(uint64_t zoneId);       // 0 once the zone has streamed out
uintptr_t   EntityFromId(uint64_t entityId);   // 0 once the entity has streamed out
uintptr_t   EntityZone(uintptr_t entity);
void        EntityLocalPos(uintptr_t entity, double out[3]);   // in EntityZone's frame
// x, y, z, w. False when the entity's move/rotate functions aren't the ones checked in 4.10.196
// (the check runs once, on the first entity asked about).
bool        EntityRotation(uintptr_t entity, double rot[4]);
// Moves (and with rot, turns) an entity within its zone: pos and rot (x y z w) in EntityZone's
// frame, through the same entity slots EntityRotation checks (moved from sc-offline's build.cpp
// MoveEntityLocal). False, and nothing moves, when those slots aren't the checked ones.
bool        SetEntityLocalPose(uintptr_t entity, const double pos[3], const double rot[4]);
void        LocalToWorld(uintptr_t zone, const double local[3], double world[3]);

// One zone as read on this call: its id, name and frame in the game's world, the origin in metres
// and axis[i] the world direction of its local axis i.
struct ZoneFrame { uint64_t id; char name[96]; double origin[3]; double axis[3][3]; };

// The zone and its ancestors, innermost first, skipping any whose id doesn't lead back to it or
// whose frame isn't orthonormal. Returns how many (0 to max).
int ReadZoneChain(uintptr_t zone, ZoneFrame* out, int max);

}  // namespace sco::game::reads
