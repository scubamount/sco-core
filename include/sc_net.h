/* SPDX-License-Identifier: MIT */
/*
 * sc_net.h: the datagram wire of sco.net sessions (docs/net-wire.md).
 *
 * MIT, unlike the rest of sco-core (GPL-3.0): this file is part of the interface exception named
 * in LICENSE and CONTRIBUTING.md, so a program that isn't GPL may include it and frame, parse and
 * verify sco.net packets. It has no sco-core dependency: plain C11 or C++20, freestanding headers
 * only, header-only. sco-core's own session code (include/sco/net/) takes its constants, parser,
 * MAC input and replay window from here, so this file is the one definition of the wire.
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
 * ---- The packet ------------------------------------------------------------------------------
 *
 * One UDP datagram, at most SC_NET_MAX_DATAGRAM (1,400) bytes. Integers are little-endian and
 * read byte by byte (never through a cast struct, so alignment and host byte order don't matter):
 *
 *   off  size  field
 *    0    4    magic "SCON" (bytes 53 43 4F 4E)              SC_NET_MAGIC
 *    4    1    protocol_version                              SC_NET_PROTOCOL_VERSION
 *    5    1    kind                                          SC_NET_HELLO .. SC_NET_BYE
 *    6    2    channel index in the session table (0 = the control channel "sco.net")
 *    8    8    sender_peer_id (the link's sender; 0 in the handshake)
 *   16    8    seq (per sender and link, monotonic from 1, never reused; 0 in the handshake)
 *   24    4    body_len, at most SC_NET_MAX_BODY (1,356)
 *   28    n    body
 *  28+n  16    tag (DATA, ACK, PING, BYE only): HMAC-SHA-256 under the link key, first 16 bytes
 *
 * ---- The MAC ---------------------------------------------------------------------------------
 *
 * tag = HMAC-SHA-256(link key, M)[0..16), where M is, in this order:
 *
 *   protocol_version (1) || kind (1) || u16 len(channel_fqn) || channel_fqn ||
 *   sender_peer_id (8) || seq (8) || body_len (4) || body
 *
 * channel_fqn is the full name ("<plugin id>.<channel>", "sco.net" for index 0) that the
 * packet's channel index names in the session table: the wire carries the index, the MAC binds
 * the name. The kind and the two lengths are in M so that no two different packets share an M.
 * sc_net_mac_head() writes M up to the body; sc_net_tag() runs it through an HMAC the caller
 * supplies (this header carries no crypto). Compare tags with sc_net_tag_equal (constant time).
 *
 * ---- Receiving -------------------------------------------------------------------------------
 *
 * sc_net_parse() checks the framing and reads nothing past len, whatever the bytes say. A
 * tagged packet is then accepted only if, in order: it came from the link's address with the
 * link's peer id; its channel index is in the table; the tag verifies; sc_net_replay_check()
 * says SC_NET_REPLAY_NEW. Only then call sc_net_replay_accept() and decode the body.
 *
 * Replay window: SC_NET_REPLAY_WINDOW (1,024) packets per link and direction. seq 0 is never
 * valid; a seq at or below highest - 1024 is too old; inside the window each seq once.
 *
 * ---- Keys (docs/net-wire.md has the handshake bodies) ----------------------------------------
 *
 *   K (session) = PBKDF2-HMAC-SHA256(passphrase, salt, SC_NET_PBKDF2_ITERS, 32)
 *   PROOF       = HMAC(K, "sco.net proof\0"   || client nonce || host nonce)
 *   WELCOME     = HMAC(K, "sco.net welcome\0" || client nonce || host nonce || u64 peer id)
 *   link key    = HMAC(K, "sco.net link\0"    || client nonce || host nonce)
 *
 * The labels are hashed with their terminating NUL (SC_NET_LABEL_* below, sizeof included).
 *
 * Results are int: SC_NET_OK (0) or one of the SC_NET_E_* codes below.
 */
