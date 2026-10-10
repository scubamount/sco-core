/*
 * abi_sc_net.c: pins sc_net.h, the MIT wire of sco.net packets.
 *
 * Compile-only, like abi_ipc.c. A program outside sco-core frames and verifies packets with these
 * numbers, so any line failing here is a protocol change: it needs SC_NET_PROTOCOL_VERSION bumped
 * and docs/net-wire.md updated in the same change. The service table (sco_net.h, plan PR 4b) gets
 * its own pin, tests/abi_net.c.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; the CMake test abi_sc_net rebuilds it. tests/test_net.cpp checks the
 * helpers at run time against sco-core's own framing.
 */
#include <stddef.h>
#include <stdint.h>

#include "sc_net.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

/* ---- identity and limits ---- */
PIN(SC_NET_MAGIC == 0x4E4F4353u);
PIN(SC_NET_PROTOCOL_VERSION == 1u);
PIN(SC_NET_PBKDF2_ITERS == 200000u);
PIN(SC_NET_MAX_PEERS == 16u);
PIN(SC_NET_MAX_UNREL == 1200u);
PIN(SC_NET_MAX_RELIABLE == 262144u);
PIN(SC_NET_MAX_DATAGRAM == 1400u);
PIN(SC_NET_HEADER_BYTES == 28u);
PIN(SC_NET_TAG_BYTES == 16u);
PIN(SC_NET_MAX_BODY == 1356u);
PIN(SC_NET_MAX_FQN == 64u);
PIN(SC_NET_MAX_NAME == 64u);
PIN(SC_NET_MAX_CHANNELS == 256u);
PIN(SC_NET_REPLAY_WINDOW == 1024u);
PIN(SC_NET_KEY_BYTES == 32u);
PIN(SC_NET_SALT_BYTES == 16u);
PIN(SC_NET_NONCE_BYTES == 16u);
PIN(SC_NET_CONTROL_MAX == 65536u);
PIN(SC_NET_FRAG_BYTES == 1200u);
PIN(SC_NET_WINDOW == 64u);
PIN(SC_NET_MAC_HEAD_MAX == 88u);
PIN(sizeof(SC_NET_CONTROL_FQN) == 8);      /* "sco.net" */
PIN(sizeof(SC_NET_LABEL_PROOF) == 14);     /* hashed with the NUL */
PIN(sizeof(SC_NET_LABEL_WELCOME) == 16);
PIN(sizeof(SC_NET_LABEL_LINK) == 13);

/* ---- flags, kinds, codes ---- */
PIN(SC_NET_RELIABLE == 1u && SC_NET_FROM_HOST == 2u && SC_NET_TO_HOST == 4u);
PIN(SC_NET_HELLO == 1u && SC_NET_CHALLENGE == 2u && SC_NET_PROOF == 3u && SC_NET_WELCOME == 4u);
PIN(SC_NET_REFUSE == 5u && SC_NET_DATA == 6u && SC_NET_ACK == 7u && SC_NET_PING == 8u && SC_NET_BYE == 9u);
PIN(SC_NET_REFUSE_VERSION == 1u && SC_NET_REFUSE_PASSPHRASE == 2u && SC_NET_REFUSE_FULL == 3u);
PIN(SC_NET_REFUSE_NOT_ADMITTED == 4u && SC_NET_REFUSE_BAD_HELLO == 5u);
PIN(SC_NET_CTL_SYNC == 1u && SC_NET_CTL_CHANNEL == 2u && SC_NET_CTL_REGISTER == 3u);
PIN(SC_NET_CTL_PEER_JOINED == 4u && SC_NET_CTL_PEER_LEFT == 5u);
PIN(SC_NET_OK == 0 && SC_NET_E_SHORT == 1 && SC_NET_E_OVERSIZE == 2 && SC_NET_E_BAD_MAGIC == 3);
PIN(SC_NET_E_BAD_VERSION == 4 && SC_NET_E_BAD_KIND == 5 && SC_NET_E_TRUNCATED == 6 && SC_NET_E_TRAILING == 7);
PIN(SC_NET_REPLAY_NEW == 0 && SC_NET_REPLAY_DUPLICATE == 1 && SC_NET_REPLAY_TOO_OLD == 2);

/* ---- the 28-byte header on the wire ---- */
PIN(SC_NET_OFF_MAGIC == 0u && SC_NET_OFF_VERSION == 4u && SC_NET_OFF_KIND == 5u && SC_NET_OFF_CHANNEL == 6u);
PIN(SC_NET_OFF_SENDER == 8u && SC_NET_OFF_SEQ == 16u && SC_NET_OFF_BODY_LEN == 24u);
PIN(SC_NET_OFF_BODY_LEN + 4u == SC_NET_HEADER_BYTES);

/* ---- bodies ---- */
PIN(SC_NET_DATA_F_RELIABLE == 1u && SC_NET_DATA_F_TO_HOST == 2u);
PIN(SC_NET_DATA_HEAD_UNREL == 12u && SC_NET_DATA_HEAD_REL == 28u);
PIN(SC_NET_ACK_ENTRY_BYTES == 20u);
PIN(SC_NET_HELLO_MIN_BODY == 64u && SC_NET_CHALLENGE_BODY == 48u && SC_NET_PROOF_BODY == 64u);
PIN(SC_NET_WELCOME_BODY == 56u);

/* ---- the decoded structs (not wire layouts, but programs allocate them) ---- */
SIZE(sc_net_header, 32);
AT(sc_net_header, magic, 0);
AT(sc_net_header, version, 4);
AT(sc_net_header, kind, 5);
AT(sc_net_header, channel, 6);
AT(sc_net_header, sender, 8);
AT(sc_net_header, seq, 16);
AT(sc_net_header, body_len, 24);
AT(sc_net_header, _pad, 28);
SIZE(sc_net_replay, 136);
AT(sc_net_replay, top, 0);
AT(sc_net_replay, bits, 8);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_sc_net_signatures(void) {
    int (*parse)(const uint8_t*, size_t, sc_net_header*, const uint8_t**, const uint8_t**) = sc_net_parse;
    void (*write_header)(const sc_net_header*, uint8_t*) = sc_net_write_header;
    size_t (*mac_head)(const sc_net_header*, const char*, size_t, uint8_t*, size_t) = sc_net_mac_head;
    int (*tag)(sc_net_hmac2_fn, void*, const void*, const sc_net_header*, const char*, size_t, const uint8_t*,
               uint8_t*) = sc_net_tag;
    int (*tag_equal)(const uint8_t*, const uint8_t*) = sc_net_tag_equal;
    int (*replay_check)(const sc_net_replay*, uint64_t) = sc_net_replay_check;
    void (*replay_accept)(sc_net_replay*, uint64_t) = sc_net_replay_accept;
    int (*valid_fqn)(const char*, size_t) = sc_net_valid_fqn;
    int (*valid_name)(const char*, size_t) = sc_net_valid_name;
    int (*kind_tagged)(uint32_t) = sc_net_kind_tagged;
    void (*hmac)(void*, const void*, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) = (sc_net_hmac2_fn)0;
    (void)parse; (void)write_header; (void)mac_head; (void)tag; (void)tag_equal; (void)replay_check;
    (void)replay_accept; (void)valid_fqn; (void)valid_name; (void)kind_tagged; (void)hmac;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_sc_net_pins(void);
void sco_abi_sc_net_pins(void) { (void)&pin_sc_net_signatures; }
