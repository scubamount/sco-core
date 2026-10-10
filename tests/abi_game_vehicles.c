/*
 * abi_game_vehicles.c: pins the layout of sc_vehicles.h (the game pack's service "game.vehicles", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sc_vehicles_v1; existing lines never change, and sc_vehicle_seat never changes in 1.x.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; CTest (abi_game_vehicles) as C11 and C++20, plus -fshort-enums where
 * the compiler has it. The C# SDK's layout test checks its lines against Sco.Sdk.Game.
 */
#include <stddef.h>
#include <stdint.h>

#include "sc_vehicles.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, limits ---- */
PIN(SC_VEHICLES_SERVICE_VERSION == 0x00010000u);
PIN(sizeof(SC_VEHICLES_SERVICE_NAME) == 14); /* "game.vehicles" */
PIN(SC_VEHICLE_SEAT_NAME_MAX == 64);

/* ---- sc_vehicle_seat_flag: 4 bytes, even with -fshort-enums ---- */
SIZE(sc_vehicle_seat_flag, 4);
PIN(SC_SEAT_USABLE == 0x1);
PIN(SC_SEAT_USABLE_KNOWN == 0x2);
PIN(SC_SEAT_OCCUPIED == 0x4);
PIN(SC_SEAT_PILOT == 0x8);
PIN(SC_SEAT_FLAG_FORCE32 == 0x7fffffff);

/* ---- sc_vehicle_seat ---- */
SIZE(sc_vehicle_seat, 96);
AT(sc_vehicle_seat, index, 0);
AT(sc_vehicle_seat, flags, 4);
AT(sc_vehicle_seat, seat_id, 8);
AT(sc_vehicle_seat, occupant_id, 16);
AT(sc_vehicle_seat, priority, 24);
AT(sc_vehicle_seat, _pad, 28);
AT(sc_vehicle_seat, name, 32);

/* ---- sc_vehicles_v1 ---- */
SIZE(sc_vehicles_v1, 64);
AT(sc_vehicles_v1, size, 0);
AT(sc_vehicles_v1, _pad, 4);
AT(sc_vehicles_v1, player_ship, 8);
AT(sc_vehicles_v1, seats, 16);
AT(sc_vehicles_v1, seat_occupant, 24);
AT(sc_vehicles_v1, seat, 32);
AT(sc_vehicles_v1, eject, 40);
AT(sc_vehicles_v1, power_on, 48);
AT(sc_vehicles_v1, last_error, 56);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_vehicles_signatures(const sc_vehicles_v1* v) {
    sco_result (*player_ship)(uint64_t*) = v->player_ship;
    sco_result (*seats)(uint64_t, sc_vehicle_seat*, uint32_t, uint32_t*, uint32_t*) = v->seats;
    sco_result (*seat_occupant)(uint64_t, uint32_t, uint64_t*) = v->seat_occupant;
    sco_result (*seat)(sco_plugin*, uint64_t, uint64_t, uint32_t) = v->seat;
    sco_result (*eject)(sco_plugin*, uint64_t) = v->eject;
    sco_result (*power_on)(sco_plugin*, uint64_t) = v->power_on;
    sco_result (*last_error)(sco_plugin*, char*, uint32_t*) = v->last_error;
    (void)player_ship; (void)seats; (void)seat_occupant; (void)seat; (void)eject; (void)power_on; (void)last_error;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_game_vehicles_pins(void);
void sco_abi_game_vehicles_pins(void) { (void)&pin_vehicles_signatures; }