#ifndef SC_NET_H
#define SC_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define SC_NET_STATIC_ASSERT(e, m) static_assert(e, m)
extern "C" {
#else
#define SC_NET_STATIC_ASSERT(e, m) _Static_assert(e, m)
#endif

/* ---- constants ------------------------------------------------------------------------------ */

#define SC_NET_MAGIC            0x4E4F4353u   /* "SCON" as little-endian bytes */
#define SC_NET_PROTOCOL_VERSION 1u
/* PBKDF2 iterations for K. Part of the protocol: changing it bumps SC_NET_PROTOCOL_VERSION. */
#define SC_NET_PBKDF2_ITERS     200000u

#define SC_NET_MAX_PEERS     16u              /* a session, the host included */
#define SC_NET_MAX_UNREL     1200u            /* bytes in one unreliable message */
#define SC_NET_MAX_RELIABLE  (256u * 1024u)   /* bytes in one reliable message */
#define SC_NET_MAX_DATAGRAM  1400u
#define SC_NET_HEADER_BYTES  28u
#define SC_NET_TAG_BYTES     16u
#define SC_NET_MAX_BODY      (SC_NET_MAX_DATAGRAM - SC_NET_HEADER_BYTES - SC_NET_TAG_BYTES)   /* 1356 */
#define SC_NET_MAX_FQN       64u              /* a channel name, bytes */
#define SC_NET_MAX_NAME      64u              /* a player name, UTF-8 bytes */
#define SC_NET_MAX_CHANNELS  256u             /* the session table, control included */
#define SC_NET_REPLAY_WINDOW 1024u            /* packets */
#define SC_NET_KEY_BYTES     32u
#define SC_NET_SALT_BYTES    16u
#define SC_NET_NONCE_BYTES   16u
#define SC_NET_CONTROL_FQN   "sco.net"        /* channel index 0 */
#define SC_NET_CONTROL_MAX   (64u * 1024u)    /* bytes in one control message */

/* Reliable streams: units of SC_NET_FRAG_BYTES, at most SC_NET_WINDOW in flight / buffered. */
#define SC_NET_FRAG_BYTES 1200u
#define SC_NET_WINDOW     64u

/* Channel flags (the register_channel flags of the sco.net service). */
#define SC_NET_RELIABLE  0x1u
#define SC_NET_FROM_HOST 0x2u
#define SC_NET_TO_HOST   0x4u

/* Kinds (byte 5). 1-5 are the handshake, untagged; 6-9 are tagged. */
#define SC_NET_HELLO     1u
#define SC_NET_CHALLENGE 2u
#define SC_NET_PROOF     3u
#define SC_NET_WELCOME   4u
#define SC_NET_REFUSE    5u
#define SC_NET_DATA      6u
#define SC_NET_ACK       7u
#define SC_NET_PING      8u
#define SC_NET_BYE       9u

/* Header field offsets. */
#define SC_NET_OFF_MAGIC    0u
#define SC_NET_OFF_VERSION  4u
#define SC_NET_OFF_KIND     5u
#define SC_NET_OFF_CHANNEL  6u
#define SC_NET_OFF_SENDER   8u
#define SC_NET_OFF_SEQ      16u
#define SC_NET_OFF_BODY_LEN 24u

/* DATA body: u8 flags, u8 0, u16 0, u64 origin peer id, then the data (unreliable), or u64 unit
 * seq, u32 message length, u32 unit offset, then the unit's data (reliable). */
#define SC_NET_DATA_F_RELIABLE 0x1u
#define SC_NET_DATA_F_TO_HOST  0x2u
#define SC_NET_DATA_HEAD_UNREL 12u
#define SC_NET_DATA_HEAD_REL   28u

/* ACK body: u16 count, then count entries of u16 channel, u16 0, u64 next expected unit, u64 mask
 * (bit i: unit next + 1 + i received). */
#define SC_NET_ACK_ENTRY_BYTES 20u

/* Handshake bodies (docs/net-wire.md). HELLO is padded to at least SC_NET_HELLO_MIN_BODY. */
#define SC_NET_HELLO_MIN_BODY  64u
#define SC_NET_CHALLENGE_BODY  48u   /* client nonce, host nonce, salt */
#define SC_NET_PROOF_BODY      64u   /* client nonce, host nonce, proof */
#define SC_NET_WELCOME_BODY    56u   /* client nonce, u64 peer id, proof */

/* REFUSE codes (body byte 16). */
#define SC_NET_REFUSE_VERSION      1u
#define SC_NET_REFUSE_PASSPHRASE   2u
#define SC_NET_REFUSE_FULL         3u
#define SC_NET_REFUSE_NOT_ADMITTED 4u
#define SC_NET_REFUSE_BAD_HELLO    5u

/* Control messages (the first body byte of a message on channel 0). */
#define SC_NET_CTL_SYNC        1u
#define SC_NET_CTL_CHANNEL     2u
#define SC_NET_CTL_REGISTER    3u
#define SC_NET_CTL_PEER_JOINED 4u
#define SC_NET_CTL_PEER_LEFT   5u

/* Key labels, hashed with their NUL: use sizeof(SC_NET_LABEL_x) bytes. */
#define SC_NET_LABEL_PROOF   "sco.net proof"
#define SC_NET_LABEL_WELCOME "sco.net welcome"
#define SC_NET_LABEL_LINK    "sco.net link"

/* The MAC input before the body: at most this many bytes (sc_net_mac_head). */
#define SC_NET_MAC_HEAD_MAX (24u + SC_NET_MAX_FQN)

/* Results of sc_net_parse. */
#define SC_NET_OK            0
#define SC_NET_E_SHORT       1   /* under SC_NET_HEADER_BYTES */
#define SC_NET_E_OVERSIZE    2   /* over SC_NET_MAX_DATAGRAM, or body_len over SC_NET_MAX_BODY */
#define SC_NET_E_BAD_MAGIC   3
#define SC_NET_E_BAD_VERSION 4   /* *out is still filled in */
#define SC_NET_E_BAD_KIND    5
#define SC_NET_E_TRUNCATED   6   /* shorter than header + body_len (+ tag) */
#define SC_NET_E_TRAILING    7   /* longer than that */

/* sc_net_replay_check results. */
#define SC_NET_REPLAY_NEW       0
#define SC_NET_REPLAY_DUPLICATE 1
#define SC_NET_REPLAY_TOO_OLD   2

/* ---- types ---------------------------------------------------------------------------------- */

/* A decoded header. This is not the wire layout (that is 28 packed bytes, above); read and write
 * it with sc_net_parse / sc_net_write_header. */
typedef struct sc_net_header {
    uint32_t magic;
    uint8_t  version;
    uint8_t  kind;
    uint16_t channel;
    uint64_t sender;
    uint64_t seq;
    uint32_t body_len;
    uint32_t _pad;
} sc_net_header;
SC_NET_STATIC_ASSERT(sizeof(sc_net_header) == 32, "sc_net_header");

/* A replay window. Zero it to start (no packet seen). */
typedef struct sc_net_replay {
    uint64_t top;                                 /* highest accepted seq, 0 = none */
    uint64_t bits[SC_NET_REPLAY_WINDOW / 64];     /* bit d: seq top - d was seen */
} sc_net_replay;
SC_NET_STATIC_ASSERT(sizeof(sc_net_replay) == 136, "sc_net_replay");

/* The HMAC-SHA-256 a program supplies: out = HMAC(key, a || b). key is whatever that function
 * needs (raw 32 bytes, or a precomputed keyed state); this header only passes it through. */
typedef void (*sc_net_hmac2_fn)(void* user, const void* key, const uint8_t* a, size_t a_len, const uint8_t* b,
                                size_t b_len, uint8_t out[32]);

/* ---- bytes ---------------------------------------------------------------------------------- */

static inline uint16_t sc_net_get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t sc_net_get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t sc_net_get64(const uint8_t* p) {
    return (uint64_t)sc_net_get32(p) | ((uint64_t)sc_net_get32(p + 4) << 32);
}
static inline void sc_net_put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void sc_net_put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u * i));
}
static inline void sc_net_put64(uint8_t* p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8u * i));
}

