/* hc_caps.c — see hc_caps.h. Object-capability tokens = base64url(payload || HMAC-SHA-256(payload)) over a
 * fixed canonical layout. Pure, allocation-free, state-free; the secret is borrowed. */

#include "hc_caps.h"

#include "hc_hash.h"

#include <string.h>

/* The canonical payload layout (big-endian ints, NUL-padded strings). Asserted to sum to HC_CAP_PAYLOAD_LEN. */
enum {
    OFF_SUBJECT = 0,
    OFF_VERB = OFF_SUBJECT + HC_CAP_SUBJECT_CAP,
    OFF_SCOPE_KIND = OFF_VERB + 4,
    OFF_SCOPE = OFF_SCOPE_KIND + 4,
    OFF_BUDGET = OFF_SCOPE + HC_CAP_SCOPE_CAP,
    OFF_NOT_AFTER = OFF_BUDGET + 4,
    OFF_AGENDA = OFF_NOT_AFTER + 8,
    OFF_CAP_ID = OFF_AGENDA + HC_CAP_AGENDA_CAP,
    OFF_NONCE = OFF_CAP_ID + 8,
    OFF_END = OFF_NONCE + HC_CAP_NONCE_LEN
};
_Static_assert(OFF_END == HC_CAP_PAYLOAD_LEN, "payload layout must sum to HC_CAP_PAYLOAD_LEN");

#define TAG_LEN 32
#define RAW_LEN (HC_CAP_PAYLOAD_LEN + TAG_LEN)

/* ---- fixed-width big-endian put/get ---- */

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)(v);
}
static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static void put_u64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * (7 - i)));
}
static uint64_t get_u64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}
/* Copy a C string into a fixed `cap`-byte field, ZERO-PADDED — canonical regardless of caller hygiene. */
static void put_str(unsigned char *dst, const char *src, size_t cap)
{
    memset(dst, 0, cap);
    size_t n = 0;
    while (n < cap && src[n]) n++;
    memcpy(dst, src, n);
}
/* Copy a fixed `cap`-byte field into a C-string field, guaranteeing NUL-termination (defensive). */
static void get_str(char *dst, const unsigned char *src, size_t cap)
{
    memcpy(dst, src, cap);
    dst[cap - 1] = '\0';
}

static void serialize(const hc_cap_claims *c, unsigned char *buf /* [HC_CAP_PAYLOAD_LEN] */)
{
    put_str(buf + OFF_SUBJECT, c->subject, HC_CAP_SUBJECT_CAP);
    put_u32(buf + OFF_VERB, c->verb);
    put_u32(buf + OFF_SCOPE_KIND, c->scope_kind);
    put_str(buf + OFF_SCOPE, c->scope, HC_CAP_SCOPE_CAP);
    put_u32(buf + OFF_BUDGET, c->budget);
    put_u64(buf + OFF_NOT_AFTER, c->not_after_ms);
    put_str(buf + OFF_AGENDA, c->agenda, HC_CAP_AGENDA_CAP);
    put_u64(buf + OFF_CAP_ID, c->cap_id);
    memcpy(buf + OFF_NONCE, c->nonce, HC_CAP_NONCE_LEN);
}

static void deserialize(const unsigned char *buf, hc_cap_claims *c)
{
    memset(c, 0, sizeof *c);
    get_str(c->subject, buf + OFF_SUBJECT, HC_CAP_SUBJECT_CAP);
    c->verb = get_u32(buf + OFF_VERB);
    c->scope_kind = get_u32(buf + OFF_SCOPE_KIND);
    get_str(c->scope, buf + OFF_SCOPE, HC_CAP_SCOPE_CAP);
    c->budget = get_u32(buf + OFF_BUDGET);
    c->not_after_ms = get_u64(buf + OFF_NOT_AFTER);
    get_str(c->agenda, buf + OFF_AGENDA, HC_CAP_AGENDA_CAP);
    c->cap_id = get_u64(buf + OFF_CAP_ID);
    memcpy(c->nonce, buf + OFF_NONCE, HC_CAP_NONCE_LEN);
}

