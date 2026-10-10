/* SPDX-License-Identifier: MIT */
/*
 * sc_ipc.h: the shared-memory wire of sco.ipc bridges (docs/ipc.md).
 *
 * MIT, unlike the rest of sco-core (GPL-3.0): this one file is the interface exception named in
 * LICENSE and CONTRIBUTING.md, so the non-GPL side of a bridge (a Northstar plugin, another
 * game's mod) may include it and speak the protocol. It has no sco-core dependency: plain C11 or
 * C++20, freestanding headers only, header-only.
 *
 * Copyright (c) 2026 the sco-core contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * ---- The channel ----------------------------------------------------------------------------
 *
 * A channel is one named mapping, Local\SCO_<plugin id>.<channel>, created by sc-offline's side
 * (the owner, through the sco.ipc service) for the current user only. The other program (the
 * peer) opens it by name (OpenFileMappingW + MapViewOfFile) and never creates it.
 *
 *   offset 0     sc_ipc_hdr (64 bytes): magic, version, pids, epoch, heartbeats, size, layout
 *   offset 64..  the bridge's layout: sc_ipc_ring and sc_ipc_block regions at offsets both
 *                sides agree on (layout_id / layout_version name that agreement)
 *
 * Every function here takes a local view (sc_ipc_chan, sc_ipc_ring_view) that never lives in the
 * shared memory: it holds the size and geometry validated once, at attach. After that, nothing a
 * function reads from the mapping is trusted: the other process can write anything at any time,
 * so every length, offset, index and sequence number read from shared memory is checked against
 * the cached geometry before use, and a value that fails is SC_IPC_CORRUPT, never an access
 * outside the mapping.
 *
 * Epoch: the owner bumps hdr.epoch each time it (re)initializes the channel (sc-offline
 * restarted while the peer kept its mapping). A view remembers the epoch it attached under; once
 * the header or a ring carries another, every call answers SC_IPC_EPOCH and the side re-attaches.
 *
 * Heartbeats are milliseconds of one machine-wide monotonic clock: GetTickCount64() on Windows
 * (CLOCK_MONOTONIC elsewhere). The owner beats from its host tick; the peer should beat at least
 * once a second. A side whose partner's age passes its own timeout treats the link as down.
 *
 * Atomics: a small shim over compiler builtins, not C11 <stdatomic.h> or C++ std::atomic_ref.
 * The fields are plain uint32_t / uint64_t in plain structs, so the very same struct (and its
 * static_asserts) compiles as C and as C++; <stdatomic.h> would need _Atomic members (another
 * type, not portable to C++20 and not in MSVC's C mode without an experimental switch), and
 * atomic_ref is C++ only. GCC and Clang (clang-cl included) use the __atomic builtins, which
 * ThreadSanitizer understands. MSVC on x64 uses volatile accesses fenced with _ReadWriteBarrier:
 * x64 keeps loads ordered with loads and stores with stores, so an aligned load is an acquire and
 * an aligned store a release once the compiler can't move them. Other MSVC targets are refused.
 *
 * Results are int: SC_IPC_OK (0) or one of the SC_IPC_* codes below.
 */
#ifndef SC_IPC_H
#define SC_IPC_H

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#if !defined(_M_X64)
#error "sc_ipc.h: MSVC builds are x64 only"
#endif
#include <intrin.h> /* _ReadWriteBarrier */
#endif

