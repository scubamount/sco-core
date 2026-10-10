/*
 * abi_spawn.c: pins the layout of sc_spawn.h (sc-offline's service "spawn.entities", 1.2).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sc_spawn_service_v1; existing lines never change.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc.
 */
#include <stddef.h>
#include <stdint.h>

#include "sc_spawn.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version ---- */
PIN(SC_SPAWN_SERVICE_VERSION == 0x00010002u);
PIN(sizeof(SC_SPAWN_SERVICE_NAME) == 15); /* "spawn.entities" */

/* ---- sc_spawn_service_v1 ---- */
SIZE(sc_spawn_service_v1, 64);
AT(sc_spawn_service_v1, size, 0);
AT(sc_spawn_service_v1, spawn_near_player, 8);
AT(sc_spawn_service_v1, class_exists, 16);
AT(sc_spawn_service_v1, local_player_id, 24);
AT(sc_spawn_service_v1, player_ship_id, 32);
AT(sc_spawn_service_v1, entity_alive, 40);         /* 1.1: a 1.0 table is 40 bytes */
AT(sc_spawn_service_v1, set_entity_transform, 48); /* 1.2: a 1.1 table is 48 bytes */
AT(sc_spawn_service_v1, spawn_as, 56);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_spawn_signatures(const sc_spawn_service_v1* s) {
    const char* (*spawn_near_player)(const char*, const double*, uint64_t*) = s->spawn_near_player;
    int (*class_exists)(const char*) = s->class_exists;
    uint64_t (*local_player_id)(void) = s->local_player_id;
    uint64_t (*player_ship_id)(void) = s->player_ship_id;
    int (*entity_alive)(uint64_t) = s->entity_alive;
    int (*set_entity_transform)(sco_plugin*, uint64_t, uint64_t, const double*, const double*) = s->set_entity_transform;
    const char* (*spawn_as)(sco_plugin*, const char*, const double*, uint64_t*) = s->spawn_as;
    (void)spawn_near_player; (void)class_exists; (void)local_player_id; (void)player_ship_id;
    (void)entity_alive; (void)set_entity_transform; (void)spawn_as;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_spawn_pins(void);
void sco_abi_spawn_pins(void) { (void)&pin_spawn_signatures; }
