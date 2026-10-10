/* sc_actors.h: the game.actors service: your player, and NPCs a plugin spawns and despawns. This
 * header ships with the sco SDK; the Star Citizen game pack publishes the service under the owner
 * "game" (sco::host::ProvideGameService, when the product sets Platform::gameServices;
 * docs/game-services.md). The sc_ prefix marks a game or product service: sco_ is reserved for
 * host services. To use it:
 *
 *   const sc_actors_v1* actors = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SC_ACTORS_NAME, SC_ACTORS_VERSION_1_0, (const void**)&actors) == SCO_OK) { ... }
 *
 * The version is the query_service argument, never a field: query_service answers SCO_UNAVAILABLE
 * when the published table is older than the version asked for. A later 1.x minor only appends
 * functions; check size before calling one: actors->size > offsetof(sc_actors_v1, <function>).
 *
 * Rules (docs/design/game-services.md):
 *  - Game thread only (a command, a tick or a run_on_game_thread task). From another thread every
 *    function except last_error answers SCO_WRONG_THREAD without touching the game.
 *  - Functions that change the game take your plugin's handle (self, as sco_plugin_load received
 *    it) first. A handle that is unknown or already released is SCO_BAD_ARG.
 *  - Ownership: an NPC you spawn here is yours. Only you may despawn it, and the host despawns
 *    every NPC you spawned here, and still own, when your plugin unloads or crashes. Nothing you
 *    spawned outlives your plugin.
 *  - Ids are the game's 64-bit entity ids: session handles that name something only while it is
 *    streamed in, never keys to store. Store the archetype's class name instead.
 *  - Nothing is allocated across the boundary; text comes back through caller buffers.
 *  - Every failure leaves a reason for last_error.
 *  - Capabilities (sco_api has()): "game.actors.local_player", "game.actors.spawn_npc" and
 *    "game.actors.despawn", each ready when its signature rows are OK on this game build. A
 *    function whose capability isn't ready answers SCO_UNAVAILABLE.
 *
 * Positions are teleport.spatial's (sc_spatial.h): metres as doubles in a zone's local frame;
 * zone ids are volatile streaming handles (player_pose, zone_of_entity): never keep one past the
 * moment you got it. */
#ifndef SC_ACTORS_H
#define SC_ACTORS_H

#include "sco_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC_ACTORS_NAME        "game.actors"
#define SC_ACTORS_VERSION_1_0 0x00010000u

/* NPCs spawned through game.actors and not yet despawned, all plugins together. */
#define SC_ACTORS_MAX_NPCS 1024u

typedef struct sc_actors_v1 {
    uint32_t size; /* sizeof(sc_actors_v1) as the game pack built it */
    uint32_t _pad;
    /* Your player: *out_actor_id, the id the game's client player record names your actor by
     * (the id the game resolves to your actor), and *out_entity_id, your entity's id (the same as
     * spawn.entities' local_player_id; use it with teleport.spatial and spawn.entities). Both 0
     * on failure. Either pointer may be NULL to skip it, not both. SCO_NOT_FOUND: you haven't
     * spawned yet. Read-only: no self; a failure's reason is last_error(NULL, ...). */
    sco_result (*local_player)(uint64_t* out_actor_id, uint64_t* out_entity_id);
    /* Spawns an NPC of archetype_class (an entity class name, as in sc-offline's npcs.txt) at pos
     * in zone zone_id's local frame, facing the zone's axes. *out_id = the new entity id, 0 on
     * failure. The id is final at once; the NPC streams in a few seconds later. The NPC is yours
     * (see Ownership above). SCO_BAD_ARG: a null or released self, a null class, pos or out_id,
     * zone 0, or a position that isn't finite. SCO_NOT_FOUND: the zone isn't streamed in, or the
     * class doesn't exist on this game build. SCO_TOO_MANY: SC_ACTORS_MAX_NPCS are alive.
     * SCO_FAILED: the game refused or faulted. */
    sco_result (*spawn_npc)(sco_plugin* self, const char* archetype_class, uint64_t zone_id, const double pos[3],
                            uint64_t* out_id);
    /* Removes an NPC you spawned with spawn_npc. SCO_OK starts the removal: the NPC leaves the
     * world within a few seconds (one that hasn't streamed in yet goes once it has), and the id
     * is no longer yours. SCO_NOT_FOUND: not an NPC you spawned here, or despawned already. */
    sco_result (*despawn)(sco_plugin* self, uint64_t entity_id);
    /* The reason for self's last failed call ("" if none), NUL-terminated, with the size
     * handshake (*inout_size: capacity in; bytes written, or needed with SCO_TOO_MANY, out).
     * self NULL: the last failure of a call made without a plugin handle (local_player). Any
     * thread. */
    sco_result (*last_error)(sco_plugin* self, char* out, uint32_t* inout_size);
} sc_actors_v1;

#ifdef __cplusplus
}
#endif

#endif
