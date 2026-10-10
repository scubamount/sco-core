/*
 * abi_net.c: pins the layout of sco_net.h (the host service "sco.net", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sco_net_v1; existing lines never change. The wire constants it reuses are pinned by
 * abi_sc_net.c.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; the CMake test abi_net rebuilds it.
 */
#include <stddef.h>
#include <stdint.h>

#include "sco_net.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, limits, flags ---- */
PIN(SCO_NET_VERSION_1_0 == 0x00010000u);
PIN(sizeof(SCO_NET_NAME) == 8); /* "sco.net" */
PIN(SCO_NET_MAX_PEERS == 16u);
PIN(SCO_NET_MAX_UNREL == 1200u);
PIN(SCO_NET_MAX_RELIABLE == 262144u);
PIN(SCO_NET_MAX_FQN == 64u);
PIN(SCO_NET_MAX_NAME == 64u);
PIN(SCO_NET_MAX_CHANNELS == 32u);
PIN(SCO_NET_QUOTA_MSGS == 256u);
PIN(SCO_NET_QUOTA_BYTES == 524288u);
PIN(SCO_NET_RELIABLE == 0x1u);
PIN(SCO_NET_FROM_HOST == 0x2u);
PIN(SCO_NET_TO_HOST == 0x4u);
PIN(SCO_NET_PEER_JOINED == 1u);
PIN(SCO_NET_PEER_LEFT == 2u);
PIN(SCO_NET_PEER_ENTITY == 3u);
PIN(sizeof(SCO_NET_EVENT_STATE) == 10); /* "net.state" */
PIN(sizeof(SCO_NET_EVENT_PEER) == 9);   /* "net.peer" */

/* ---- data ---- */
SIZE(sco_net_peer, 16);
AT(sco_net_peer, peer_id, 0);
AT(sco_net_peer, entity_id, 8);

SIZE(sco_net_state_event, 136);
AT(sco_net_state_event, size, 0);
AT(sco_net_state_event, active, 4);
AT(sco_net_state_event, reason, 8);

SIZE(sco_net_peer_event, 24);
AT(sco_net_peer_event, size, 0);
AT(sco_net_peer_event, what, 4);
AT(sco_net_peer_event, peer, 8);

/* ---- sco_net_v1 ---- */
SIZE(sco_net_v1, 64);
AT(sco_net_v1, size, 0);
AT(sco_net_v1, _pad, 4);
AT(sco_net_v1, is_active, 8);
AT(sco_net_v1, get_peers, 16);
AT(sco_net_v1, get_peer_name, 24);
AT(sco_net_v1, send_channel, 32);
AT(sco_net_v1, register_channel, 40);
AT(sco_net_v1, unregister_channel, 48);
AT(sco_net_v1, self_peer, 56);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_net_signatures(const sco_net_v1* t) {
    int (*is_active)(void) = t->is_active;
    sco_result (*get_peers)(sco_net_peer*, uint32_t*) = t->get_peers;
    sco_result (*get_peer_name)(uint64_t, char*, uint32_t) = t->get_peer_name;
    sco_result (*send_channel)(sco_plugin*, const char*, const void*, uint32_t) = t->send_channel;
    sco_result (*register_channel)(sco_plugin*, const char*, uint32_t, uint32_t, sco_net_on_message, void*) =
        t->register_channel;
    sco_result (*unregister_channel)(sco_plugin*, const char*) = t->unregister_channel;
    uint64_t (*self_peer)(void) = t->self_peer;
    void (*on_message)(uint64_t, const void*, uint32_t, void*) = (sco_net_on_message)0;
    (void)is_active; (void)get_peers; (void)get_peer_name; (void)send_channel; (void)register_channel;
    (void)unregister_channel; (void)self_peer; (void)on_message;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_net_pins(void);
void sco_abi_net_pins(void) { (void)&pin_net_signatures; }