#ifdef __cplusplus
#define SC_IPC_STATIC_ASSERT(e, m) static_assert(e, m)
extern "C" {
#else
#define SC_IPC_STATIC_ASSERT(e, m) _Static_assert(e, m)
#endif

/* ---- constants ------------------------------------------------------------------------------ */

#define SC_IPC_MAGIC        0x5343494Fu /* "SCIO", sc_ipc_hdr.magic */
#define SC_IPC_RING_MAGIC   0x53435247u /* "SCRG", sc_ipc_ring.magic */
#define SC_IPC_VERSION      1u          /* sc_ipc_hdr.version: this file's wire */
#define SC_IPC_HEADER_BYTES 64u         /* sizeof(sc_ipc_hdr): the bridge's layout starts here */
#define SC_IPC_RING_BYTES   192u        /* sizeof(sc_ipc_ring): the ring's data follows it */
#define SC_IPC_BLOCK_BYTES  16u         /* sizeof(sc_ipc_block): the block's bytes follow it */
#define SC_IPC_REC_BYTES    8u          /* sizeof(sc_ipc_rec): one record's header in a ring */
#define SC_IPC_RING_MIN     64u         /* smallest ring capacity (a power of two) */
#define SC_IPC_REC_PAD      0xFFFFFFFFu /* record type marking the unused end of a ring */

#define SC_IPC_TO_PEER      1u /* sc_ipc_ring.direction: the owner pushes, the peer pops */
#define SC_IPC_FROM_PEER    2u /* the peer pushes, the owner pops */

#define SC_IPC_STATE_OPEN   1u /* sc_ipc_hdr.state */
#define SC_IPC_STATE_CLOSED 2u /* the owner closed the channel (or its plugin unloaded) */

/* Results. */
#define SC_IPC_OK         0
#define SC_IPC_FULL       1  /* push: no room now; pop later frees it */
#define SC_IPC_EMPTY      2  /* pop: nothing to read */
#define SC_IPC_TOO_SMALL  3  /* pop: *inout_size now holds the record's size; it stays queued */
#define SC_IPC_BAD_ARG    4  /* the caller's arguments: NULL, an offset or size outside the mapping */
#define SC_IPC_CORRUPT    5  /* the shared memory fails validation (a hostile or broken peer) */
#define SC_IPC_EPOCH      6  /* the owner re-created the channel or ring: attach again */
#define SC_IPC_BUSY       7  /* a block or the header is being written: try again */
#define SC_IPC_NOT_READY  8  /* not initialized yet (no magic, a block never written, no heartbeat) */
#define SC_IPC_MISMATCH   9  /* another wire version, layout or block size */
#define SC_IPC_GONE       10 /* the owner closed the channel */

/* ---- shared memory layout (identical on both sides; pinned below) -------------------------- */

/* The channel header at offset 0. Written by the owner except peer_pid and peer_heartbeat_ms. */
typedef struct sc_ipc_hdr {
    uint32_t magic;              /* SC_IPC_MAGIC once initialized; 0 while (re)initializing */
    uint32_t version;            /* SC_IPC_VERSION */
    uint32_t owner_pid;          /* the process that created the channel (sc-offline) */
    uint32_t peer_pid;           /* written by the peer (informational) */
    uint64_t epoch;              /* bumped each time the owner (re)initializes the channel */
    uint64_t owner_heartbeat_ms; /* the owner's last beat */
    uint64_t bytes;              /* the mapping's size as the owner created it */
    uint64_t peer_heartbeat_ms;  /* the peer's last beat; 0 until it beats */
    uint32_t layout_id;          /* the bridge's own layout: an id both sides agree on */
    uint32_t layout_version;     /* and its version */
    uint32_t state;              /* SC_IPC_STATE_OPEN or SC_IPC_STATE_CLOSED */
    uint32_t reserved;           /* 0 */
} sc_ipc_hdr;

/* A single-producer single-consumer byte ring: this header, then `capacity` bytes of records.
 * Its offset in the mapping is a multiple of 64; head and tail sit on their own cache lines. */
typedef struct sc_ipc_ring {
    uint32_t magic;     /* SC_IPC_RING_MAGIC once initialized */
    uint32_t direction; /* SC_IPC_TO_PEER or SC_IPC_FROM_PEER */
    uint64_t capacity;  /* data bytes after this header: a power of two, at least SC_IPC_RING_MIN */
    uint64_t epoch;     /* the channel epoch the owner laid it out under */
    uint8_t  pad0[40];
    uint64_t head;      /* producer: bytes ever written (wraps at 2^64) */
    uint64_t pushed;    /* producer: records ever written (informational) */
    uint8_t  pad1[48];
    uint64_t tail;      /* consumer: bytes ever read (wraps at 2^64) */
    uint64_t popped;    /* consumer: records ever read (informational) */
    uint8_t  pad2[48];
} sc_ipc_ring;

/* One record in a ring's data: this header, then `size` bytes, padded to a multiple of 8. A
 * record never wraps: when it doesn't fit before the end, the producer fills the end with one
 * SC_IPC_REC_PAD record (size = the rest minus 8) and writes the record at the start. */
typedef struct sc_ipc_rec {
    uint32_t size; /* payload bytes */
    uint32_t type; /* the bridge's message type; SC_IPC_REC_PAD is reserved */
} sc_ipc_rec;

/* A seqlock snapshot: this header, then `size` bytes. One writer per block. seq is even when
 * stable, odd while being written, 0 until first written. */
typedef struct sc_ipc_block {
    uint64_t seq;
    uint32_t size;     /* bytes of the last snapshot */
    uint32_t reserved; /* 0 */
} sc_ipc_block;

SC_IPC_STATIC_ASSERT(sizeof(sc_ipc_hdr) == SC_IPC_HEADER_BYTES, "sc_ipc_hdr size");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, magic) == 0, "sc_ipc_hdr.magic");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, version) == 4, "sc_ipc_hdr.version");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, owner_pid) == 8, "sc_ipc_hdr.owner_pid");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, peer_pid) == 12, "sc_ipc_hdr.peer_pid");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, epoch) == 16, "sc_ipc_hdr.epoch");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, owner_heartbeat_ms) == 24, "sc_ipc_hdr.owner_heartbeat_ms");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, bytes) == 32, "sc_ipc_hdr.bytes");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, peer_heartbeat_ms) == 40, "sc_ipc_hdr.peer_heartbeat_ms");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, layout_id) == 48, "sc_ipc_hdr.layout_id");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, layout_version) == 52, "sc_ipc_hdr.layout_version");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, state) == 56, "sc_ipc_hdr.state");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_hdr, reserved) == 60, "sc_ipc_hdr.reserved");
SC_IPC_STATIC_ASSERT(sizeof(sc_ipc_ring) == SC_IPC_RING_BYTES, "sc_ipc_ring size");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_ring, magic) == 0, "sc_ipc_ring.magic");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_ring, direction) == 4, "sc_ipc_ring.direction");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_ring, capacity) == 8, "sc_ipc_ring.capacity");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_ring, epoch) == 16, "sc_ipc_ring.epoch");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_ring, head) == 64, "sc_ipc_ring.head");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_ring, pushed) == 72, "sc_ipc_ring.pushed");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_ring, tail) == 128, "sc_ipc_ring.tail");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_ring, popped) == 136, "sc_ipc_ring.popped");
SC_IPC_STATIC_ASSERT(sizeof(sc_ipc_rec) == SC_IPC_REC_BYTES, "sc_ipc_rec size");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_rec, size) == 0, "sc_ipc_rec.size");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_rec, type) == 4, "sc_ipc_rec.type");
SC_IPC_STATIC_ASSERT(sizeof(sc_ipc_block) == SC_IPC_BLOCK_BYTES, "sc_ipc_block size");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_block, seq) == 0, "sc_ipc_block.seq");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_block, size) == 8, "sc_ipc_block.size");
SC_IPC_STATIC_ASSERT(offsetof(sc_ipc_block, reserved) == 12, "sc_ipc_block.reserved");

