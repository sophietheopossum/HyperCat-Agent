#ifndef HC_HASH_H
#define HC_HASH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_hash — vendored SHA-256 (FIPS 180-4) + HMAC-SHA-256 (RFC 2104), HyperCat's shared content-identity +
 * message-authentication primitive. Self-contained, dependency-free, hand-written per the "rebuild, don't
 * copy" rule.
 *
 * Purpose:   SHA-256 gives bytes a stable, collision-resistant IDENTITY (hc_artifacts object ids, hc_manifest
 *            turn hashes); HMAC-SHA-256 is the keyed MAC the P09 capability tokens sign + verify with. Both run
 *            on ONE incremental core (sha256_core.h, library-internal): the public one-shot is that core driven
 *            init->update(once)->final; HMAC streams the pad block + the message (no concat buffer, any length).
 * Owns:      nothing — every digest/tag is written to a caller-provided buffer; no allocation, no global state.
 * Threading: reentrant; all functions are stateless from the caller's view (scratch lives on the stack).
 * Lifetime:  inputs/outputs are caller-owned; in hc_hmac_sha256 the `key` is BORROWED for the call only and its
 *            derived intermediates are zeroized before return. The constant-time tag COMPARE is the CONSUMER's
 *            job (e.g. hc_caps), never done here. */

#define HC_SHA256_HEX_LEN 65 /* 64 lowercase-hex chars + NUL */

void hc_sha256(const void *data, size_t len, unsigned char out[32]);   /* 32 raw digest bytes        */
void hc_sha256_hex(const unsigned char digest[32], char out[65]);       /* 64 lowercase hex + NUL     */
/* One-shot convenience: hash `len` bytes straight to a lowercase-hex string (HC_SHA256_HEX_LEN incl. NUL). */
void hc_sha256_hex_str(const void *data, size_t len, char out[HC_SHA256_HEX_LEN]);

/* HMAC-SHA-256 (RFC 2104): the keyed MAC = H((K0^opad) || H((K0^ipad) || msg)). `key`/`msg` may be any
 * length (key longer than the 64-byte block is pre-hashed). Writes the 32-byte tag to `out`. The routine
 * zeroizes its key-derived intermediates. To CHECK a tag, recompute and compare in CONSTANT TIME at the
 * call site (never memcmp) — this routine intentionally does not compare, only emits. */
void hc_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len, unsigned char out[32]);

#ifdef __cplusplus
}
#endif
#endif /* HC_HASH_H */
