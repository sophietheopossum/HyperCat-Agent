/* Tests for hc_hash — SHA-256 against the standard FIPS 180-4 vectors (empty, "abc", the 56-byte
 * boundary where padding spills to a second block), plus the one-shot hex_str convenience. Exit
 * non-zero on any failure. */

#include "hc_hash.h"

#include <stdio.h>
#include <string.h>

static int g_fails = 0;
#define CHECK(c, m)                                                                                  \
    do {                                                                                             \
        if (!(c)) {                                                                                  \
            fprintf(stderr, "FAIL: %s\n", (m));                                                      \
            g_fails++;                                                                                \
        }                                                                                            \
    } while (0)

static int hex_eq(const char *data, size_t len, const char *expect)
{
    char out[HC_SHA256_HEX_LEN];
    hc_sha256_hex_str(data, len, out);
    return strcmp(out, expect) == 0;
}

/* Compute HMAC-SHA-256(key, msg) and compare the lowercase-hex tag against `expect` (an RFC 4231 vector). */
static int hmac_hex_eq(const void *key, size_t kl, const void *msg, size_t ml, const char *expect)
{
    unsigned char tag[32];
    char          hex[HC_SHA256_HEX_LEN];
    hc_hmac_sha256(key, kl, msg, ml, tag);
    hc_sha256_hex(tag, hex);
    return strcmp(hex, expect) == 0;
}

int main(void)
{
    CHECK(hex_eq("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "sha256(\"\")");
    CHECK(hex_eq("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256(\"abc\")");
    /* the standard FIPS 180-4 two-block vector: a 56-byte message, so padding + the 64-bit length spill
     * into a SECOND block (the boundary the one-shot padding math must get right) */
    CHECK(hex_eq("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
          "sha256(56-byte two-block boundary)");
    /* hc_sha256 + hc_sha256_hex (the two-step) agrees with the one-shot */
    unsigned char dg[32];
    char          a[HC_SHA256_HEX_LEN], b[HC_SHA256_HEX_LEN];
    hc_sha256("hypercat", 8, dg);
    hc_sha256_hex(dg, a);
    hc_sha256_hex_str("hypercat", 8, b);
    CHECK(strcmp(a, b) == 0, "two-step == one-shot hex");

    /* ---- HMAC-SHA-256: the RFC 4231 vectors (prove the keyed MAC + exercise the streaming core, incl. the
     * >64-byte-key pre-hash path in TC6/TC7). These are the proof obligation for P09's capability signing. ---- */
    {
        unsigned char k[131], d[50];
        memset(k, 0x0b, 20); /* TC1 */
        CHECK(hmac_hex_eq(k, 20, "Hi There", 8,
                          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
              "HMAC RFC4231 TC1 (20-byte 0x0b key)");
        CHECK(hmac_hex_eq("Jefe", 4, "what do ya want for nothing?", 28, /* TC2 */
                          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
              "HMAC RFC4231 TC2 (\"Jefe\" key)");
        memset(k, 0xaa, 20); /* TC3 */
        memset(d, 0xdd, 50);
        CHECK(hmac_hex_eq(k, 20, d, 50,
                          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"),
              "HMAC RFC4231 TC3 (50-byte 0xdd data -> multi-block stream)");
        memset(k, 0xaa, 131); /* TC6 — key > block, pre-hashed */
        CHECK(hmac_hex_eq(k, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54,
                          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"),
              "HMAC RFC4231 TC6 (131-byte key pre-hash path)");
        CHECK(hmac_hex_eq(k, 131, /* TC7 — key > block AND data > block */
                          "This is a test using a larger than block-size key and a larger than block-size "
                          "data. The key needs to be hashed before being used by the HMAC algorithm.",
                          152, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"),
              "HMAC RFC4231 TC7 (131-byte key + 152-byte data)");
    }

    if (g_fails) {
        fprintf(stderr, "hc_hash: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_hash: all checks passed\n");
    return 0;
}