/* ---- local views (each process's own memory, never shared) -------------------------------- */

typedef struct sc_ipc_chan {
    uint8_t* base;  /* the mapping */
    uint64_t bytes; /* its size, validated at init / attach; every bound below uses this */
    uint64_t epoch; /* the epoch this view belongs to */
} sc_ipc_chan;

typedef struct sc_ipc_ring_view {
    sc_ipc_hdr*  hdr;
    sc_ipc_ring* ring;
    uint8_t*     data;      /* ring + SC_IPC_RING_BYTES */
    uint64_t     capacity;  /* cached at attach: never re-read from the mapping */
    uint64_t     epoch;
    uint32_t     direction;
    uint32_t     reserved;
} sc_ipc_ring_view;

/* ---- the atomics shim ------------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
static inline uint32_t sc_ipc_i_ld32(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline uint64_t sc_ipc_i_ld64(const uint64_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline uint32_t sc_ipc_i_ld32r(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline uint64_t sc_ipc_i_ld64r(const uint64_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline uint8_t sc_ipc_i_ld8r(const uint8_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void sc_ipc_i_st32(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void sc_ipc_i_st64(uint64_t* p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void sc_ipc_i_st32r(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline void sc_ipc_i_st64r(uint64_t* p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline void sc_ipc_i_st8r(uint8_t* p, uint8_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline void sc_ipc_i_acquire(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
static inline void sc_ipc_i_release(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
#else
static inline uint32_t sc_ipc_i_ld32(const uint32_t* p) {
    uint32_t v = *(const volatile uint32_t*)p;
    _ReadWriteBarrier();
    return v;
}
static inline uint64_t sc_ipc_i_ld64(const uint64_t* p) {
    uint64_t v = *(const volatile uint64_t*)p;
    _ReadWriteBarrier();
    return v;
}
static inline uint32_t sc_ipc_i_ld32r(const uint32_t* p) { return *(const volatile uint32_t*)p; }
static inline uint64_t sc_ipc_i_ld64r(const uint64_t* p) { return *(const volatile uint64_t*)p; }
static inline uint8_t sc_ipc_i_ld8r(const uint8_t* p) { return *(const volatile uint8_t*)p; }
static inline void sc_ipc_i_st32(uint32_t* p, uint32_t v) {
    _ReadWriteBarrier();
    *(volatile uint32_t*)p = v;
}
static inline void sc_ipc_i_st64(uint64_t* p, uint64_t v) {
    _ReadWriteBarrier();
    *(volatile uint64_t*)p = v;
}
static inline void sc_ipc_i_st32r(uint32_t* p, uint32_t v) { *(volatile uint32_t*)p = v; }
static inline void sc_ipc_i_st64r(uint64_t* p, uint64_t v) { *(volatile uint64_t*)p = v; }
static inline void sc_ipc_i_st8r(uint8_t* p, uint8_t v) { *(volatile uint8_t*)p = v; }
static inline void sc_ipc_i_acquire(void) { _ReadWriteBarrier(); }
static inline void sc_ipc_i_release(void) { _ReadWriteBarrier(); }
#endif

/* ---- internals ------------------------------------------------------------------------------- */

