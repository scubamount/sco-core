/*
 * abi_spatial.c: pins the layout of sc_spatial.h (sc-offline's service "teleport.spatial", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sc_spatial_v1; existing lines never change.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc.
 */
#include <stddef.h>
#include <stdint.h>

#include "sc_spatial.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version ---- */
PIN(SC_SPATIAL_SERVICE_VERSION == 0x00010000u);
PIN(sizeof(SC_SPATIAL_SERVICE_NAME) == 17); /* "teleport.spatial" */

/* ---- sc_spatial_v1 ---- */
SIZE(sc_spatial_v1, 56);
AT(sc_spatial_v1, size, 0);
AT(sc_spatial_v1, player_pose, 8);
AT(sc_spatial_v1, zone_of_entity, 16);
AT(sc_spatial_v1, local_to_world, 24);
AT(sc_spatial_v1, world_to_local, 32);
AT(sc_spatial_v1, zone_to_zone, 40);
AT(sc_spatial_v1, zone_name, 48);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_spatial_signatures(const sc_spatial_v1* s) {
    int (*player_pose)(double*, double*, uint64_t*) = s->player_pose;
    int (*zone_of_entity)(uint64_t, uint64_t*) = s->zone_of_entity;
    int (*local_to_world)(uint64_t, const double*, double*) = s->local_to_world;
    int (*world_to_local)(uint64_t, const double*, double*) = s->world_to_local;
    int (*zone_to_zone)(uint64_t, uint64_t, const double*, double*) = s->zone_to_zone;
    int (*zone_name)(uint64_t, char*, uint32_t) = s->zone_name;
    (void)player_pose; (void)zone_of_entity; (void)local_to_world; (void)world_to_local;
    (void)zone_to_zone; (void)zone_name;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_spatial_pins(void);
void sco_abi_spatial_pins(void) { (void)&pin_spatial_signatures; }
