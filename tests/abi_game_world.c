/*
 * abi_game_world.c: pins the layout of sc_world.h (the game pack's service "game.world", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sc_world_v1 and sc_world_hit; existing lines never change. sdk/csharp/Sco.Sdk.Tests checks its C# copy against
 * the SIZE / AT / PIN lines below.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc.
 */
#include <stddef.h>
#include <stdint.h>

#include "sco_api.h"
#include "sc_world.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, limits ---- */
PIN(SC_WORLD_VERSION_1_0 == 0x00010000u);
PIN(sizeof(SC_WORLD_NAME) == 11); /* "game.world" */
PIN(SC_WORLD_RAY_MAX_DISTANCE == 20000);

/* ---- sc_world_hit_flag ---- */
SIZE(sc_world_hit_flag, 4);
PIN(SC_WORLD_HIT_ENTITY == 0x1);
PIN(SC_WORLD_HIT_NORMAL == 0x2);
PIN(SC_WORLD_HIT_FLAG_FORCE32 == 0x7fffffff);

/* ---- sc_world_hit ---- */
SIZE(sc_world_hit, 72);
AT(sc_world_hit, size, 0);
AT(sc_world_hit, flags, 4);
AT(sc_world_hit, entity_id, 8);
AT(sc_world_hit, pos, 16);
AT(sc_world_hit, normal, 40);
AT(sc_world_hit, distance, 64);

/* ---- sc_world_v1 ---- */
SIZE(sc_world_v1, 32);
AT(sc_world_v1, size, 0);
AT(sc_world_v1, _pad, 4);
AT(sc_world_v1, raycast, 8);
AT(sc_world_v1, camera, 16);
AT(sc_world_v1, last_error, 24);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_world_signatures(const sc_world_v1* w) {
    sco_result (*raycast)(uint64_t, const double*, const double*, double, sc_world_hit*) = w->raycast;
    sco_result (*camera)(double*, double*, uint64_t*) = w->camera;
    sco_result (*last_error)(sco_plugin*, char*, uint32_t*) = w->last_error;
    (void)raycast; (void)camera; (void)last_error;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_game_world_pins(void);
void sco_abi_game_world_pins(void) { (void)&pin_world_signatures; }
