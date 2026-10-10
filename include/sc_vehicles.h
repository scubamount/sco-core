/* sc_vehicles.h: the game.vehicles service: the ship you're aboard, a ship's seats and who sits in
 * them, seating and unseating actors, and powering a ship on (Flight Ready). This header ships with
 * the sco SDK; the Star Citizen game pack publishes the service under the owner "game"
 * (sco::host::ProvideGameService, when the product sets Platform::gameServices;
 * docs/game-services.md). To use it:
 *
 *   const sc_vehicles_v1* veh = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SC_VEHICLES_SERVICE_NAME, SC_VEHICLES_SERVICE_VERSION,
 *                          (const void**)&veh) == SCO_OK && api->has("game.vehicles.seats")) { ... }
 *
 * Capabilities, one per system, so a game patch that breaks one leaves the others working:
 *   game.vehicles.seats        player_ship, seats, seat_occupant
 *   game.vehicles.seat         seat, eject (and SC_SEAT_USABLE_KNOWN on every seat)
 *   game.vehicles.flight_ready power_on
 * A function whose capability isn't ready answers SCO_UNAVAILABLE.
 *
 * Every function: game thread only (a command, a tick or a run_on_game_thread task); from another
 * thread it answers SCO_WRONG_THREAD without touching the game. Every failure leaves a reason for
 * last_error. Functions that change the game take your plugin's handle (self, as sco_plugin_load
 * received it) first; the game pack logs each of them with your plugin id in mod.log.
 *
 * Ids are the game's own 64-bit entity ids (as in sc_spawn.h and sc_spatial.h): session handles
 * that name something only while it's streamed in. Never store one; store class or seat names.
 * Seat indexes are positions in the list seats() returns; they stay the same while the ship stays
 * streamed in, and seat() and seat_occupant() read the list afresh on every call. */
#ifndef SC_VEHICLES_SERVICE_H
#define SC_VEHICLES_SERVICE_H

#include "sco_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC_VEHICLES_SERVICE_NAME    "game.vehicles"
#define SC_VEHICLES_SERVICE_VERSION 0x00010000u /* 1.0 */
#define SC_VEHICLE_SEAT_NAME_MAX    64          /* sc_vehicle_seat.name, NUL included */

/* sc_vehicle_seat.flags, or-ed. */
typedef enum sc_vehicle_seat_flag {
    /* The game's own seat picker accepts this seat: its owner entity has a live
     * IInteractableComponent. seat() refuses a seat without it (turret items and remote-operated
     * parts are listed as seats too, but the game never puts anyone in them). Set only together
     * with SC_SEAT_USABLE_KNOWN. */
    SC_SEAT_USABLE = 0x1,
    /* The game pack could check SC_SEAT_USABLE on this game build (its seat gate row is OK; it
     * always is when game.vehicles.seat is ready). Without it SC_SEAT_USABLE is never set:
     * "unknown", not "unusable". */
    SC_SEAT_USABLE_KNOWN = 0x2,
    /* Someone or something is in it (the game's occupant field isn't 0). occupant_id is 0 when
     * the game pack can't tell who. */
    SC_SEAT_OCCUPIED = 0x4,
    /* The ship's highest-priority seat (the pilot's). */
    SC_SEAT_PILOT = 0x8,
    SC_SEAT_FLAG_FORCE32 = 0x7fffffff
} sc_vehicle_seat_flag;

/* One seat, as seats() writes it into your array. Fixed for all of 1.x. */
typedef struct sc_vehicle_seat {
    uint32_t index;        /* its position in the list: the seat_index of seat() and seat_occupant() */
    uint32_t flags;        /* sc_vehicle_seat_flag bits */
    uint64_t seat_id;      /* the seat item's entity id */
    uint64_t occupant_id;  /* the actor in it, or 0 (empty, or occupied by someone unknown) */
    uint32_t priority;     /* the game's seat priority: the pilot's seat is the highest (1000 or more) */
    uint32_t _pad;
    char name[SC_VEHICLE_SEAT_NAME_MAX]; /* the seat item's entity name, NUL-terminated, cut to fit */
} sc_vehicle_seat;

typedef struct sc_vehicles_v1 {
    uint32_t size; /* sizeof(sc_vehicles_v1) as the game pack built it */
    uint32_t _pad;
    /* The ship you're aboard (standing or seated in it). SCO_OK and the id; SCO_NOT_FOUND when
     * you're in no ship; SCO_UNAVAILABLE before you've spawned or without the capability.
     * game.vehicles.seats. */
    sco_result (*player_ship)(uint64_t* out_ship);
    /* Fills out[0..max) with ship's seats in the game's order and sets *out_count to how many
     * were written and *out_more to 1 when the ship has more than that (else 0). out may be NULL
     * when max is 0 (then *out_more says whether the ship has any). SCO_NOT_FOUND: ship isn't
     * streamed in or isn't a vehicle. game.vehicles.seats. */
    sco_result (*seats)(uint64_t ship, sc_vehicle_seat* out, uint32_t max, uint32_t* out_count,
                        uint32_t* out_more);
    /* *out_actor = the actor in seat seat_index of ship, or 0 when the seat is empty. SCO_FAILED
     * when the seat is occupied but the game pack can't tell by whom (last_error has the raw
     * field); SCO_NOT_FOUND for a ship that isn't streamed in or an index past the last seat.
     * game.vehicles.seats. */
    sco_result (*seat_occupant)(uint64_t ship, uint32_t seat_index, uint64_t* out_actor);
    /* Puts actor into seat seat_index of ship, moving it out of any seat it's in first. You may
     * seat your player's own actor, or an actor your plugin spawned through spawn.entities'
     * spawn_as or game.actors' spawn_npc (while your plugin is loaded); anything else is SCO_BAD_ARG. SCO_FAILED: the seat
     * isn't usable (no SC_SEAT_USABLE), someone else is in it (nobody is evicted), the actor
     * isn't streamed in yet or has no seat link, or the game faulted. SCO_OK means the game
     * accepted the link; it takes effect within a second or two: check seat_occupant() on a
     * later tick, and call seat() again if it didn't take. game.vehicles.seat. */
    sco_result (*seat)(sco_plugin* self, uint64_t actor, uint64_t ship, uint32_t seat_index);
    /* Takes actor out of the seat it's in (same rule on whose actor as seat()). SCO_FAILED when
     * it isn't seated. game.vehicles.seat. */
    sco_result (*eject)(sco_plugin* self, uint64_t actor);
    /* Sends the game's Flight Ready event to ship's pilot dashboard, as pressing R in the pilot
     * seat does. You may power on the ship you're aboard, your player's own registered vehicles
     * and ships your plugin spawned through spawn_as; anything else is SCO_BAD_ARG. SCO_FAILED
     * while the ship has no dashboard streamed in yet (a big ship takes up to 20 s): try again
     * on a later tick. game.vehicles.flight_ready. */
    sco_result (*power_on)(sco_plugin* self, uint64_t ship);
    /* The reason for the last failed call ("" if none), NUL-terminated, with sco.storage's size
     * handshake: *inout_size is out's capacity on entry and the bytes written (NUL included) on
     * SCO_OK, or the bytes needed on SCO_TOO_MANY. out may be NULL when *inout_size is 0.
     * On the game thread: the newer of your own last failed call (functions with self) and the
     * last failed query (functions without self, by any plugin); ask right after the call.
     * On another thread: the last refusal on that thread. Any thread. */
    sco_result (*last_error)(sco_plugin* self, char* out, uint32_t* inout_size);
} sc_vehicles_v1;

#ifdef __cplusplus
}
#endif

#endif
