/*
 * sco_ipc.h: the host service "sco.ipc", version 1.0.
 *
 * Local shared-memory channels between a plugin and another program on the same PC (a bridge to
 * another game). The other program includes only sc_ipc.h (MIT), the wire this service lays out;
 * see docs/ipc.md. Plain C; usable from C and C++. Published by the host (a host-owned service
 * under the reserved id "sco"), found with sco_api 1.1 query_service:
 *
 *   const sco_ipc_v1* ipc = NULL;
 *   uint64_t ch = 0;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SCO_IPC_NAME, SCO_IPC_VERSION_1_0, (const void**)&ipc) == SCO_OK &&
 *       ipc->create(self, "link", 1u << 20, MY_LAYOUT_ID, 1, &ch) == SCO_OK) {
 *       ipc->ring_init(self, ch, 4096, 65536, SCO_IPC_TO_PEER);
 *       ipc->ring_push(self, ch, 4096, MSG_HELLO, &hello, sizeof hello);
 *   }
 *
 * Layout pinned by tests/abi_ipc.c. The rules of sco_api.h hold here too (4-byte enums, results
 * instead of exceptions, 64-bit only), plus:
 *  - Names. A channel is the mapping Local\SCO_<plugin id>.<name>; name is 1-31 characters of
 *    [a-z0-9_]. The host builds the full name, so a plugin can never pick Global\, another
 *    plugin's prefix or an arbitrary name. The mapping's DACL grants the current user only.
 *  - Sizes. bytes (the whole mapping, sc_ipc_hdr included) is SCO_IPC_MIN_CHANNEL_BYTES to
 *    SCO_IPC_MAX_CHANNEL_BYTES; a plugin's open channels total at most SCO_IPC_MAX_PLUGIN_BYTES;
 *    at most SCO_IPC_MAX_CHANNELS open channels per plugin, SCO_IPC_MAX_RINGS rings per channel.
 *  - Offsets are from the start of the mapping and never reach into the 64-byte header. A block
 *    is an sc_ipc_block at a multiple of 8; a ring an sc_ipc_ring at a multiple of 64.
 *  - Ids, never pointers: a channel is a uint64_t id. The one exception is view, which hands out
 *    the plugin's own mapping (bulk data such as video frames can't be copied per call); that
 *    pointer is valid until close or unload.
 *  - Nothing read from the mapping is trusted: the peer can write anything. A record, counter or
 *    header that fails validation is SCO_FAILED and is never followed outside the mapping.
 *  - Any thread; block_* and ring_* never block (a call holds the channel's lock only for its
 *    copy). The owner heartbeat is written from the host tick.
 *  - When the plugin unloads or crashes, the host marks each of its channels closed, stops its
 *    heartbeat and unmaps it; every later call naming that self is SCO_BAD_ARG. Once the host
 *    has stopped the service, calls answer SCO_UNAVAILABLE.
 *
 * License: GPL-3.0, like the rest of sco-core (sc_ipc.h alone is MIT).
 */
#ifndef SCO_IPC_H
#define SCO_IPC_H

#include "sco_api.h"
#include "sc_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCO_IPC_NAME "sco.ipc"
#define SCO_IPC_VERSION_1_0 0x00010000u

#define SCO_IPC_MAX_NAME          31u         /* channel name bytes, NUL excluded */
#define SCO_IPC_MAX_CHANNELS      16u         /* open channels per plugin */
#define SCO_IPC_MAX_RINGS         32u         /* rings per channel */
#define SCO_IPC_MIN_CHANNEL_BYTES 4096u       /* bytes per channel, sc_ipc_hdr included */
#define SCO_IPC_MAX_CHANNEL_BYTES 268435456u  /* 256 MiB */
#define SCO_IPC_MAX_PLUGIN_BYTES  536870912u  /* 512 MiB over a plugin's open channels */

