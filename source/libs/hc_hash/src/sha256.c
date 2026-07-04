/* sha256.c — SHA-256 (FIPS 180-4): the block compression function + an incremental core (init/update/final,
 * declared in sha256_core.h) + the one-shot + hex helpers of hc_hash.h.
 *
 * Purpose:   the message-schedule + compression of SHA-256, plus the buffering that drives it incrementally;
 *            hc_sha256 is init->update(once)->final, so there is ONE buffering path shared with HMAC.
 * Owns:      nothing — all running state lives in the caller's hc_sha256_ctx; the one-shot uses a stack ctx.
 * Threading: reentrant; no shared/global mutable state.
 * Lifetime:  digests are written to caller-provided buffers; no allocation.
 * Verified:  the FIPS vectors (empty, "abc", the 56-byte two-block boundary) + the RFC 4231 HMAC vectors that
 *            drive the streaming core, in tests/test_hc_hash.c. */

#include "hc_hash.h"
#include "sha256_core.h"

#include <stdint.h>
#include <string.h>

/* round constants: first 32 bits of the fractional parts of the cube roots of the first 64 primes */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_block(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 |
               (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++)
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

/* ---- the incremental core (sha256_core.h) ---- */

void hc_sha256_init(hc_sha256_ctx *c)
{
    /* init: first 32 bits of the fractional parts of the square roots of the first 8 primes */
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->blen = 0;
}

void hc_sha256_update(hc_sha256_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data; /* len==0 => p never dereferenced (NULL ok) */
    c->total += len;
    /* top off a partial block first */
    if (c->blen) {
        while (len && c->blen < 64) {
            c->buf[c->blen++] = *p++;
            len--;
        }
        if (c->blen == 64) {
            sha256_block(c->h, c->buf);
            c->blen = 0;
        }
    }
    /* whole blocks straight from the input */
    while (len >= 64) {
        sha256_block(c->h, p);
        p += 64;
        len -= 64;
    }
    /* stash the tail (blen ends in [0,63]) */
    while (len) {
        c->buf[c->blen++] = *p++;
        len--;
    }
}

void hc_sha256_final(hc_sha256_ctx *c, unsigned char out[32])
{
    uint64_t bits = c->total * 8; /* the length is taken BEFORE the padding is appended */
    /* append 0x80, then zeros to the 56-byte mark, then the 64-bit big-endian bit length. If the 0x80
     * leaves no room for the length (blen > 56), pad this block out, compress it, and use a fresh one. */
    c->buf[c->blen++] = 0x80; /* blen was <= 63 after update, so this index is in-bounds (-> [1,64]) */
    if (c->blen > 56) {
        while (c->blen < 64) c->buf[c->blen++] = 0;
        sha256_block(c->h, c->buf);
        c->blen = 0;
    }
    while (c->blen < 56) c->buf[c->blen++] = 0;
    for (int i = 0; i < 8; i++) c->buf[56 + i] = (unsigned char)(bits >> (8 * (7 - i)));
    sha256_block(c->h, c->buf);

    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}

/* ---- the public one-shot + hex helpers (hc_hash.h) ---- */

void hc_sha256(const void *data, size_t len, unsigned char out[32])
{
    hc_sha256_ctx c;
    hc_sha256_init(&c);
    hc_sha256_update(&c, data, len);
    hc_sha256_final(&c, out);
}

void hc_sha256_hex(const unsigned char digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    out[64] = '\0';
}

void hc_sha256_hex_str(const void *data, size_t len, char out[HC_SHA256_HEX_LEN])
{
    unsigned char dg[32];
    hc_sha256(data, len, dg);
    hc_sha256_hex(dg, out);
}
