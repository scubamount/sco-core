/* SHA-256, HMAC-SHA-256, PBKDF2-HMAC-SHA256 and a constant-time compare (sco/net/sha2.h).
 * Straight from FIPS 180-4 section 6.2 and RFC 2104 / RFC 8018; bytes are assembled one at a
 * time, so nothing depends on alignment or byte order. */
#include "sco/net/sha2.h"

#include <string.h>

static const uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t Rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void Compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
               ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    for (unsigned i = 16; i < 64; ++i) {
        const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t t1 = k + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void sco_sha256_init(sco_sha256* c) {
    static const uint32_t kInit[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(c->h, kInit, sizeof(kInit));
    c->total = 0;
    c->used = 0;
    memset(c->buf, 0, sizeof(c->buf));
}

void sco_sha256_update(sco_sha256* c, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    c->total += (uint64_t)len;
    if (c->used) {
        size_t take = 64u - c->used;
        if (take > len) take = len;
        memcpy(c->buf + c->used, p, take);
        c->used += (uint32_t)take;
        p += take;
        len -= take;
        if (c->used < 64u) return;
        Compress(c->h, c->buf);
        c->used = 0;
    }
    while (len >= 64u) {
        Compress(c->h, p);
        p += 64;
        len -= 64u;
    }
    if (len) {
        memcpy(c->buf, p, len);
        c->used = (uint32_t)len;
    }
}

void sco_sha256_final(sco_sha256* c, uint8_t out[32]) {
    const uint64_t bits = c->total * 8u;
    c->buf[c->used++] = 0x80u;
    if (c->used > 56u) {
        memset(c->buf + c->used, 0, 64u - c->used);
        Compress(c->h, c->buf);
        c->used = 0;
    }
    memset(c->buf + c->used, 0, 56u - c->used);
    for (unsigned i = 0; i < 8; ++i) c->buf[56 + i] = (uint8_t)(bits >> (56u - 8u * i));
    Compress(c->h, c->buf);
    for (unsigned i = 0; i < 8; ++i) {
        out[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
    sco_wipe(c, sizeof(*c));
}

void sco_sha256_digest(const void* data, size_t len, uint8_t out[32]) {
    sco_sha256 c;
    sco_sha256_init(&c);
    sco_sha256_update(&c, data, len);
    sco_sha256_final(&c, out);
}

void sco_hmac_sha256_init(sco_hmac_sha256* c, const void* key, size_t keyLen) {
    uint8_t k[64];
    uint8_t pad[64];
    memset(k, 0, sizeof(k));
    if (keyLen > 64u) {
        sco_sha256_digest(key, keyLen, k);
    } else if (keyLen) {
        memcpy(k, key, keyLen);
    }
    for (unsigned i = 0; i < 64; ++i) pad[i] = (uint8_t)(k[i] ^ 0x36u);
    sco_sha256_init(&c->inner);
    sco_sha256_update(&c->inner, pad, 64);
    for (unsigned i = 0; i < 64; ++i) pad[i] = (uint8_t)(k[i] ^ 0x5cu);
    sco_sha256_init(&c->outer);
    sco_sha256_update(&c->outer, pad, 64);
    sco_wipe(k, sizeof(k));
    sco_wipe(pad, sizeof(pad));
}

void sco_hmac_sha256_update(sco_hmac_sha256* c, const void* data, size_t len) {
    sco_sha256_update(&c->inner, data, len);
}

void sco_hmac_sha256_final(sco_hmac_sha256* c, uint8_t out[32]) {
    uint8_t ih[32];
    sco_sha256_final(&c->inner, ih);
    sco_sha256_update(&c->outer, ih, sizeof(ih));
    sco_sha256_final(&c->outer, out);
    sco_wipe(ih, sizeof(ih));
}

void sco_hmac_sha256_mac(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[32]) {
    sco_hmac_sha256 c;
    sco_hmac_sha256_init(&c, key, keyLen);
    sco_hmac_sha256_update(&c, data, len);
    sco_hmac_sha256_final(&c, out);
}

int sco_pbkdf2_hmac_sha256(const void* pass, size_t passLen, const void* salt, size_t saltLen,
                           uint32_t iterations, uint8_t* out, size_t outLen) {
    if (!out || outLen == 0 || iterations == 0) return 0;
    if ((uint64_t)outLen > 0xFFFFFFFFull * 32u) return 0;
    /* The keyed states are computed once; each iteration then costs two compressions. */
    sco_hmac_sha256 keyed;
    sco_hmac_sha256_init(&keyed, pass, passLen);
    uint32_t block = 1;
    size_t done = 0;
    while (done < outLen) {
        uint8_t be[4];
        uint8_t u[32], t[32];
        be[0] = (uint8_t)(block >> 24);
        be[1] = (uint8_t)(block >> 16);
        be[2] = (uint8_t)(block >> 8);
        be[3] = (uint8_t)block;
        sco_hmac_sha256 c = keyed;
        sco_hmac_sha256_update(&c, salt, saltLen);
        sco_hmac_sha256_update(&c, be, sizeof(be));
        sco_hmac_sha256_final(&c, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; ++i) {
            c = keyed;
            sco_hmac_sha256_update(&c, u, sizeof(u));
            sco_hmac_sha256_final(&c, u);
            for (unsigned j = 0; j < 32; ++j) t[j] ^= u[j];
        }
        size_t take = outLen - done;
        if (take > 32u) take = 32u;
        memcpy(out + done, t, take);
        done += take;
        ++block;
        sco_wipe(u, sizeof(u));
        sco_wipe(t, sizeof(t));
    }
    sco_wipe(&keyed, sizeof(keyed));
    return 1;
}

int sco_ct_equal(const void* a, const void* b, size_t n) {
    const volatile uint8_t* x = (const volatile uint8_t*)a;
    const volatile uint8_t* y = (const volatile uint8_t*)b;
    uint32_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= (uint32_t)(x[i] ^ y[i]);
    /* 1 when diff == 0, without a data-dependent branch. */
    return (int)(((diff - 1u) >> 8) & 1u);
}

void sco_wipe(void* p, size_t n) {
    volatile uint8_t* v = (volatile uint8_t*)p;
    for (size_t i = 0; i < n; ++i) v[i] = 0;
}
