#ifndef HC_HASH_SHA256_CORE_H
#define HC_HASH_SHA256_CORE_H

/* hc_hash INTERNAL — the incremental SHA-256 core shared by the one-shot hc_sha256 (sha256.c) and
 * hc_hmac_sha256 (hmac.c). NOT a public header (it does not live under include/), so these symbols are
 * library-internal: they are hc_*-prefixed to avoid clashing within the static archive but are deliberately
 * absent from hc_hash.h, so the advertised ABI stays {hc_sha256, hc_sha256_hex*, hc_hmac_sha256} only.
 *
 * Purpose:   stream SHA-256 over a message in chunks (init, then update repeatedly, then final) so HMAC can
 *            hash (ipad || msg) and
 *            (opad || inner) WITHOUT a concatenation buffer (msg may be arbitrarily large), and so the one-shot
 *            is the SAME core driven init->update(once)->final — ONE buffering path, not two.
 * Owns:      nothing — all state is the caller-provided hc_sha256_ctx; the block compression fn is unchanged.
 * Threading: reentrant; a ctx is single-owner (no concurrent use of one ctx).
 * Lifetime:  the ctx is caller-allocated (stack is fine) and lives from init() to final(). */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t      h[8];    /* the running state                                  */
    uint64_t      total;   /* total message bytes absorbed (for the length pad)  */
    unsigned char buf[64]; /* the partial (not-yet-compressed) block             */
    size_t        blen;    /* valid bytes in buf, always in [0, 63] after update */
} hc_sha256_ctx;

void hc_sha256_init(hc_sha256_ctx *c);
void hc_sha256_update(hc_sha256_ctx *c, const void *data, size_t len);
void hc_sha256_final(hc_sha256_ctx *c, unsigned char out[32]);

#endif /* HC_HASH_SHA256_CORE_H */
