/*
 * abi_game_events.c: pins the layout of sc_game_events.h (the game.* bus event payloads of the game pack).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the event payloads changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * a payload struct; existing lines never change. sdk/csharp/Sco.Sdk.Tests checks its C# copy against
 * the SIZE / AT / PIN lines below.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc.
 */
#include <stddef.h>
#include <stdint.h>

#include "sco_api.h"
#include "sc_game_events.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- event names ---- */
PIN(sizeof(SC_GAME_EVENT_PLAYER_SPAWNED) == 20);  /* "game.player.spawned" */
PIN(sizeof(SC_GAME_EVENT_PLAYER_DIED) == 17);     /* "game.player.died" */
PIN(sizeof(SC_GAME_EVENT_ZONE_CHANGED) == 18);    /* "game.zone.changed" */
PIN(sizeof(SC_GAME_EVENT_VEHICLE_BOARDED) == 21); /* "game.vehicle.boarded" */
PIN(sizeof(SC_GAME_EVENT_VEHICLE_EXITED) == 20);  /* "game.vehicle.exited" */

/* ---- sc_game_player_spawned ---- */
SIZE(sc_game_player_spawned, 24);
AT(sc_game_player_spawned, size, 0);
AT(sc_game_player_spawned, _pad, 4);
AT(sc_game_player_spawned, entity_id, 8);
AT(sc_game_player_spawned, zone_id, 16);

/* ---- sc_game_player_died ---- */
SIZE(sc_game_player_died, 24);
AT(sc_game_player_died, size, 0);
AT(sc_game_player_died, _pad, 4);
AT(sc_game_player_died, entity_id, 8);
AT(sc_game_player_died, killer_id, 16);

/* ---- sc_game_zone_changed ---- */
SIZE(sc_game_zone_changed, 24);
AT(sc_game_zone_changed, size, 0);
AT(sc_game_zone_changed, _pad, 4);
AT(sc_game_zone_changed, old_zone_id, 8);
AT(sc_game_zone_changed, new_zone_id, 16);

/* ---- sc_game_vehicle_seat (boarded and exited, reserved) ---- */
SIZE(sc_game_vehicle_seat, 16);
AT(sc_game_vehicle_seat, size, 0);
AT(sc_game_vehicle_seat, seat_index, 4);
AT(sc_game_vehicle_seat, vehicle_entity_id, 8);

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_game_events_pins(void);
void sco_abi_game_events_pins(void) {}
