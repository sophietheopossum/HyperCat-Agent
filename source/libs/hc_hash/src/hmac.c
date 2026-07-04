/* hmac.c — HMAC-SHA-256 (RFC 2104) over the incremental SHA-256 core. See hc_hash.h.
 *
 * Purpose:   the keyed MAC H((K0^opad) || H((K0^ipad) || msg)) that signs + verifies the P09 capability
 *            tokens. It lives in hc_hash (not a new lib) because HMAC IS a SHA-256 construction and every
 *            consumer already links hc_hash — a one-function library would add a DAG node for no isolation.
 * Owns:      nothing — the 32-byte tag is written to a caller buffer; all scratch is on the stack.
 * Threading: reentrant; no shared state.
 * Lifetime:  the `key` is BORROWED for the call only (never stored/freed); every key-DERIVED intermediate
 *            (k0/ipad/opad/ctx/inner) is zeroized before return via a local non-elidable scrub — kept LOCAL
 *            (not hc_secrets) to honor hc_hash's dependency-free banner. The constant-time tag COMPARE is the
 *            CONSUMER's job (hc_caps); this routine only EMITS the tag, so no caller memcmp()s one here.
 * Verified:  the RFC 4231 vectors in tests/test_hc_hash.c. */

#include "hc_hash.h"
#include "sha256_core.h"

#include <stddef.h>
#include <string.h>

/* Non-elidable zero of the key-derived pads (they embed the key): a volatile function pointer to memset
 * defeats dead-store elimination without pulling in hc_secrets (mirroring hc_secrets_zero's discipline,
 * kept local to honor hc_hash's dependency-free banner). */
static void *(*const volatile hc_hash_memset_v)(void *, int, size_t) = memset;
static void  secure_zero(void *p, size_t n) { hc_hash_memset_v(p, 0, n); }

void hc_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len, unsigned char out[32])
{
    /* K0 = the key brought to the 64-byte block size: hashed if longer, zero-padded otherwise (RFC 2104). */
    unsigned char k0[64];
    secure_zero(k0, sizeof k0);
    if (key_len > 64)
        hc_sha256(key, key_len, k0); /* a 32-byte digest left-justified, the rest stays zero */
    else if (key_len && key)
        memcpy(k0, key, key_len);

    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (unsigned char)(k0[i] ^ 0x36);
        opad[i] = (unsigned char)(k0[i] ^ 0x5c);
    }

    /* inner = SHA256(ipad || msg) — streamed, so msg of any length needs no concatenation buffer */
    unsigned char inner[32];
    hc_sha256_ctx c;
    hc_sha256_init(&c);
    hc_sha256_update(&c, ipad, sizeof ipad);
    hc_sha256_update(&c, msg, msg_len);
    hc_sha256_final(&c, inner);

    /* HMAC = SHA256(opad || inner) */
    hc_sha256_init(&c);
    hc_sha256_update(&c, opad, sizeof opad);
    hc_sha256_update(&c, inner, sizeof inner);
    hc_sha256_final(&c, out);

    /* scrub every byte that is a function of the key */
    secure_zero(k0, sizeof k0);
    secure_zero(ipad, sizeof ipad);
    secure_zero(opad, sizeof opad);
    secure_zero(&c, sizeof c);
    secure_zero(inner, sizeof inner);
}
