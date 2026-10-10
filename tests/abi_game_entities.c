/*
 * abi_game_entities.c: pins the layout of sc_entities.h (the game pack's service "game.entities", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sc_entities_v1; existing lines never change. sdk/csharp/Sco.Sdk.Tests checks its C# copy against
 * the SIZE / AT / PIN lines below.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc.
 */
#include <stddef.h>
#include <stdint.h>

#include "sc_entities.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, limits, event bits ---- */
PIN(SC_ENTITIES_VERSION_1_0 == 0x00010000u);
PIN(sizeof(SC_ENTITIES_NAME) == 14); /* "game.entities" */
PIN(SC_ENTITIES_MAX_OWNED == 1024u);
PIN(SC_ENTITIES_MAX_WATCHES == 64u);
PIN(SC_ENTITIES_MAX_TYPE_LEN == 63u);
PIN(SC_ENTITY_STREAMED_IN == 1u);
PIN(SC_ENTITY_STREAMED_OUT == 2u);

/* ---- sc_entities_v1 ---- */
SIZE(sc_entities_v1, 88);
AT(sc_entities_v1, size, 0);
AT(sc_entities_v1, _pad, 4);
AT(sc_entities_v1, alive, 8);
AT(sc_entities_v1, class_of, 16);
AT(sc_entities_v1, get_transform, 24);
AT(sc_entities_v1, set_transform, 32);
AT(sc_entities_v1, spawn, 40);
AT(sc_entities_v1, despawn, 48);
AT(sc_entities_v1, watch, 56);
AT(sc_entities_v1, unwatch, 64);
AT(sc_entities_v1, query_radius, 72);
AT(sc_entities_v1, last_error, 80);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_entities_signatures(const sc_entities_v1* e) {
    int (*alive)(uint64_t) = e->alive;
    sco_result (*class_of)(uint64_t, char*, uint32_t*) = e->class_of;
    sco_result (*get_transform)(uint64_t, double*, double*, uint64_t*) = e->get_transform;
    sco_result (*set_transform)(sco_plugin*, uint64_t, uint64_t, const double*, const double*) = e->set_transform;
    sco_result (*spawn)(sco_plugin*, const char*, uint64_t, const double*, const double*, uint64_t*) = e->spawn;
    sco_result (*despawn)(sco_plugin*, uint64_t) = e->despawn;
    sco_result (*watch)(sco_plugin*, uint32_t, const char*, sc_entity_watch_fn, void*, uint64_t*) = e->watch;
    sco_result (*unwatch)(sco_plugin*, uint64_t) = e->unwatch;
    sco_result (*query_radius)(uint64_t, const double*, double, const char*, uint64_t*, uint32_t, uint32_t*, uint32_t*) = e->query_radius;
    sco_result (*last_error)(sco_plugin*, char*, uint32_t*) = e->last_error;
    void (*fn)(void*, uint32_t, uint64_t, const char*) = (sc_entity_watch_fn)0;
    (void)alive; (void)class_of; (void)get_transform; (void)set_transform; (void)spawn; (void)despawn;
    (void)watch; (void)unwatch; (void)query_radius; (void)last_error; (void)fn;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_game_entities_pins(void);
void sco_abi_game_entities_pins(void) { (void)&pin_entities_signatures; }