/* ---- base64url (no padding) ---- */

static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Encoded length (no padding) of `n` input bytes: ceil(n*8 / 6). */
static size_t b64_enc_len(size_t n) { return (n * 8 + 5) / 6; }

static void b64_encode(const unsigned char *in, size_t n, char *out)
{
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = B64URL[(v >> 18) & 0x3f];
        out[o++] = B64URL[(v >> 12) & 0x3f];
        out[o++] = B64URL[(v >> 6) & 0x3f];
        out[o++] = B64URL[v & 0x3f];
        i += 3;
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64URL[(v >> 18) & 0x3f];
        out[o++] = B64URL[(v >> 12) & 0x3f];
    } else if (rem == 2) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;
        out[o++] = B64URL[(v >> 18) & 0x3f];
        out[o++] = B64URL[(v >> 12) & 0x3f];
        out[o++] = B64URL[(v >> 6) & 0x3f];
    }
    out[o] = '\0';
}

/* Reverse-lookup for a base64url char: its 0..63 value, or -1 if not in the alphabet. */
static int b64_val(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* Decode `len` base64url chars into `out` (capacity `out_cap`). Returns the byte count via *out_n, or 0 on
 * any malformation (non-alphabet byte, a length ≡ 1 mod 4 which is impossible in base64, or overflow). Total
 * + bounded: it never reads past `len` and never writes past `out_cap`. */
static int b64_decode(const char *in, size_t len, unsigned char *out, size_t out_cap, size_t *out_n)
{
    if (len % 4 == 1) return 0; /* a lone trailing char can never be valid base64 */
    size_t o = 0;
    size_t i = 0;
    while (i + 4 <= len) {
        int a = b64_val(in[i]), b = b64_val(in[i + 1]), c = b64_val(in[i + 2]), d = b64_val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return 0;
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
        if (o + 3 > out_cap) return 0;
        out[o++] = (unsigned char)(v >> 16);
        out[o++] = (unsigned char)(v >> 8);
        out[o++] = (unsigned char)(v);
        i += 4;
    }
    size_t rem = len - i;
    if (rem == 2) {
        int a = b64_val(in[i]), b = b64_val(in[i + 1]);
        if (a < 0 || b < 0) return 0;
        if (b & 0x0f) return 0; /* CANONICAL: the 2-char group's 4 unused low bits MUST be zero — reject a
                                 * non-canonical encoding so each token has exactly ONE valid string (no
                                 * malleability where a flipped slack bit decodes to the same capability). */
        if (o + 1 > out_cap) return 0;
        out[o++] = (unsigned char)((a << 2) | (b >> 4));
    } else if (rem == 3) {
        int a = b64_val(in[i]), b = b64_val(in[i + 1]), c = b64_val(in[i + 2]);
        if (a < 0 || b < 0 || c < 0) return 0;
        if (c & 0x03) return 0; /* CANONICAL: the 3-char group's 2 unused low bits MUST be zero */
        if (o + 2 > out_cap) return 0;
        out[o++] = (unsigned char)((a << 2) | (b >> 4));
        out[o++] = (unsigned char)((b << 4) | (c >> 2));
    }
    *out_n = o;
    return 1;
}

/* ---- constant-time compares ---- */

static int ct_equal(const unsigned char *a, const unsigned char *b, size_t n)
{
    volatile unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* Constant-time match of a fixed `cap`-byte NUL-padded field against a C string (zero-padded into a scratch
 * field), so a token's bound subject is compared without an early-out length leak. The scratch is sized to the
 * subject cap (the only field this compares); the asserts below pin that "subject is the largest" invariant. */
_Static_assert(HC_CAP_SUBJECT_CAP >= HC_CAP_AGENDA_CAP, "field_eq scratch must hold the largest compared field");
static int field_eq(const char *field, size_t cap, const char *s)
{
    unsigned char a[HC_CAP_SUBJECT_CAP];
    if (cap > sizeof a) return 0; /* fail-closed if ever called with a larger field than the scratch holds */
    memset(a, 0, cap);
    size_t n = 0;
    if (s)
        while (n < cap && s[n]) n++;
    if (n) memcpy(a, s, n);
    return ct_equal((const unsigned char *)field, a, cap);
}

/* ---- scope predicate ---- */

/* Component-boundary prefix: `prefix` covers `path` iff path starts with prefix AND the boundary lands on a
 * path separator or the end — so "notes/" covers "notes/x" but never "notes-evil/x". */
static int path_prefix_match(const char *prefix, const char *path)
{
    if (!path) return 0;
    size_t pl = strlen(prefix);
    if (pl == 0) return 1; /* empty prefix = any path */
    if (strncmp(path, prefix, pl) != 0) return 0;
    if (prefix[pl - 1] == '/') return 1; /* the prefix already ends on a boundary */
    char after = path[pl];
    return after == '\0' || after == '/';
}

/* ---- public API ---- */

hc_cap_status hc_caps_mint(const hc_cap_claims *claims, const uint8_t *secret, size_t secret_len, char *out,
                           size_t out_cap, size_t *out_len)
{
    if (!claims || !secret || !out) return HC_CAP_ERR_INVALID;
    unsigned char raw[RAW_LEN];
    serialize(claims, raw);
    hc_hmac_sha256(secret, secret_len, raw, HC_CAP_PAYLOAD_LEN, raw + HC_CAP_PAYLOAD_LEN);
    size_t need = b64_enc_len(RAW_LEN);
    if (out_cap < need + 1) return HC_CAP_ERR_INVALID; /* need the chars + a NUL */
    b64_encode(raw, RAW_LEN, out);
    if (out_len) *out_len = need;
    return HC_CAP_OK;
}

hc_cap_status hc_caps_verify(const char *token, size_t len, const uint8_t *secret, size_t secret_len,
                             hc_cap_claims *out)
{
    if (!token || !secret || !out) return HC_CAP_ERR_INVALID;
    unsigned char raw[RAW_LEN];
    size_t        n = 0;
    if (!b64_decode(token, len, raw, sizeof raw, &n)) return HC_CAP_ERR_BADFMT;
    if (n != RAW_LEN) return HC_CAP_ERR_BADFMT; /* exact length — no over/under-long token */
    unsigned char tag[TAG_LEN];
    hc_hmac_sha256(secret, secret_len, raw, HC_CAP_PAYLOAD_LEN, tag);
    if (!ct_equal(tag, raw + HC_CAP_PAYLOAD_LEN, TAG_LEN)) return HC_CAP_ERR_FORGED;
    deserialize(raw, out); /* parse ONLY after the tag is authenticated */
    return HC_CAP_OK;
}

int hc_caps_authorizes(const hc_cap_claims *claims, const char *authenticated_subject, uint32_t verb,
                       const char *path_or_arg, uint64_t now_ms)
{
    if (!claims) return 0;
    if (!field_eq(claims->subject, HC_CAP_SUBJECT_CAP, authenticated_subject)) return 0; /* deputy bind */
    if (claims->verb != verb) return 0;
    if (now_ms != 0 && claims->not_after_ms != 0 && now_ms > claims->not_after_ms) return 0; /* expired */
    switch (claims->scope_kind) {
    case HC_CAP_SCOPE_NONE:
        return 1;
    case HC_CAP_SCOPE_PATH_PREFIX:
        return path_prefix_match(claims->scope, path_or_arg);
    case HC_CAP_SCOPE_PATH_EXACT:
    case HC_CAP_SCOPE_ARG_EQ:
        return path_or_arg && strcmp(claims->scope, path_or_arg) == 0;
    default:
        return 0; /* an unknown scope kind authorizes nothing (fail closed) */
    }
}