/* ---- framing -------------------------------------------------------------------------------- */

/* 1 for the kinds that carry a tag. */
static inline int sc_net_kind_tagged(uint32_t kind) { return kind >= SC_NET_DATA && kind <= SC_NET_BYE; }

/* Writes the 28 header bytes (the magic always SC_NET_MAGIC). */
static inline void sc_net_write_header(const sc_net_header* h, uint8_t out[SC_NET_HEADER_BYTES]) {
    sc_net_put32(out + SC_NET_OFF_MAGIC, SC_NET_MAGIC);
    out[SC_NET_OFF_VERSION] = h->version;
    out[SC_NET_OFF_KIND] = h->kind;
    sc_net_put16(out + SC_NET_OFF_CHANNEL, h->channel);
    sc_net_put64(out + SC_NET_OFF_SENDER, h->sender);
    sc_net_put64(out + SC_NET_OFF_SEQ, h->seq);
    sc_net_put32(out + SC_NET_OFF_BODY_LEN, h->body_len);
}

/* Checks the framing of len bytes at data; on SC_NET_OK sets *body (body_len bytes) and *tag
 * (SC_NET_TAG_BYTES, or NULL for an untagged kind), both pointing into data. Fills *out before
 * returning SC_NET_E_BAD_VERSION (only the first six bytes are stable across versions). */