static inline uint64_t sc_ipc_i_align8(uint64_t n) { return (n + 7u) & ~(uint64_t)7u; }

static inline int sc_ipc_i_pow2(uint64_t n) { return n != 0 && (n & (n - 1u)) == 0; }

/* [offset, offset + len) lies inside [lo, bytes), without overflow. */
static inline int sc_ipc_i_inside(uint64_t bytes, uint64_t lo, uint64_t offset, uint64_t len) {
    return offset >= lo && offset <= bytes && len <= bytes - offset;
}

/* Copies out of shared memory with relaxed atomic loads (a seqlock reader races the writer by
 * design; relaxed accesses keep that defined and visible to ThreadSanitizer). */
static inline void sc_ipc_i_copy_out(void* dst, const uint8_t* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    uint64_t i = 0;
    if ((((uintptr_t)d | (uintptr_t)src) & 7u) == 0) {
        for (; i + 8u <= n; i += 8u) {
            const uint64_t w = sc_ipc_i_ld64r((const uint64_t*)(const void*)(src + i));
            *(uint64_t*)(void*)(d + i) = w;
        }
    }
    for (; i < n; ++i) d[i] = sc_ipc_i_ld8r(src + i);
}

/* Copies into shared memory with relaxed atomic stores. */
static inline void sc_ipc_i_copy_in(uint8_t* dst, const void* src, uint64_t n) {
    const uint8_t* s = (const uint8_t*)src;
    uint64_t i = 0;
    if ((((uintptr_t)dst | (uintptr_t)s) & 7u) == 0) {
        for (; i + 8u <= n; i += 8u) sc_ipc_i_st64r((uint64_t*)(void*)(dst + i), *(const uint64_t*)(const void*)(s + i));
    }
    for (; i < n; ++i) sc_ipc_i_st8r(dst + i, s[i]);
}

static inline sc_ipc_hdr* sc_ipc_i_hdr(const sc_ipc_chan* c) { return (sc_ipc_hdr*)(void*)c->base; }

/* ---- the channel ------------------------------------------------------------------------------ */

/* Owner: (re)initializes the header of a mapping of map_bytes (base 64-byte aligned, as a mapping
 * is) and fills *out. The epoch is one past the one found (1 for a fresh mapping), so a peer
 * still attached to the old channel gets SC_IPC_EPOCH. Rings must be laid out again
 * (sc_ipc_ring_init) after this. */
static inline int sc_ipc_init(void* base, uint64_t map_bytes, uint32_t owner_pid, uint32_t layout_id,
                              uint32_t layout_version, uint64_t now_ms, sc_ipc_chan* out) {
    sc_ipc_hdr* h = (sc_ipc_hdr*)base;
    uint64_t epoch = 1;
    if (!base || !out || map_bytes < SC_IPC_HEADER_BYTES || ((uintptr_t)base & 63u) != 0) return SC_IPC_BAD_ARG;
    if (sc_ipc_i_ld32(&h->magic) == SC_IPC_MAGIC) epoch = sc_ipc_i_ld64r(&h->epoch) + 1u;
    if (epoch == 0) epoch = 1;
    sc_ipc_i_st32(&h->magic, 0); /* peers stop trusting the header while it is rewritten */
    sc_ipc_i_release();
    sc_ipc_i_st32r(&h->version, SC_IPC_VERSION);
    sc_ipc_i_st32r(&h->owner_pid, owner_pid);
    sc_ipc_i_st32r(&h->peer_pid, 0);
    sc_ipc_i_st64r(&h->epoch, epoch);
    sc_ipc_i_st64r(&h->owner_heartbeat_ms, now_ms);
    sc_ipc_i_st64r(&h->bytes, map_bytes);
    sc_ipc_i_st64r(&h->peer_heartbeat_ms, 0);
    sc_ipc_i_st32r(&h->layout_id, layout_id);
    sc_ipc_i_st32r(&h->layout_version, layout_version);
    sc_ipc_i_st32r(&h->state, SC_IPC_STATE_OPEN);
    sc_ipc_i_st32r(&h->reserved, 0);
    sc_ipc_i_st32(&h->magic, SC_IPC_MAGIC);
    out->base = (uint8_t*)base;
    out->bytes = map_bytes;
    out->epoch = epoch;
    return SC_IPC_OK;
}

