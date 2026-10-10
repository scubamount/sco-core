/*
 * abi_ipc.c: pins the layout of sco_ipc.h (the host service "sco.ipc", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sco_ipc_v1; existing lines never change. The wire structs of sc_ipc.h pin themselves with
 * static_asserts in that header, so including it here checks them too.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; the CMake test abi_ipc rebuilds it.
 */
#include <stddef.h>
#include <stdint.h>

#include "sco_ipc.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, limits ---- */
PIN(SCO_IPC_VERSION_1_0 == 0x00010000u);
PIN(SCO_IPC_MAX_NAME == 31u);
PIN(SCO_IPC_MAX_CHANNELS == 16u);
PIN(SCO_IPC_MAX_RINGS == 32u);
PIN(SCO_IPC_MIN_CHANNEL_BYTES == 4096u);
PIN(SCO_IPC_MAX_CHANNEL_BYTES == 268435456u);
PIN(SCO_IPC_MAX_PLUGIN_BYTES == 536870912u);
PIN(SCO_IPC_TO_PEER == 1u);
PIN(SCO_IPC_FROM_PEER == 2u);
PIN(sizeof(SCO_IPC_NAME) == 8); /* "sco.ipc" */

/* ---- sco_ipc_v1 ---- */
SIZE(sco_ipc_v1, 80);
AT(sco_ipc_v1, size, 0);
AT(sco_ipc_v1, _pad, 4);
AT(sco_ipc_v1, create, 8);
AT(sco_ipc_v1, peer_age_ms, 16);
AT(sco_ipc_v1, block_write, 24);
AT(sco_ipc_v1, block_read, 32);
AT(sco_ipc_v1, ring_init, 40);
AT(sco_ipc_v1, ring_push, 48);
AT(sco_ipc_v1, ring_pop, 56);
AT(sco_ipc_v1, view, 64);
AT(sco_ipc_v1, close, 72);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_ipc_signatures(const sco_ipc_v1* t) {
    sco_result (*create)(sco_plugin*, const char*, uint64_t, uint32_t, uint32_t, uint64_t*) = t->create;
    sco_result (*peer_age_ms)(sco_plugin*, uint64_t, uint32_t*) = t->peer_age_ms;
    sco_result (*block_write)(sco_plugin*, uint64_t, uint64_t, const void*, uint32_t) = t->block_write;
    sco_result (*block_read)(sco_plugin*, uint64_t, uint64_t, void*, uint32_t) = t->block_read;
    sco_result (*ring_init)(sco_plugin*, uint64_t, uint64_t, uint64_t, uint32_t) = t->ring_init;
    sco_result (*ring_push)(sco_plugin*, uint64_t, uint64_t, uint32_t, const void*, uint32_t) = t->ring_push;
    sco_result (*ring_pop)(sco_plugin*, uint64_t, uint64_t, uint32_t*, void*, uint32_t*) = t->ring_pop;
    sco_result (*view)(sco_plugin*, uint64_t, void**, uint64_t*) = t->view;
    sco_result (*close_fn)(sco_plugin*, uint64_t) = t->close;
    (void)create; (void)peer_age_ms; (void)block_write; (void)block_read; (void)ring_init; (void)ring_push;
    (void)ring_pop; (void)view; (void)close_fn;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_ipc_pins(void);
void sco_abi_ipc_pins(void) { (void)&pin_ipc_signatures; }
