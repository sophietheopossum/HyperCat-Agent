#ifndef HC_CAPS_H
#define HC_CAPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_caps — unforgeable, scoped, expiring OBJECT-CAPABILITY TOKENS (P09). A token says "the bearer named
 * `subject` may perform `verb` within `scope` until `not_after_ms`," authenticated by an HMAC-SHA-256 tag
 * over a fixed, canonical payload — so it cannot be forged, widened, or replayed across subjects without
 * the host's signing key.
 *
 * Purpose:   mint + verify + structurally-authorize capability tokens. The stateful half — minting policy, a
 *            budget/revocation registry, the bus round-trip — is the host's CapabilityAuthority (app/host_bridge),
 *            NOT here. Depends only on hc_hash (HMAC) + libc.
 * Owns:      nothing — a PURE primitive: it operates on caller-owned bytes, holds NO state, allocates nothing.
 * Threading: reentrant; no shared state.
 * Lifetime:  inputs/outputs are caller-owned; the `secret` is BORROWED (never copied/stored/freed); a minted
 *            token string is valid until the caller's buffer is reused.
 *
 * Security contract:
 *   - The tag is computed over the ENTIRE fixed payload (subject+verb+scope+budget+expiry+agenda+cap_id+
 *     nonce), so every field — including expiry — is signed; flipping any bit invalidates the token.
 *   - hc_caps_verify recomputes the tag and compares it in CONSTANT TIME, then parses the claims ONLY on a
 *     match. A caller MUST NOT read *out on a non-OK return.
 *   - hc_caps_authorizes binds the token to an AUTHENTICATED subject (the broker-stamped sender id): a token
 *     minted for subject A is rejected when presented by subject B (the confused-deputy / stolen-token close).
 *   - Wire form is base64url(payload||tag) with NO padding — frame/JSON-safe (no +,/,=). Decode is total +
 *     bounded + CANONICAL (non-alphabet bytes, wrong length, or non-canonical slack bits -> ERR_BADFMT; never
 *     reads past `len`), so each capability has exactly ONE valid token string.
 *
 * Caller obligations (the host CapabilityAuthority, P09.2 — this primitive does NOT enforce them):
 *   - PATH SCOPE IS LEXICAL: path_prefix_match does NO normalization, so a caller MUST canonicalize
 *     `path_or_arg` (collapse '..'/'.', resolve to the same form the sandbox's openat(O_NOFOLLOW) enforces)
 *     BEFORE hc_caps_authorizes, or '..' escapes the scope. The scope is a policy floor ABOVE the sandbox jail,
 *     never a replacement for it.
 *   - NUL-FREE FIELDS: a subject/scope/agenda is truncated at its first NUL on mint — the authority must reject
 *     fields containing '\0' (broker-stamped ids cannot carry one, so this is belt-and-braces).
 *   - REAL CLOCK: pass a real now_ms to hc_caps_authorizes; now_ms==0 SKIPS the expiry check (a sentinel for
 *     "expiry not evaluated"), so never feed it a live clock value of 0. */

/* The action a capability authorizes. Only HC_CAP_FS_WRITE is wired end-to-end in the P09 MVP; the rest are
 * reserved so the wire format is stable as they are added (the value, not the name, is what is signed). */
typedef enum {
    HC_CAP_VERB_NONE = 0,
    HC_CAP_FS_WRITE = 1,
    HC_CAP_DEEP_REASON = 2,
    HC_CAP_MEMORY_WRITE = 3,
    HC_CAP_EXEC = 4
} hc_cap_verb;

/* How `scope` constrains the action's target. PATH_PREFIX uses a COMPONENT-BOUNDARY match ("notes/" covers
 * "notes/x" but never "notes-evil/x"); PATH_EXACT / ARG_EQ require an exact string equality; NONE = unconstrained. */
typedef enum {
    HC_CAP_SCOPE_NONE = 0,
    HC_CAP_SCOPE_PATH_PREFIX = 1,
    HC_CAP_SCOPE_PATH_EXACT = 2,
    HC_CAP_SCOPE_ARG_EQ = 3
} hc_cap_scope_kind;

typedef enum {
    HC_CAP_OK = 0,
    HC_CAP_ERR_INVALID, /* null arg, or the out buffer is too small to hold the token            */
    HC_CAP_ERR_BADFMT,  /* the token is not valid base64url, or decodes to the wrong length       */
    HC_CAP_ERR_FORGED   /* the tag does not match — tampered, wrong key, or fabricated            */
} hc_cap_status;

/* Field byte-caps (the strings are NUL-terminated within these; the authority bounds them on mint). */
#define HC_CAP_SUBJECT_CAP 64
#define HC_CAP_SCOPE_CAP   256
#define HC_CAP_AGENDA_CAP  64
#define HC_CAP_NONCE_LEN   16

/* The claims a token carries. POD, caller-owned. Mint zero-pads the char fields canonically, so a caller need
 * only set the bytes it uses (but SHOULD zero-init the struct). `cap_id`/`nonce` make each token unique +
 * registry-addressable; `budget`/revocation/agenda-liveness are enforced by the host registry, not here. */
typedef struct {
    char     subject[HC_CAP_SUBJECT_CAP]; /* the bearer id the token is bound to (== broker-stamped sender) */
    uint32_t verb;                        /* an hc_cap_verb                                                 */
    uint32_t scope_kind;                  /* an hc_cap_scope_kind                                           */
    char     scope[HC_CAP_SCOPE_CAP];     /* the path prefix / exact path / arg the verb is confined to     */
    uint32_t budget;                      /* max uses (registry-enforced; carried so it is signed)          */
    uint64_t not_after_ms;               /* wall-clock expiry in ms (0 = no expiry); signed + checked        */
    char     agenda[HC_CAP_AGENDA_CAP];   /* the agenda this grant is scoped to (registry liveness)         */
    uint64_t cap_id;                      /* the registry key (unique per grant)                            */
    uint8_t  nonce[HC_CAP_NONCE_LEN];     /* 128-bit uniqueness                                             */
} hc_cap_claims;

/* The serialized payload is a fixed canonical layout (big-endian ints, NUL-padded strings); the token is
 * base64url(payload || 32-byte tag), no padding. Sizes exposed so callers size buffers without internals. */
#define HC_CAP_PAYLOAD_LEN (HC_CAP_SUBJECT_CAP + 4 + 4 + HC_CAP_SCOPE_CAP + 4 + 8 + HC_CAP_AGENDA_CAP + 8 + HC_CAP_NONCE_LEN)
#define HC_CAP_TOKEN_MAX   640 /* base64url(HC_CAP_PAYLOAD_LEN + 32) is 614 chars; 640 leaves room + NUL */

/* Mint a token for `claims`, signed with `secret`. Writes a NUL-terminated base64url token into `out`
 * (capacity `out_cap`, use HC_CAP_TOKEN_MAX) and its length (excl. NUL) into *out_len (if non-null).
 * The secret is BORROWED (read, never stored). HC_CAP_ERR_INVALID on a null arg or too-small `out`. */
hc_cap_status hc_caps_mint(const hc_cap_claims *claims, const uint8_t *secret, size_t secret_len,
                           char *out, size_t out_cap, size_t *out_len);

/* Verify a token: decode, recompute the tag, CONSTANT-TIME compare, and ONLY on a match parse the claims
 * into *out. Returns HC_CAP_OK (and *out is valid) or an error (and *out is untouched — do NOT read it).
 * `len` bounds the token bytes read. */
hc_cap_status hc_caps_verify(const char *token, size_t len, const uint8_t *secret, size_t secret_len,
                             hc_cap_claims *out);

/* Structural authorization check on ALREADY-VERIFIED claims (call hc_caps_verify first): the token's subject
 * equals `authenticated_subject` (constant-time), its verb equals `verb`, `path_or_arg` satisfies the scope
 * (component-boundary prefix / exact), and it is unexpired when now_ms != 0. Returns 1 if authorized, else 0.
 * Budget + revocation + agenda-liveness are the host registry's checks, layered ON TOP of this. */
int hc_caps_authorizes(const hc_cap_claims *claims, const char *authenticated_subject, uint32_t verb,
                       const char *path_or_arg, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* HC_CAPS_H */