/* Peer: validates the header of a view of view_bytes (the size the OS reports for the mapped
 * view, never a size read from the mapping) and fills *out. SC_IPC_NOT_READY: not initialized
 * yet; SC_IPC_MISMATCH: another wire version or layout; SC_IPC_CORRUPT: hdr.bytes doesn't fit
 * the view; SC_IPC_GONE: closed; SC_IPC_BUSY: being re-initialized right now. */
static inline int sc_ipc_attach(void* base, uint64_t view_bytes, uint32_t layout_id, uint32_t layout_version,
                                sc_ipc_chan* out) {
    sc_ipc_hdr* h = (sc_ipc_hdr*)base;
    uint64_t bytes, epoch;
    if (!base || !out || view_bytes < SC_IPC_HEADER_BYTES || ((uintptr_t)base & 63u) != 0) return SC_IPC_BAD_ARG;
    if (sc_ipc_i_ld32(&h->magic) != SC_IPC_MAGIC) return SC_IPC_NOT_READY;
    epoch = sc_ipc_i_ld64r(&h->epoch);
    bytes = sc_ipc_i_ld64r(&h->bytes);
    if (sc_ipc_i_ld32r(&h->version) != SC_IPC_VERSION) return SC_IPC_MISMATCH;
    if (bytes < SC_IPC_HEADER_BYTES || bytes > view_bytes) return SC_IPC_CORRUPT;
    if (sc_ipc_i_ld32r(&h->layout_id) != layout_id || sc_ipc_i_ld32r(&h->layout_version) != layout_version)
        return SC_IPC_MISMATCH;
    if (sc_ipc_i_ld32r(&h->state) != SC_IPC_STATE_OPEN) return SC_IPC_GONE;
    sc_ipc_i_acquire();
    if (sc_ipc_i_ld32r(&h->magic) != SC_IPC_MAGIC || sc_ipc_i_ld64r(&h->epoch) != epoch) return SC_IPC_BUSY;
    out->base = (uint8_t*)base;
    out->bytes = bytes;
    out->epoch = epoch;
    return SC_IPC_OK;
}

/* Either side: SC_IPC_OK while the channel is the one this view attached to and open;
 * SC_IPC_EPOCH once re-initialized (attach again); SC_IPC_GONE once closed. */
static inline int sc_ipc_check(const sc_ipc_chan* c) {
    sc_ipc_hdr* h;
    if (!c || !c->base) return SC_IPC_BAD_ARG;
    h = sc_ipc_i_hdr(c);
    if (sc_ipc_i_ld32(&h->magic) != SC_IPC_MAGIC || sc_ipc_i_ld64r(&h->epoch) != c->epoch) return SC_IPC_EPOCH;
    if (sc_ipc_i_ld32r(&h->state) != SC_IPC_STATE_OPEN) return SC_IPC_GONE;
    return SC_IPC_OK;
}

/* Owner: marks the channel closed, so the peer sees SC_IPC_GONE before the heartbeat times out. */
static inline void sc_ipc_close(const sc_ipc_chan* c) {
    if (c && c->base) sc_ipc_i_st32(&sc_ipc_i_hdr(c)->state, SC_IPC_STATE_CLOSED);
}

static inline void sc_ipc_owner_beat(const sc_ipc_chan* c, uint64_t now_ms) {
    if (c && c->base) sc_ipc_i_st64(&sc_ipc_i_hdr(c)->owner_heartbeat_ms, now_ms);
}

static inline void sc_ipc_peer_beat(const sc_ipc_chan* c, uint32_t peer_pid, uint64_t now_ms) {
    if (!c || !c->base) return;
    sc_ipc_i_st32r(&sc_ipc_i_hdr(c)->peer_pid, peer_pid);
    sc_ipc_i_st64(&sc_ipc_i_hdr(c)->peer_heartbeat_ms, now_ms);
}

/* Milliseconds since beat_ms, capped at UINT32_MAX; a beat in the future (a clock the other side
 * got wrong, or a hostile value) reads as 0. SC_IPC_NOT_READY: never beaten (0). */
