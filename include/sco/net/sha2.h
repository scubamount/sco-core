/* sco::net crypto: SHA-256 (FIPS 180-4), HMAC-SHA-256 (RFC 2104 / 4231) and PBKDF2-HMAC-SHA256
 * (RFC 8018), plus a constant-time compare. Plain C11 (src/net/sha2.c), no OpenSSL, no platform
 * code; written for sco-core and checked against the published vectors in tests/test_net.cpp.
 * Internal to sco-core (the session layer of sco.net); not part of any plugin ABI. */
#ifndef SCO_NET_SHA2_H
#define SCO_NET_SHA2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCO_SHA256_BYTES 32u
#define SCO_SHA256_BLOCK 64u

typedef struct sco_sha256 {
    uint32_t h[8];
    uint64_t total;      /* bytes hashed so far */
    uint8_t  buf[64];
    uint32_t used;       /* bytes in buf */
} sco_sha256;

void sco_sha256_init(sco_sha256* c);
void sco_sha256_update(sco_sha256* c, const void* data, size_t len);
void sco_sha256_final(sco_sha256* c, uint8_t out[32]);
void sco_sha256_digest(const void* data, size_t len, uint8_t out[32]);

/* HMAC-SHA-256. init keeps the keyed inner and outer states, so one init serves many messages:
 * copy the struct, update, final. Keys longer than a block are hashed first (RFC 2104). */
typedef struct sco_hmac_sha256 {
    sco_sha256 inner;
    sco_sha256 outer;
} sco_hmac_sha256;

void sco_hmac_sha256_init(sco_hmac_sha256* c, const void* key, size_t keyLen);
void sco_hmac_sha256_update(sco_hmac_sha256* c, const void* data, size_t len);
void sco_hmac_sha256_final(sco_hmac_sha256* c, uint8_t out[32]);
void sco_hmac_sha256_mac(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[32]);

/* PBKDF2-HMAC-SHA256 (RFC 8018 section 5.2). 1 on success; 0 for iterations 0, outLen 0, a NULL
 * output, or outLen over (2^32 - 1) * 32. */
int sco_pbkdf2_hmac_sha256(const void* pass, size_t passLen, const void* salt, size_t saltLen,
                           uint32_t iterations, uint8_t* out, size_t outLen);

/* 1 when the n bytes at a and b are equal, else 0. The time depends on n only, never on where
 * the bytes differ (for MAC tags). */
int sco_ct_equal(const void* a, const void* b, size_t n);

/* Overwrites n bytes with zeros in a way the compiler doesn't drop (keys, passphrases). */
void sco_wipe(void* p, size_t n);

#ifdef __cplusplus
}
#endif

#endif
