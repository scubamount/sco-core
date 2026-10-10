/*
 * sco_net.h: the host service "sco.net", version 1.0.
 *
 * Typed message channels between players in a private co-presence session (design
 * docs/design/multiplayer.md section 3.1; plugin guide docs/net.md). Plain C; usable from C and
 * C++. Published by the host (a host-owned service under the reserved id "sco"), found with
 * sco_api 1.1 query_service. The host also sets the capability "sco.net" while the service is
 * published, so a plugin.ini with `requires = sco.net` is refused with a reason on a host
 * without it:
 *
 *   static void OnPose(uint64_t from, const void* buf, uint32_t len, void* ctx) { ... }
 *   const sco_net_v1* net = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SCO_NET_NAME, SCO_NET_VERSION_1_0, (const void**)&net) == SCO_OK) {
 *       net->register_channel(self, "myplugin.pose", 0, 64, OnPose, NULL);   // unreliable
 *       ...
 *       if (net->is_active()) net->send_channel(self, "myplugin.pose", &pose, sizeof pose);
 *   }
 *
 * Plugins never open, join or leave a session: that is the product's decision (sc-offline's
 * launcher and menu, through the host-side C++ API sco/net/session.h). With no session,
 * is_active() is 0 and send_channel answers SCO_UNAVAILABLE; nothing listens or connects.
 *
 * Layout pinned by tests/abi_net.c. The rules of sco_api.h hold here too (4-byte enums, results
 * instead of exceptions, 64-bit only), plus:
 *  - Channels. A channel is "<plugin id>.<name>": name is one or more dot-separated parts of
 *    [A-Za-z0-9_-], the whole at most SCO_NET_MAX_FQN bytes. A plugin registers and sends only
 *    on channels under its own id; anything else is SCO_BAD_ARG. Peers learn each other's
 *    channels when they join; a message on a channel no other peer registered goes nowhere.
 *  - Ids, never pointers. Peers are opaque uint64_t ids, unique within a session (the host is
 *    1); entity_id is the session-scoped id of the ghost the product spawned for that peer, 0
 *    when there is none.
 *  - Threads. Every function may be called from any thread. Callbacks run on the game thread,
 *    from the host tick, as callouts of the plugin that registered them (a fault disables only
 *    that plugin); buf is valid for the call only. A callback may call any function here.
 *  - Quotas. Each plugin may send SCO_NET_QUOTA_MSGS messages and SCO_NET_QUOTA_BYTES bytes per
 *    second by default (a product may change both; one second's worth may be sent at once).
 *    Over either, send_channel answers SCO_TOO_MANY and the message is dropped.
 *  - Unload. When the plugin unloads or crashes the host drops its channels and every message
 *    still queued for it; later calls naming that self are SCO_BAD_ARG. Once the host has
 *    stopped the service, calls answer SCO_UNAVAILABLE (is_active 0).
 *  - Plaintext. Messages are authenticated and integrity-checked but not encrypted (anyone on
 *    the path can read them); never send secrets or personal data.
 *
 * Events on the bus (sco_api subscribe), dispatched on the game thread from the host tick; data
 * starts with its size:
 *   SCO_NET_EVENT_STATE "net.state"  sco_net_state_event: a session came up or ended
 *   SCO_NET_EVENT_PEER  "net.peer"   sco_net_peer_event: a peer joined, left or got an entity
 *
 * License: GPL-3.0, like the rest of sco-core (sc_net.h alone is MIT).
 */
#ifndef SCO_NET_H
#define SCO_NET_H

#include "sco_api.h"
#include "sc_net.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCO_NET_NAME "sco.net"
#define SCO_NET_VERSION_1_0 0x00010000u

#define SCO_NET_MAX_PEERS     SC_NET_MAX_PEERS     /* 16: a session, self included */
#define SCO_NET_MAX_UNREL     SC_NET_MAX_UNREL     /* 1200: bytes in one unreliable message */
#define SCO_NET_MAX_RELIABLE  SC_NET_MAX_RELIABLE  /* 262144: bytes in one reliable message */
#define SCO_NET_MAX_FQN       SC_NET_MAX_FQN       /* 64: bytes in a channel name, NUL excluded */
#define SCO_NET_MAX_NAME      SC_NET_MAX_NAME      /* 64: bytes in a player name, NUL excluded */
#define SCO_NET_MAX_CHANNELS  32u                  /* registered channels per plugin */

#define SCO_NET_QUOTA_MSGS    256u                 /* default send quota per plugin: messages/s */
#define SCO_NET_QUOTA_BYTES   524288u              /* and bytes/s (512 KiB) */

/* register_channel flags. Default (0) is unreliable to every other peer. */
#define SCO_NET_RELIABLE   SC_NET_RELIABLE    /* 0x1: exactly once, in order, up to 256 KiB */
#define SCO_NET_FROM_HOST  SC_NET_FROM_HOST   /* 0x2: only the session host sends; joiners receive */
#define SCO_NET_TO_HOST    SC_NET_TO_HOST     /* 0x4: joiners send to the host only (not relayed) */