#define SCO_IPC_TO_PEER   SC_IPC_TO_PEER      /* ring_init: the plugin pushes, the peer pops */
#define SCO_IPC_FROM_PEER SC_IPC_FROM_PEER    /* the peer pushes, the plugin pops */

typedef struct sco_ipc_v1 {
    uint32_t size; /* sizeof(sco_ipc_v1) as the host built it */
    uint32_t _pad;

    /* Creates (or, if the peer still holds it, re-opens) Local\SCO_<plugin id>.<name> with bytes
     * bytes and lays out the sc_ipc_hdr with the bridge's layout_id / layout_version and a new
     * epoch (rings must be laid out again). SCO_BAD_ARG: a bad name or size, or a channel of that
     * name already open. SCO_TOO_MANY: the channel or byte quota is full. SCO_FAILED: the OS
     * refused, or an existing mapping of that name is smaller or another user's. */
    sco_result (*create)(sco_plugin* self, const char* name, uint64_t bytes, uint32_t layout_id,
                         uint32_t layout_version, uint64_t* out_channel);
    /* Milliseconds since the peer's last beat (capped at UINT32_MAX). SCO_NOT_FOUND: no such
     * channel, or the peer hasn't beaten yet (*out_ms 0). */
    sco_result (*peer_age_ms)(sco_plugin* self, uint64_t channel, uint32_t* out_ms);
    /* Writes a seqlock snapshot of size bytes into the block at offset. */
    sco_result (*block_write)(sco_plugin* self, uint64_t channel, uint64_t offset, const void* data, uint32_t size);
    /* Reads exactly size bytes of the block at offset. SCO_NOT_FOUND: never written. SCO_BAD_ARG:
     * the writer's snapshot has another size. SCO_FAILED: the writer kept it torn; retry. */
    sco_result (*block_read)(sco_plugin* self, uint64_t channel, uint64_t offset, void* out, uint32_t size);
    /* Lays out an empty ring at offset with capacity data bytes (a power of two, at least 64), in
     * one direction (SCO_IPC_TO_PEER or SCO_IPC_FROM_PEER). Re-initializing a ring at the same
     * offset resets it. SCO_BAD_ARG: outside the mapping, overlapping another ring, a bad
     * capacity or direction. SCO_TOO_MANY: SCO_IPC_MAX_RINGS already. */
    sco_result (*ring_init)(sco_plugin* self, uint64_t channel, uint64_t offset, uint64_t capacity,
                            uint32_t direction);
    /* Appends one record to the SCO_IPC_TO_PEER ring at offset. SCO_TOO_MANY: full now.
     * SCO_NOT_FOUND: no ring laid out there. SCO_BAD_ARG: a FROM_PEER ring, type 0xFFFFFFFF, or a
     * record the ring can never hold. */
    sco_result (*ring_push)(sco_plugin* self, uint64_t channel, uint64_t offset, uint32_t type, const void* data,
                            uint32_t size);
    /* Takes the oldest record of the SCO_IPC_FROM_PEER ring at offset, with the size handshake:
     * *inout_size holds out's capacity on entry, the record's size on return; with SCO_TOO_MANY
     * the size needed, and the record stays queued (pass out NULL with *inout_size 0 to ask).
     * out_type may be NULL. SCO_NOT_FOUND: empty, or no ring there. SCO_FAILED: the peer wrote
     * something invalid. */
    sco_result (*ring_pop)(sco_plugin* self, uint64_t channel, uint64_t offset, uint32_t* out_type, void* out,
                           uint32_t* inout_size);
    /* The plugin's own mapping (header included) and its size, for bulk regions. Valid until close
     * or unload; the host still validates everything it reads there. */
    sco_result (*view)(sco_plugin* self, uint64_t channel, void** out_base, uint64_t* out_bytes);
    /* Marks the channel closed for the peer and unmaps it. SCO_NOT_FOUND: no such channel. */
    sco_result (*close)(sco_plugin* self, uint64_t channel);
} sco_ipc_v1;

#ifdef __cplusplus
}
#endif

#endif /* SCO_IPC_H */