static inline int sc_ipc_age(uint64_t beat_ms, uint64_t now_ms, uint32_t* out_ms) {
    uint64_t age;
    if (!out_ms) return SC_IPC_BAD_ARG;
    *out_ms = 0;
    if (beat_ms == 0) return SC_IPC_NOT_READY;
    age = now_ms > beat_ms ? now_ms - beat_ms : 0;
    *out_ms = age > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)age;
    return SC_IPC_OK;
}

static inline int sc_ipc_peer_age_ms(const sc_ipc_chan* c, uint64_t now_ms, uint32_t* out_ms) {
    if (!c || !c->base) return SC_IPC_BAD_ARG;
    return sc_ipc_age(sc_ipc_i_ld64(&sc_ipc_i_hdr(c)->peer_heartbeat_ms), now_ms, out_ms);
}

static inline int sc_ipc_owner_age_ms(const sc_ipc_chan* c, uint64_t now_ms, uint32_t* out_ms) {
    if (!c || !c->base) return SC_IPC_BAD_ARG;
    return sc_ipc_age(sc_ipc_i_ld64(&sc_ipc_i_hdr(c)->owner_heartbeat_ms), now_ms, out_ms);
}

/* ---- seqlock blocks ---------------------------------------------------------------------------- */

/* Writes a snapshot of size bytes into the block at offset (a multiple of 8, past the header).
 * Never blocks. One writer per block. */
static inline int sc_ipc_block_write(const sc_ipc_chan* c, uint64_t offset, const void* data, uint32_t size) {
    sc_ipc_block* b;
    uint64_t seq;
    if (!c || !c->base || (!data && size) || (offset & 7u) != 0) return SC_IPC_BAD_ARG;
    if (!sc_ipc_i_inside(c->bytes, SC_IPC_HEADER_BYTES, offset, (uint64_t)SC_IPC_BLOCK_BYTES + size)) return SC_IPC_BAD_ARG;
    b = (sc_ipc_block*)(void*)(c->base + offset);
    seq = sc_ipc_i_ld64r(&b->seq) | 1u; /* odd: being written (an odd value left by a dead writer stays odd) */
    if (seq == 0xFFFFFFFFFFFFFFFFull) seq = 1;
    sc_ipc_i_st64r(&b->seq, seq);
    sc_ipc_i_release();
    sc_ipc_i_st32r(&b->size, size);
    sc_ipc_i_copy_in(c->base + offset + SC_IPC_BLOCK_BYTES, data, size);
    sc_ipc_i_st64(&b->seq, seq + 1u);
    return SC_IPC_OK;
}

/* Reads a snapshot of exactly size bytes into out. SC_IPC_BUSY: torn (being written); retry.
 * SC_IPC_NOT_READY: never written. SC_IPC_MISMATCH: the writer's snapshot has another size.
 * Unless SC_IPC_OK, out holds nothing usable. Never blocks. */
static inline int sc_ipc_block_read(const sc_ipc_chan* c, uint64_t offset, void* out, uint32_t size) {
    sc_ipc_block* b;
    uint64_t s1, s2;
    uint32_t stored;
    if (!c || !c->base || (!out && size) || (offset & 7u) != 0) return SC_IPC_BAD_ARG;
    if (!sc_ipc_i_inside(c->bytes, SC_IPC_HEADER_BYTES, offset, (uint64_t)SC_IPC_BLOCK_BYTES + size)) return SC_IPC_BAD_ARG;
    b = (sc_ipc_block*)(void*)(c->base + offset);
    s1 = sc_ipc_i_ld64(&b->seq);
    if (s1 == 0) return SC_IPC_NOT_READY;
    if (s1 & 1u) return SC_IPC_BUSY;
    stored = sc_ipc_i_ld32r(&b->size);
    sc_ipc_i_copy_out(out, c->base + offset + SC_IPC_BLOCK_BYTES, size);
    sc_ipc_i_acquire();
    s2 = sc_ipc_i_ld64r(&b->seq);
    if (s1 != s2) return SC_IPC_BUSY;
    if (stored != size) return SC_IPC_MISMATCH;
    return SC_IPC_OK;
}

/* ---- rings --------------------------------------------------------------------------------------- */

/* Owner: lays out an empty ring at offset (a multiple of 64, past the header) with capacity data
 * bytes (a power of two, at least SC_IPC_RING_MIN) and fills *out. Resets a ring already there. */
