/*
 * abi_game_actors.c: pins the layout of sc_actors.h (the game pack's service "game.actors", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sc_actors_v1; existing lines never change. sdk/csharp/Sco.Sdk.Tests checks its C# copy against
 * the SIZE / AT / PIN lines below.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc.
 */
#include <stddef.h>
#include <stdint.h>

#include "sc_actors.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, limits ---- */
PIN(SC_ACTORS_VERSION_1_0 == 0x00010000u);
PIN(sizeof(SC_ACTORS_NAME) == 12); /* "game.actors" */
PIN(SC_ACTORS_MAX_NPCS == 1024u);

/* ---- sc_actors_v1 ---- */
SIZE(sc_actors_v1, 40);
AT(sc_actors_v1, size, 0);
AT(sc_actors_v1, _pad, 4);
AT(sc_actors_v1, local_player, 8);
AT(sc_actors_v1, spawn_npc, 16);
AT(sc_actors_v1, despawn, 24);
AT(sc_actors_v1, last_error, 32);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_actors_signatures(const sc_actors_v1* a) {
    sco_result (*local_player)(uint64_t*, uint64_t*) = a->local_player;
    sco_result (*spawn_npc)(sco_plugin*, const char*, uint64_t, const double*, uint64_t*) = a->spawn_npc;
    sco_result (*despawn)(sco_plugin*, uint64_t) = a->despawn;
    sco_result (*last_error)(sco_plugin*, char*, uint32_t*) = a->last_error;
    (void)local_player; (void)spawn_npc; (void)despawn; (void)last_error;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_game_actors_pins(void);
void sco_abi_game_actors_pins(void) { (void)&pin_actors_signatures; }
