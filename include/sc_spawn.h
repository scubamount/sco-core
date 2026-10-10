/* sc_spawn.h: sc-offline's spawn.entities service: spawn entities near you, look up your entity
 * and ship ids, and move the entities you spawned. This header ships with the sco SDK; the
 * service is provided by sc-offline's built-in "spawn" plugin, which publishes it through sco_api
 * 1.1 (provide_service). The sc_ prefix marks a product's service: sco_ is reserved for host
 * services. To use it:
 *
 *   const sc_spawn_service_v1* spawn = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION,
 *                          (const void**)&spawn) == SCO_OK) { ... }
 *
 * query_service answers SCO_UNAVAILABLE when the published table is older than the version asked
 * for. To also run on an older sc-offline, ask for the oldest minor you need (0x00010000u) and
 * check size before calling a function a later minor added:
 * spawn->size > offsetof(sc_spawn_service_v1, <function>).
 *
 * Every function: game thread only (a command, a tick or a run_on_game_thread task); called from
 * another thread they do nothing (spawn_near_player and spawn_as answer "game thread only"). The
 * built-in loads before every plugin and unloads after them, so the table stays valid for as long
 * as your plugin is loaded. Nothing here works when the spawn.ship capability is missing
 * (has("spawn.ship") == 0).
 *
 * Frames, units and ids are teleport.spatial's (sc_spatial.h): metres as doubles, rotations as
 * unit quaternions in the game's (x, y, z, w) order, zone and entity ids the game's own 64-bit ids,
 * opaque, naming something only while it's streamed in. Zone ids are volatile streaming handles:
 * never keep one past the moment you got it. */
#ifndef SC_SPAWN_SERVICE_H
#define SC_SPAWN_SERVICE_H

#include "sco_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC_SPAWN_SERVICE_NAME    "spawn.entities"
#define SC_SPAWN_SERVICE_VERSION 0x00010002u /* 1.2: set_entity_transform, spawn_as */

typedef struct sc_spawn_service_v1 {
    uint32_t size; /* sizeof(sc_spawn_service_v1) as sc-offline built it */
    /* Spawns an entity class (a ship, a vehicle, an item) at offset metres from you, in your
     * current zone's frame. NULL on success with *out_id = the new entity id; else the reason.
     * The id is final at once, but the entity streams in later: a few seconds, up to a minute for
     * a big ship. entity_alive(id) answers 1 once it has.
     * An entity spawned here belongs to no plugin: set_entity_transform refuses it. Use
     * spawn_as for an entity you will move. */
    const char* (*spawn_near_player)(const char* entity_class, const double offset[3], uint64_t* out_id);
    /* 1 if entity_class is a spawnable class on this game build, else 0. */
    int (*class_exists)(const char* entity_class);
    /* Your entity id, or 0 before you've spawned. */
    uint64_t (*local_player_id)(void);
    /* The ship you're aboard, or 0. */
    uint64_t (*player_ship_id)(void);
    /* 1.1. 1 while the entity id resolves in the game (spawned and streamed in), else 0; 0 for
     * id 0. Asked afresh on every call. A 1.0 table ends before this field: call it only when
     * size > offsetof(sc_spawn_service_v1, entity_alive). */
    int (*entity_alive)(uint64_t entity_id);
    /* 1.2. Moves and turns an entity: pos (metres) and rot (a unit quaternion, x y z w, the order
     * of sc_spatial.h's player_pose) in zone zone_id's frame. zone_id 0 is the world (root)
     * frame; any other id is that zone's local frame (ids from teleport.spatial: player_pose,
     * zone_of_entity). The entity stays in the zone it's in; the pose is converted to that zone's
     * frame. 1 on success, 0 on failure: wrong thread, a null pointer, rot not finite or of zero
     * length (it is normalized), an id that isn't streamed in, a zone the built-in can't place,
     * or an entity you may not move; sc-offline logs the reason to mod.log once per id and
     * reason. A fresh spawn takes seconds to stream in and answers 0 until then: check
     * entity_alive(id) first, and try again on a later tick while it answers 0. You may move:
     *  - an entity spawned through spawn_as with your own self, while your plugin is loaded
     *    (unloading forgets them; spawn_near_player and the spawn.ship command count for nobody);
     *  - your player's own vehicle once sc-offline has registered it as retrieved or delivered
     *    by ATC (no build registers one yet).
     * self is your plugin's handle, as sco_plugin_load received it. A 1.1 table ends before this
     * field: call it only when size > offsetof(sc_spawn_service_v1, set_entity_transform). */
    int (*set_entity_transform)(sco_plugin* self, uint64_t entity_id, uint64_t zone_id, const double pos[3],
                                const double rot[4]);
    /* 1.2. spawn_near_player, recorded as yours (self, as sco_plugin_load received it), so
     * set_entity_transform lets you move the entity. Same answers as spawn_near_player, plus
     * "bad argument" for a null self. Same size check as set_entity_transform:
     * size > offsetof(sc_spawn_service_v1, spawn_as). */
    const char* (*spawn_as)(sco_plugin* self, const char* entity_class, const double offset[3], uint64_t* out_id);
} sc_spawn_service_v1;

#ifdef __cplusplus
}
#endif

#endif