#define SCO_NET_EVENT_STATE "net.state"
#define SCO_NET_EVENT_PEER  "net.peer"

/* sco_net_peer_event.what */
#define SCO_NET_PEER_JOINED 1u
#define SCO_NET_PEER_LEFT   2u
#define SCO_NET_PEER_ENTITY 3u   /* the product set (or cleared) the peer's entity_id */

/* A peer in the session. */
typedef struct sco_net_peer {
    uint64_t peer_id;
    uint64_t entity_id;   /* session-scoped; 0 when the product has spawned nothing for it */
} sco_net_peer;

/* "net.state": the session came up (active 1) or ended or failed (active 0, reason says why). */
typedef struct sco_net_state_event {
    uint32_t size;        /* sizeof(sco_net_state_event) */
    int32_t  active;
    char     reason[128]; /* UTF-8, NUL-terminated; empty when active */
} sco_net_state_event;

/* "net.peer": what happened to peer. */
typedef struct sco_net_peer_event {
    uint32_t     size;    /* sizeof(sco_net_peer_event) */
    uint32_t     what;    /* SCO_NET_PEER_* */
    sco_net_peer peer;
} sco_net_peer_event;

/* A message on a registered channel: sender_peer_id is the peer that sent it (relayed messages
 * keep their origin). Game thread; buf is valid for the call only. */
typedef void (*sco_net_on_message)(uint64_t sender_peer_id, const void* buf, uint32_t len, void* ctx);

typedef struct sco_net_v1 {
    uint32_t size; /* sizeof(sco_net_v1) as the host built it */
    uint32_t _pad;

    /* 1 while a session is up (hosting, or joined with the channel table received), else 0. */
    int (*is_active)(void);
    /* The session's peers, self first, then by id. *inout_count holds out's capacity on entry
     * (out may be NULL with 0) and the number of peers on return; up to the capacity are
     * written. SCO_TOO_MANY: more peers than the capacity (the first ones were written).
     * SCO_OK with 0 when no session is active. SCO_BAD_ARG: inout_count NULL, or out NULL with a
     * nonzero capacity. */
    sco_result (*get_peers)(sco_net_peer* out, uint32_t* inout_count);
    /* Copies the peer's chosen name (UTF-8) into buf, NUL-terminated, cut to cap - 1 bytes.
     * SCO_NOT_FOUND: no such peer in the active session. SCO_BAD_ARG: buf NULL or cap 0. */
    sco_result (*get_peer_name)(uint64_t peer_id, char* buf, uint32_t cap);
    /* Queues len bytes on channel_fqn to the other peers (only the host for SCO_NET_TO_HOST).
     * Copies buf; never blocks. SCO_BAD_ARG: a name outside this plugin's id, len over the
     * channel's max_len, buf NULL with len > 0, or the wrong direction (a FROM_HOST channel
     * from a joiner, a TO_HOST channel from the host). SCO_NOT_FOUND: this plugin hasn't
     * registered the channel. SCO_UNAVAILABLE: no active session. SCO_TOO_MANY: over the send
     * quota, or the plugin's outgoing queue is full; dropped. A message no other peer has a
     * channel for is dropped silently. */
    sco_result (*send_channel)(sco_plugin* self, const char* channel_fqn, const void* buf, uint32_t len);
    /* Registers channel_fqn ("<this plugin id>.<name>") with flags (SCO_NET_*) and the largest
     * message this side accepts or sends, max_len (1..SCO_NET_MAX_UNREL, or
     * 1..SCO_NET_MAX_RELIABLE with SCO_NET_RELIABLE); a longer incoming message is dropped
     * before it is reassembled. cb runs on the game thread for each message; ctx is borrowed
     * until unregister_channel or unload. Works with or without a session. SCO_BAD_ARG: a bad
     * name, a name outside this plugin's id, one already registered, bad flags (FROM_HOST with
     * TO_HOST), a bad max_len or a NULL cb. SCO_TOO_MANY: SCO_NET_MAX_CHANNELS already. */
    sco_result (*register_channel)(sco_plugin* self, const char* channel_fqn, uint32_t flags, uint32_t max_len,
                                   sco_net_on_message cb, void* ctx);
    /* Removes one of this plugin's channels: no callback runs for it after this returns (on the
     * game thread; from another thread, one already running may finish). SCO_NOT_FOUND: not
     * registered by this plugin. */
    sco_result (*unregister_channel)(sco_plugin* self, const char* channel_fqn);
    /* This player's peer id in the active session (the host is 1), 0 when none. */
    uint64_t (*self_peer)(void);
} sco_net_v1;

#ifdef __cplusplus
}
#endif

#endif /* SCO_NET_H */
