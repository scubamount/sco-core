/* sc_world.h: the game.world service: ray casts into the game's physics and the camera the player
 * sees through. This header ships with the sco SDK; the Star Citizen game pack publishes the
 * service under the owner "game" (sco::host::ProvideGameService, when the product sets
 * Platform::gameServices; docs/game-services.md). The sc_ prefix marks a game or product service:
 * sco_ is reserved for host services. To use it:
 *
 *   const sc_world_v1* world = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SC_WORLD_NAME, SC_WORLD_VERSION_1_0, (const void**)&world) == SCO_OK) { ... }
 *
 * The version is the query_service argument, never a field: query_service answers SCO_UNAVAILABLE
 * when the published table is older than the version asked for. A later 1.x minor only appends
 * functions; check size before calling one: world->size > offsetof(sc_world_v1, <function>).
 *
 * Rules (docs/design/game-services.md):
 *  - Game thread only (a command, a tick or a run_on_game_thread task). From another thread every
 *    function except last_error answers SCO_WRONG_THREAD without touching the game.
 *  - Both functions are read-only queries: they take no plugin handle and change nothing in the
 *    game.
 *  - Positions are metres as doubles. raycast works in a zone's local frame, like teleport.spatial
 *    (sc_spatial.h); camera answers in the world frame plus the zone you're in, so you can hand it
 *    to teleport.spatial's world_to_local. Zone ids are volatile streaming handles: never keep one
 *    past the moment you got it.
 *  - Nothing is allocated across the boundary; text comes back through caller buffers.
 *  - Every failure leaves a reason for last_error.
 *  - Capabilities (sco_api has()): "game.world.raycast" and "game.world.camera", each ready when
 *    its signature rows are OK on this game build. A function whose capability isn't ready answers
 *    SCO_UNAVAILABLE.
 *  - Not here: the camera's field of view. The game computes it per view; nothing pins which value
 *    is the one on screen (docs/design/game-world-spikes.md, B2), so there is no fov, not even a
 *    guess. The struct and table gain one when it is pinned.
 */
#ifndef SC_WORLD_H
#define SC_WORLD_H

#include "sco_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC_WORLD_NAME        "game.world"
#define SC_WORLD_VERSION_1_0 0x00010000u

/* The longest ray, metres. A longer max_dist is clamped to this, not refused. */
#define SC_WORLD_RAY_MAX_DISTANCE 20000

/* Which optional sc_world_hit fields the game pack filled. 4 bytes. 1.0 never sets either: the
 * game's hit record doesn't pin where the hit entity or the surface normal are (spike B1), so
 * entity_id and normal stay zero until it does. */
typedef enum sc_world_hit_flag {
    SC_WORLD_HIT_ENTITY = 0x1, /* entity_id is the entity that was hit */
    SC_WORLD_HIT_NORMAL = 0x2, /* normal is the surface normal there */
    SC_WORLD_HIT_FLAG_FORCE32 = 0x7fffffff
} sc_world_hit_flag;

typedef struct sc_world_hit {
    uint32_t size;     /* sizeof(sc_world_hit): the caller sets it, raycast refuses a smaller one */
    uint32_t flags;    /* sc_world_hit_flag bits: which of the fields below were filled */
    uint64_t entity_id; /* the entity hit (SC_WORLD_HIT_ENTITY), else 0 */
    double   pos[3];   /* where the ray hit, in the frame of the zone you cast in */
    double   normal[3]; /* the surface normal (SC_WORLD_HIT_NORMAL), else 0 */
    double   distance; /* from the ray's origin to pos, metres */
} sc_world_hit;

typedef struct sc_world_v1 {
    uint32_t size; /* sizeof(sc_world_v1) as the game pack built it */
    uint32_t _pad;
    /* Casts a ray from from (metres, in zone zone_id's local frame) along dir (any non-zero
     * length; it is normalised) for at most max_dist metres, and fills *out_hit (out_hit->size
     * must be sizeof(sc_world_hit)). max_dist must be positive and finite; one over
     * SC_WORLD_RAY_MAX_DISTANCE is clamped to it. The cast tries the zone and then up to two of
     * its parents (never the world's root zone), as sc-offline's build mode does, and reports the
     * first hit, in the frame of zone_id. Nothing is skipped: your own player's body can be hit.
     * SCO_OK: hit. SCO_NOT_FOUND: nothing hit within max_dist (last_error says "no hit"), or the
     * zone isn't streamed in, or you haven't spawned. SCO_BAD_ARG: a NULL pointer, zone 0, a
     * non-finite or zero direction, a bad max_dist or a too-small out_hit->size. SCO_UNAVAILABLE:
     * game.world.raycast isn't ready. SCO_FAILED: the game faulted. *out_hit is zeroed (apart
     * from size) on every failure. */
    sco_result (*raycast)(uint64_t zone_id, const double from[3], const double dir[3], double max_dist,
                          sc_world_hit* out_hit);
    /* The camera you see through: *out_pos in the world frame (metres), *out_rot_xyzw the same
     * frame's rotation (x, y, z, w), *out_zone_id the zone your player is in (so you can convert
     * with teleport.spatial). No fov (see above). Any pointer may be NULL to skip it, not all.
     * SCO_NOT_FOUND: you haven't spawned yet. SCO_UNAVAILABLE: game.world.camera isn't ready.
     * SCO_FAILED: the game faulted. Outputs are zeroed on failure. */
    sco_result (*camera)(double out_pos[3], double out_rot_xyzw[4], uint64_t* out_zone_id);
    /* The reason for the last failed call of any plugin ("" if none), NUL-terminated, with the
     * size handshake (*inout_size: capacity in; bytes written, or needed with SCO_TOO_MANY, out).
     * The queries keep no per-plugin state, so self is only checked: NULL or a loaded plugin's
     * handle (anything else is SCO_BAD_ARG). Any thread. */
    sco_result (*last_error)(sco_plugin* self, char* out, uint32_t* inout_size);
} sc_world_v1;

#ifdef __cplusplus
}
#endif

#endif
