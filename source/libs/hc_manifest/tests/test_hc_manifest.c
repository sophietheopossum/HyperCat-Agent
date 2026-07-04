/* Tests for hc_manifest — append/read roundtrip, the per-turn digest's DETERMINISM (the same turn
 * content yields the same turn_sha256 across independent runs — the replay oracle's foundation, and the
 * P2 gate), bounds (over-long literal rejected), and persistence (reopen). Exit non-zero on failure. */

#define _DEFAULT_SOURCE 1

#include "hc_manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(c, m)                                                                                  \
    do {                                                                                             \
        if (!(c)) {                                                                                  \
            fprintf(stderr, "FAIL: %s\n", (m));                                                      \
            g_fails++;                                                                                \
        }                                                                                            \
    } while (0)

static hc_manifest_turn turn(int i, const char *input, const char *atext, const char *tcalls,
                             const char *tresults, const char *finish)
{
    hc_manifest_turn t;
    memset(&t, 0, sizeof t);
    t.turn_index = i;
    t.model = "echo";
    t.input = input;
    t.assistant_text = atext;
    t.tool_calls_json = tcalls;
    t.tool_results_json = tresults;
    t.finish_reason = finish;
    return t;
}

/* record the same two turns into a fresh dir; return the two turn_sha256 via out params (or "" on fail) */
static void record_run(const char *dir, char h0[HC_SHA256_HEX_LEN], char h1[HC_SHA256_HEX_LEN])
{
    h0[0] = h1[0] = '\0';
    hc_manifest *m = hc_manifest_open(dir);
    if (!m) return;
    hc_manifest_turn t0 = turn(0, "system+user history A", "let me reverse it", "", "", "stop");
    hc_manifest_turn t1 = turn(1, "system+user history A+B", "the answer is 42",
                               "[{\"name\":\"fs_write\"}]", "[{\"ok\":true}]", "tool_calls");
    hc_manifest_append(m, &t0);
    hc_manifest_append(m, &t1);
    hc_manifest_rec *recs = NULL;
    size_t           n = 0;
    if (hc_manifest_read(m, &recs, &n) == 0 && n == 2) {
        snprintf(h0, HC_SHA256_HEX_LEN, "%s", recs[0].turn_sha256);
        snprintf(h1, HC_SHA256_HEX_LEN, "%s", recs[1].turn_sha256);
    }
    hc_manifest_recs_free(recs, n);
    hc_manifest_close(m);
}