static inline int sc_ipc_ring_init(const sc_ipc_chan* c, uint64_t offset, uint64_t capacity, uint32_t direction,
                                   sc_ipc_ring_view* out) {
    sc_ipc_ring* r;
    if (!c || !c->base || !out || (offset & 63u) != 0) return SC_IPC_BAD_ARG;
    if (direction != SC_IPC_TO_PEER && direction != SC_IPC_FROM_PEER) return SC_IPC_BAD_ARG;
    if (!sc_ipc_i_pow2(capacity) || capacity < SC_IPC_RING_MIN || capacity > c->bytes) return SC_IPC_BAD_ARG;
    if (!sc_ipc_i_inside(c->bytes, SC_IPC_HEADER_BYTES, offset, SC_IPC_RING_BYTES + capacity)) return SC_IPC_BAD_ARG;
    r = (sc_ipc_ring*)(void*)(c->base + offset);
    sc_ipc_i_st32(&r->magic, 0);
    sc_ipc_i_release();
    sc_ipc_i_st32r(&r->direction, direction);
    sc_ipc_i_st64r(&r->capacity, capacity);
    sc_ipc_i_st64r(&r->epoch, c->epoch);
    sc_ipc_i_st64r(&r->head, 0);
    sc_ipc_i_st64r(&r->pushed, 0);
    sc_ipc_i_st64r(&r->tail, 0);
    sc_ipc_i_st64r(&r->popped, 0);
    sc_ipc_i_st32(&r->magic, SC_IPC_RING_MAGIC);
    out->hdr = sc_ipc_i_hdr(c);
    out->ring = r;
    out->data = c->base + offset + SC_IPC_RING_BYTES;
    out->capacity = capacity;
    out->epoch = c->epoch;
    out->direction = direction;
    out->reserved = 0;
    return SC_IPC_OK;
}

/* Either side: validates the ring at offset and fills *out with its geometry, cached from here
 * on. SC_IPC_NOT_READY: not laid out yet; SC_IPC_EPOCH: laid out under another epoch;
 * SC_IPC_CORRUPT: a capacity or direction that can't be right for this mapping. */
static inline int sc_ipc_ring_attach(const sc_ipc_chan* c, uint64_t offset, sc_ipc_ring_view* out) {
    sc_ipc_ring* r;
    uint64_t capacity, epoch;
    uint32_t direction;
    if (!c || !c->base || !out || (offset & 63u) != 0) return SC_IPC_BAD_ARG;
    if (!sc_ipc_i_inside(c->bytes, SC_IPC_HEADER_BYTES, offset, SC_IPC_RING_BYTES)) return SC_IPC_BAD_ARG;
    r = (sc_ipc_ring*)(void*)(c->base + offset);
    if (sc_ipc_i_ld32(&r->magic) != SC_IPC_RING_MAGIC) return SC_IPC_NOT_READY;
    capacity = sc_ipc_i_ld64r(&r->capacity);
    direction = sc_ipc_i_ld32r(&r->direction);
    epoch = sc_ipc_i_ld64r(&r->epoch);
    if (!sc_ipc_i_pow2(capacity) || capacity < SC_IPC_RING_MIN || capacity > c->bytes ||
        !sc_ipc_i_inside(c->bytes, SC_IPC_HEADER_BYTES, offset, SC_IPC_RING_BYTES + capacity))
        return SC_IPC_CORRUPT;
    if (direction != SC_IPC_TO_PEER && direction != SC_IPC_FROM_PEER) return SC_IPC_CORRUPT;
    if (epoch != c->epoch) return SC_IPC_EPOCH;
    out->hdr = sc_ipc_i_hdr(c);
    out->ring = r;
    out->data = c->base + offset + SC_IPC_RING_BYTES;
    out->capacity = capacity;
    out->epoch = epoch;
    out->direction = direction;
    out->reserved = 0;
    return SC_IPC_OK;
}

/* The channel and the ring are still the ones the view attached to. */
static inline int sc_ipc_i_ring_live(const sc_ipc_ring_view* v) {
    if (sc_ipc_i_ld32(&v->hdr->magic) != SC_IPC_MAGIC || sc_ipc_i_ld64r(&v->hdr->epoch) != v->epoch) return SC_IPC_EPOCH;
    if (sc_ipc_i_ld32r(&v->hdr->state) != SC_IPC_STATE_OPEN) return SC_IPC_GONE;
    if (sc_ipc_i_ld32(&v->ring->magic) != SC_IPC_RING_MAGIC || sc_ipc_i_ld64r(&v->ring->epoch) != v->epoch)
        return SC_IPC_EPOCH;
    return SC_IPC_OK;
}

/* Producer: appends one record (type anything but SC_IPC_REC_PAD, size bytes). SC_IPC_FULL: no
 * room until the consumer pops; SC_IPC_BAD_ARG: a record this ring can never hold
 * (SC_IPC_REC_BYTES + size rounded up to 8 must not exceed the capacity). Never blocks. */
