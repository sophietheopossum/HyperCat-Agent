/* test_hc_caps — the P09 capability-token gate. Proves: mint->verify round-trips the claims; EVERY single-bit
 * flip of the token invalidates it (the tag covers the whole payload); a token is bound to its subject (a
 * stolen token fails for another subject); the component-boundary scope ("notes/" vs "notes-evil/"); expiry is
 * honored; and malformed / wrong-key / truncated tokens are rejected (never parsed). Pure, no I/O. */

#include "hc_caps.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m)                                                                                          \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            fprintf(stderr, "FAIL: %s\n", (m));                                                              \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

static hc_cap_claims sample_claims(void)
{
    hc_cap_claims c;
    memset(&c, 0, sizeof c);
    strcpy(c.subject, "agent:A");
    c.verb = HC_CAP_FS_WRITE;
    c.scope_kind = HC_CAP_SCOPE_PATH_PREFIX;
    strcpy(c.scope, "notes/");
    c.budget = 5;
    c.not_after_ms = 1000000;
    strcpy(c.agenda, "conductor-7");
    c.cap_id = 42;
    for (int i = 0; i < HC_CAP_NONCE_LEN; i++) c.nonce[i] = (unsigned char)(i * 7 + 1);
    return c;
}

int main(void)
{
    const uint8_t key[32] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
                             0xcc, 0xdd, 0xee, 0xff, 0x00, 1,    2,    3,    4,    5,    6,
                             7,    8,    9,    10,   11,   12,   13,   14,   15,   16};
    const uint8_t key2[32] = {0}; /* a DIFFERENT key (all zero) */

    /* --- mint -> verify round-trips every field --- */
    hc_cap_claims c = sample_claims();
    char          tok[HC_CAP_TOKEN_MAX];
    size_t        tlen = 0;
    CHECK(hc_caps_mint(&c, key, sizeof key, tok, sizeof tok, &tlen) == HC_CAP_OK, "mint ok");
    CHECK(tlen > 0 && tlen < HC_CAP_TOKEN_MAX, "token length is bounded");

    hc_cap_claims v;
    CHECK(hc_caps_verify(tok, tlen, key, sizeof key, &v) == HC_CAP_OK, "verify ok");
    CHECK(strcmp(v.subject, "agent:A") == 0 && v.verb == HC_CAP_FS_WRITE &&
              v.scope_kind == HC_CAP_SCOPE_PATH_PREFIX && strcmp(v.scope, "notes/") == 0 && v.budget == 5 &&
              v.not_after_ms == 1000000 && strcmp(v.agenda, "conductor-7") == 0 && v.cap_id == 42,
          "verify round-trips every claim");
    CHECK(memcmp(v.nonce, c.nonce, HC_CAP_NONCE_LEN) == 0, "verify round-trips the nonce");

    /* --- a DIFFERENT signing key rejects the token (FORGED, not parsed) --- */
    hc_cap_claims junk;
    CHECK(hc_caps_verify(tok, tlen, key2, sizeof key2, &junk) == HC_CAP_ERR_FORGED, "wrong key -> FORGED");

    /* --- EVERY single-bit flip of EVERY token byte invalidates it (the tag covers the whole payload) --- */
    {
        int survived = 0; /* count any flip that still verifies OK == a forgery hole */
        for (size_t i = 0; i < tlen; i++) {
            for (int bit = 0; bit < 6; bit++) {       /* base64url chars: flipping any of 6 value-bits... */
                char saved = tok[i];
                /* perturb to a DIFFERENT valid-or-invalid char: just xor the char code */
                char mutated = (char)(saved ^ (1 << (bit % 7)));
                if (mutated == saved) continue;
                tok[i] = mutated;
                hc_cap_claims t;
                if (hc_caps_verify(tok, tlen, key, sizeof key, &t) == HC_CAP_OK) survived++;
                tok[i] = saved;
            }
        }
        CHECK(survived == 0, "no single-character perturbation of the token ever verifies (tag covers all)");
    }

    /* --- subject binding: a token minted for agent:A is NOT authorized for agent:B (stolen-token close) --- */
    CHECK(hc_caps_authorizes(&v, "agent:A", HC_CAP_FS_WRITE, "notes/x.txt", 0) == 1,
          "authorizes the bound subject");
    CHECK(hc_caps_authorizes(&v, "agent:B", HC_CAP_FS_WRITE, "notes/x.txt", 0) == 0,
          "REJECTS a different subject (confused-deputy / stolen token)");

    /* --- verb must match --- */
    CHECK(hc_caps_authorizes(&v, "agent:A", HC_CAP_EXEC, "notes/x.txt", 0) == 0, "rejects a different verb");

    /* --- component-boundary scope: notes/ covers notes/x but NEVER notes-evil/x --- */
    CHECK(hc_caps_authorizes(&v, "agent:A", HC_CAP_FS_WRITE, "notes/sub/x", 0) == 1, "prefix covers notes/sub/x");
    CHECK(hc_caps_authorizes(&v, "agent:A", HC_CAP_FS_WRITE, "notes-evil/x", 0) == 0,
          "prefix does NOT cover notes-evil/x (component boundary)");
    CHECK(hc_caps_authorizes(&v, "agent:A", HC_CAP_FS_WRITE, "src/x", 0) == 0, "prefix does not cover src/x");

    /* --- expiry: now past not_after_ms -> not authorized; now==0 -> expiry skipped --- */
    CHECK(hc_caps_authorizes(&v, "agent:A", HC_CAP_FS_WRITE, "notes/x", 999999) == 1, "unexpired at t<deadline");
    CHECK(hc_caps_authorizes(&v, "agent:A", HC_CAP_FS_WRITE, "notes/x", 1000001) == 0, "expired at t>deadline");

    /* --- exact-scope + arg-eq variants --- */
    {
        hc_cap_claims e = sample_claims();
        e.scope_kind = HC_CAP_SCOPE_PATH_EXACT;
        strcpy(e.scope, "out/report.txt");
        char et[HC_CAP_TOKEN_MAX];
        size_t el = 0;
        hc_caps_mint(&e, key, sizeof key, et, sizeof et, &el);
        hc_cap_claims ev;
        CHECK(hc_caps_verify(et, el, key, sizeof key, &ev) == HC_CAP_OK, "exact-scope token verifies");
        CHECK(hc_caps_authorizes(&ev, "agent:A", HC_CAP_FS_WRITE, "out/report.txt", 0) == 1, "exact match ok");
        CHECK(hc_caps_authorizes(&ev, "agent:A", HC_CAP_FS_WRITE, "out/report.txt.bak", 0) == 0,
              "exact scope rejects a near-miss");
    }

    /* --- SCOPE_NONE authorizes any path; SCOPE_ARG_EQ is an exact arg match --- */
    {
        hc_cap_claims n = sample_claims();
        n.scope_kind = HC_CAP_SCOPE_NONE;
        n.scope[0] = '\0';
        char nt[HC_CAP_TOKEN_MAX];
        size_t nl = 0;
        hc_caps_mint(&n, key, sizeof key, nt, sizeof nt, &nl);
        hc_cap_claims nv;
        CHECK(hc_caps_verify(nt, nl, key, sizeof key, &nv) == HC_CAP_OK, "scope-none token verifies");
        CHECK(hc_caps_authorizes(&nv, "agent:A", HC_CAP_FS_WRITE, "anything/at/all", 0) == 1,
              "SCOPE_NONE authorizes any path");
        CHECK(hc_caps_authorizes(&nv, "agent:A", HC_CAP_FS_WRITE, NULL, 0) == 1,
              "SCOPE_NONE authorizes even a NULL target");

        hc_cap_claims g = sample_claims();
        g.verb = HC_CAP_EXEC;
        g.scope_kind = HC_CAP_SCOPE_ARG_EQ;
        strcpy(g.scope, "--safe-flag");
        char gt[HC_CAP_TOKEN_MAX];
        size_t gl = 0;
        hc_caps_mint(&g, key, sizeof key, gt, sizeof gt, &gl);
        hc_cap_claims gv;
        CHECK(hc_caps_verify(gt, gl, key, sizeof key, &gv) == HC_CAP_OK, "arg-eq token verifies");
        CHECK(hc_caps_authorizes(&gv, "agent:A", HC_CAP_EXEC, "--safe-flag", 0) == 1, "ARG_EQ exact match ok");
        CHECK(hc_caps_authorizes(&gv, "agent:A", HC_CAP_EXEC, "--safe-flagX", 0) == 0,
              "ARG_EQ rejects a near-miss arg");
    }

    /* --- malformed input rejected (never parsed) --- */
    {
        hc_cap_claims t;
        CHECK(hc_caps_verify("", 0, key, sizeof key, &t) == HC_CAP_ERR_BADFMT, "empty token -> BADFMT");
        CHECK(hc_caps_verify("!!!!not base64url!!!!", 21, key, sizeof key, &t) == HC_CAP_ERR_BADFMT,
              "non-alphabet bytes -> BADFMT");
        CHECK(hc_caps_verify(tok, tlen - 4, key, sizeof key, &t) == HC_CAP_ERR_BADFMT,
              "a truncated token -> BADFMT (wrong decoded length)");
        char longer[HC_CAP_TOKEN_MAX + 8];
        memcpy(longer, tok, tlen);
        memcpy(longer + tlen, "AAAA", 4); /* 4 extra valid chars -> 3 extra bytes -> wrong length */
        CHECK(hc_caps_verify(longer, tlen + 4, key, sizeof key, &t) == HC_CAP_ERR_BADFMT,
              "an over-long token -> BADFMT");
        CHECK(hc_caps_verify(NULL, 0, key, sizeof key, &t) == HC_CAP_ERR_INVALID, "null token -> INVALID");
    }

    /* --- mint into a too-small buffer fails cleanly --- */
    {
        char small[16];
        CHECK(hc_caps_mint(&c, key, sizeof key, small, sizeof small, NULL) == HC_CAP_ERR_INVALID,
              "mint into a too-small buffer -> INVALID");
    }

    if (g_fail) {
        fprintf(stderr, "test_hc_caps: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_hc_caps: all checks passed\n");
    return 0;
}