int main(void)
{
    char dirA[] = "/tmp/hcman_XXXXXX";
    if (!mkdtemp(dirA)) {
        perror("mkdtemp");
        return 1;
    }

    /* roundtrip + field correctness */
    hc_manifest *m = hc_manifest_open(dirA);
    CHECK(m != NULL, "open");
    hc_manifest_turn t0 = turn(0, "hist0", "first answer", "", "", "stop");
    hc_manifest_turn t1 = turn(1, "hist1", "second answer", "[{\"name\":\"x\"}]", "[1,2]", "tool_calls");
    CHECK(hc_manifest_append(m, &t0) == 0 && hc_manifest_append(m, &t1) == 0, "append 2 turns");

    hc_manifest_rec *recs = NULL;
    size_t           n = 0;
    CHECK(hc_manifest_read(m, &recs, &n) == 0 && n == 2, "read 2 turns");
    if (n == 2) {
        CHECK(recs[0].turn_index == 0 && strcmp(recs[0].model, "echo") == 0, "turn 0 fields");
        CHECK(strcmp(recs[0].assistant_text, "first answer") == 0, "turn 0 assistant literal");
        CHECK(strcmp(recs[1].tool_calls_json, "[{\"name\":\"x\"}]") == 0, "turn 1 tool_calls literal");
        CHECK(strcmp(recs[1].tool_results_json, "[1,2]") == 0, "turn 1 tool_results literal");
        CHECK(strlen(recs[0].turn_sha256) == 64 && strlen(recs[0].input_sha256) == 64, "digests are hex");
        CHECK(strcmp(recs[0].turn_sha256, recs[1].turn_sha256) != 0, "distinct turns -> distinct digests");
    }
    hc_manifest_recs_free(recs, n);

    /* over-long assistant literal is rejected */
    char *big = malloc(HC_MANIFEST_FIELD_MAX + 16);
    CHECK(big != NULL, "alloc big");
    if (big) {
        memset(big, 'x', HC_MANIFEST_FIELD_MAX + 8);
        big[HC_MANIFEST_FIELD_MAX + 8] = '\0';
        hc_manifest_turn tbig = turn(2, "h", big, "", "", "stop");
        CHECK(hc_manifest_append(m, &tbig) == -1, "over-long literal rejected");
        free(big);
    }
    hc_manifest_close(m);

    /* persistence: reopen dirA, the 2 turns are still there */
    m = hc_manifest_open(dirA);
    CHECK(hc_manifest_read(m, &recs, &n) == 0 && n == 2, "reopen keeps the 2 turns");
    hc_manifest_recs_free(recs, n);
    hc_manifest_close(m);

    /* DETERMINISM: two independent runs of the same turn content produce identical turn_sha256 */
    char a0[HC_SHA256_HEX_LEN], a1[HC_SHA256_HEX_LEN], b0[HC_SHA256_HEX_LEN], b1[HC_SHA256_HEX_LEN];
    char dirC[] = "/tmp/hcman_XXXXXX";
    char dirD[] = "/tmp/hcman_XXXXXX";
    if (mkdtemp(dirC) && mkdtemp(dirD)) {
        record_run(dirC, a0, a1);
        record_run(dirD, b0, b1);
        CHECK(a0[0] && strcmp(a0, b0) == 0 && strcmp(a1, b1) == 0,
              "turn_sha256 is stable across identical runs (the oracle foundation)");
        CHECK(strcmp(a0, a1) != 0, "the two distinct turns still differ");
        char p[1100];
        snprintf(p, sizeof p, "%s/manifest.jsonl", dirC); unlink(p); rmdir(dirC);
        snprintf(p, sizeof p, "%s/manifest.jsonl", dirD); unlink(p); rmdir(dirD);
    }

    /* --- the divergence oracle (hc_manifest_diff): match / mid-sequence / length mismatch --- */
    {
        hc_manifest_rec x[3], y[3];
        memset(x, 0, sizeof x);
        memset(y, 0, sizeof y);
        for (int i = 0; i < 3; i++) {
            x[i].turn_index = y[i].turn_index = i;
            snprintf(x[i].turn_sha256, sizeof x[i].turn_sha256, "sha-%d", i);
            snprintf(y[i].turn_sha256, sizeof y[i].turn_sha256, "sha-%d", i);
        }
        hc_manifest_diff_report rep;

        hc_manifest_diff(x, 3, y, 3, &rep);
        CHECK(rep.match && rep.matched == 3 && rep.first_diff == -1, "oracle: identical runs match");

        snprintf(y[1].turn_sha256, sizeof y[1].turn_sha256, "sha-DIFFERENT"); /* diverge at turn 1 */
        hc_manifest_diff(x, 3, y, 3, &rep);
        CHECK(!rep.match && rep.matched == 1 && rep.first_diff == 1,
              "oracle: mid-sequence divergence pinpoints the first differing turn");

        snprintf(y[1].turn_sha256, sizeof y[1].turn_sha256, "sha-1"); /* restore, then shorten */
        hc_manifest_diff(x, 3, y, 2, &rep);
        CHECK(!rep.match && rep.matched == 2 && rep.first_diff == 2, "oracle: a length mismatch is a divergence");
    }

    char p[1100];
    snprintf(p, sizeof p, "%s/manifest.jsonl", dirA); unlink(p); rmdir(dirA);

    if (g_fails) {
        fprintf(stderr, "hc_manifest: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_manifest: all checks passed\n");
    return 0;
}