static inline int sc_ipc_ring_push(sc_ipc_ring_view* v, uint32_t type, const void* data, uint32_t size) {
    sc_ipc_rec rec;
    uint64_t need, head, tail, used, off, contig;
    int live;
    if (!v || !v->ring || (!data && size) || type == SC_IPC_REC_PAD) return SC_IPC_BAD_ARG;
    need = SC_IPC_REC_BYTES + sc_ipc_i_align8(size);
    if (need > v->capacity) return SC_IPC_BAD_ARG;
    live = sc_ipc_i_ring_live(v);
    if (live != SC_IPC_OK) return live;
    head = sc_ipc_i_ld64r(&v->ring->head);
    tail = sc_ipc_i_ld64(&v->ring->tail);
    used = head - tail;
    if (((head | tail) & 7u) != 0 || used > v->capacity) return SC_IPC_CORRUPT;
    off = head & (v->capacity - 1u);
    contig = v->capacity - off;
    if (need > contig) { /* doesn't fit before the end: pad the end, start over at 0 */
        if (used + contig > v->capacity) return SC_IPC_FULL;
        rec.size = (uint32_t)(contig - SC_IPC_REC_BYTES);
        rec.type = SC_IPC_REC_PAD;
        sc_ipc_i_copy_in(v->data + off, &rec, sizeof rec);
        head += contig;
        used += contig;
        off = 0;
        sc_ipc_i_st64(&v->ring->head, head);
    }
    if (used + need > v->capacity) return SC_IPC_FULL;
    rec.size = size;
    rec.type = type;
    sc_ipc_i_copy_in(v->data + off, &rec, sizeof rec);
    sc_ipc_i_copy_in(v->data + off + SC_IPC_REC_BYTES, data, size);
    sc_ipc_i_st64r(&v->ring->pushed, sc_ipc_i_ld64r(&v->ring->pushed) + 1u);
    sc_ipc_i_st64(&v->ring->head, head + need);
    return SC_IPC_OK;
}

/* Consumer: takes the oldest record. *inout_size holds out's capacity on entry and the record's
 * size on return; with SC_IPC_TOO_SMALL it holds the size needed and the record stays queued
 * (pass out NULL with *inout_size 0 to ask). out_type may be NULL. SC_IPC_EMPTY: nothing queued.
 * SC_IPC_CORRUPT: the producer wrote an impossible record or counter; nothing is read. */
static inline int sc_ipc_ring_pop(sc_ipc_ring_view* v, uint32_t* out_type, void* out, uint32_t* inout_size) {
    sc_ipc_rec rec;
    uint64_t need, head, tail, used, off, contig;
    int live;
    if (!v || !v->ring || !inout_size || (!out && *inout_size)) return SC_IPC_BAD_ARG;
    live = sc_ipc_i_ring_live(v);
    if (live != SC_IPC_OK) return live;
    tail = sc_ipc_i_ld64r(&v->ring->tail);
    head = sc_ipc_i_ld64(&v->ring->head);
    used = head - tail;
    if (((head | tail) & 7u) != 0 || used > v->capacity) return SC_IPC_CORRUPT;
    if (used == 0) return SC_IPC_EMPTY;
    off = tail & (v->capacity - 1u);
    contig = v->capacity - off;
    sc_ipc_i_copy_out(&rec, v->data + off, sizeof rec); /* read once: the peer may rewrite it */
    if (rec.type == SC_IPC_REC_PAD) {
        if (contig > used || rec.size != contig - SC_IPC_REC_BYTES) return SC_IPC_CORRUPT;
        tail += contig;
        used -= contig;
        sc_ipc_i_st64(&v->ring->tail, tail);
        if (used == 0) return SC_IPC_EMPTY;
        off = 0;
        contig = v->capacity;
        sc_ipc_i_copy_out(&rec, v->data, sizeof rec);
        if (rec.type == SC_IPC_REC_PAD) return SC_IPC_CORRUPT;
    }
    need = SC_IPC_REC_BYTES + sc_ipc_i_align8(rec.size);
    if (need > used || need > contig) return SC_IPC_CORRUPT;
    if (rec.size > *inout_size) {
        *inout_size = rec.size;
        return SC_IPC_TOO_SMALL;
    }
    sc_ipc_i_copy_out(out, v->data + off + SC_IPC_REC_BYTES, rec.size);
    if (out_type) *out_type = rec.type;
    *inout_size = rec.size;
    sc_ipc_i_st64r(&v->ring->popped, sc_ipc_i_ld64r(&v->ring->popped) + 1u);
    sc_ipc_i_st64(&v->ring->tail, tail + need);
    return SC_IPC_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* SC_IPC_H */