static inline int sc_net_parse(const uint8_t* data, size_t len, sc_net_header* out, const uint8_t** body,
                               const uint8_t** tag) {
    uint64_t need;
    *body = NULL;
    *tag = NULL;
    if (!data || len < SC_NET_HEADER_BYTES) return SC_NET_E_SHORT;
    if (len > SC_NET_MAX_DATAGRAM) return SC_NET_E_OVERSIZE;
    if (sc_net_get32(data + SC_NET_OFF_MAGIC) != SC_NET_MAGIC) return SC_NET_E_BAD_MAGIC;
    out->magic = SC_NET_MAGIC;
    out->version = data[SC_NET_OFF_VERSION];
    out->kind = data[SC_NET_OFF_KIND];
    out->channel = sc_net_get16(data + SC_NET_OFF_CHANNEL);
    out->sender = sc_net_get64(data + SC_NET_OFF_SENDER);
    out->seq = sc_net_get64(data + SC_NET_OFF_SEQ);
    out->body_len = sc_net_get32(data + SC_NET_OFF_BODY_LEN);
    out->_pad = 0;
    if (out->version != SC_NET_PROTOCOL_VERSION) return SC_NET_E_BAD_VERSION;
    if (out->kind < SC_NET_HELLO || out->kind > SC_NET_BYE) return SC_NET_E_BAD_KIND;
    if (out->body_len > SC_NET_MAX_BODY) return SC_NET_E_OVERSIZE;
    need = (uint64_t)SC_NET_HEADER_BYTES + out->body_len + (sc_net_kind_tagged(out->kind) ? SC_NET_TAG_BYTES : 0u);
    if ((uint64_t)len < need) return SC_NET_E_TRUNCATED;
    if ((uint64_t)len > need) return SC_NET_E_TRAILING;
    *body = data + SC_NET_HEADER_BYTES;
    if (sc_net_kind_tagged(out->kind)) *tag = data + SC_NET_HEADER_BYTES + out->body_len;
    return SC_NET_OK;
}

/* ---- the MAC -------------------------------------------------------------------------------- */

/* Writes M up to the body (see the top of this file) for h under the channel name fqn into out;
 * returns its length (24 + fqn_len), or 0 when fqn_len is over SC_NET_MAX_FQN or cap too small. */
static inline size_t sc_net_mac_head(const sc_net_header* h, const char* fqn, size_t fqn_len, uint8_t* out,
                                     size_t cap) {
    size_t n = 0;
    if (fqn_len > SC_NET_MAX_FQN || cap < 24u + fqn_len) return 0;
    out[n++] = h->version;
    out[n++] = h->kind;
    sc_net_put16(out + n, (uint16_t)fqn_len);
    n += 2;
    for (size_t i = 0; i < fqn_len; ++i) out[n++] = (uint8_t)fqn[i];
    sc_net_put64(out + n, h->sender);
    n += 8;
    sc_net_put64(out + n, h->seq);
    n += 8;
    sc_net_put32(out + n, h->body_len);
    n += 4;
    return n;
}

/* tag = the first SC_NET_TAG_BYTES of hmac(key, M). 1, or 0 for a bad fqn length. */
static inline int sc_net_tag(sc_net_hmac2_fn hmac, void* user, const void* key, const sc_net_header* h,
                             const char* fqn, size_t fqn_len, const uint8_t* body, uint8_t tag[SC_NET_TAG_BYTES]) {
    uint8_t head[SC_NET_MAC_HEAD_MAX];
    uint8_t full[32];
    const size_t n = sc_net_mac_head(h, fqn, fqn_len, head, sizeof(head));
    if (!n) return 0;
    hmac(user, key, head, n, body, h->body_len, full);
    for (unsigned i = 0; i < SC_NET_TAG_BYTES; ++i) tag[i] = full[i];
    for (unsigned i = 0; i < sizeof(full); ++i) ((volatile uint8_t*)full)[i] = 0;
    return 1;
}

/* 1 when the two tags are equal; the time doesn't depend on where they differ. */
static inline int sc_net_tag_equal(const uint8_t a[SC_NET_TAG_BYTES], const uint8_t b[SC_NET_TAG_BYTES]) {
    const volatile uint8_t* x = a;
    const volatile uint8_t* y = b;
    uint32_t diff = 0;
    for (unsigned i = 0; i < SC_NET_TAG_BYTES; ++i) diff |= (uint32_t)(x[i] ^ y[i]);
    return (int)(((diff - 1u) >> 8) & 1u);
}

/* ---- the replay window ---------------------------------------------------------------------- */

static inline int sc_net_replay_check(const sc_net_replay* w, uint64_t seq) {
    uint64_t d;
    if (seq == 0) return SC_NET_REPLAY_TOO_OLD;
    if (seq > w->top) return SC_NET_REPLAY_NEW;
    d = w->top - seq;
    if (d >= SC_NET_REPLAY_WINDOW) return SC_NET_REPLAY_TOO_OLD;
    return ((w->bits[d / 64] >> (d % 64)) & 1u) ? SC_NET_REPLAY_DUPLICATE : SC_NET_REPLAY_NEW;
}

/* Records seq. Call only after sc_net_replay_check said SC_NET_REPLAY_NEW and the tag verified. */
static inline void sc_net_replay_accept(sc_net_replay* w, uint64_t seq) {
    const uint32_t words = SC_NET_REPLAY_WINDOW / 64;
    if (seq == 0) return;
    if (seq > w->top) {
        const uint64_t shift = seq - w->top;
        if (shift >= SC_NET_REPLAY_WINDOW) {
            for (uint32_t i = 0; i < words; ++i) w->bits[i] = 0;
        } else {
            const uint32_t ws = (uint32_t)(shift / 64), bs = (uint32_t)(shift % 64);
            for (uint32_t i = words; i-- > 0;) {
                uint64_t v = 0;
                if (i >= ws) {
                    v = w->bits[i - ws] << bs;
                    if (bs && i >= ws + 1) v |= w->bits[i - ws - 1] >> (64 - bs);
                }
                w->bits[i] = v;
            }
        }
        w->top = seq;
        w->bits[0] |= 1u;
        return;
    }
    {
        const uint64_t d = w->top - seq;
        if (d < SC_NET_REPLAY_WINDOW) w->bits[d / 64] |= (uint64_t)1 << (d % 64);
    }
}

/* ---- names ---------------------------------------------------------------------------------- */

/* A channel name: 3..SC_NET_MAX_FQN bytes of [A-Za-z0-9_-] in two or more dot-separated parts. */
static inline int sc_net_valid_fqn(const char* s, size_t n) {
    int dot = 0;
    char prev = '.';
    if (!s || n < 3 || n > SC_NET_MAX_FQN) return 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (c == '.') {
            if (prev == '.') return 0;
            dot = 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                     c == '-')) {
            return 0;
        }
        prev = c;
    }
    return dot && prev != '.';
}

/* A player name: 1..SC_NET_MAX_NAME bytes, no control characters (UTF-8 passes through). */
static inline int sc_net_valid_name(const char* s, size_t n) {
    if (!s || n == 0 || n > SC_NET_MAX_NAME) return 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char u = (unsigned char)s[i];
        if (u < 0x20u || u == 0x7Fu) return 0;
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
