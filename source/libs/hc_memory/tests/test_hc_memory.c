/* Tests for hc_memory — fully offline (vectors are hand-crafted fixtures; no hc_llm). Covers write +
 * dedup, dim adoption + mismatch reject, the cosine query + the scope filter (an agent cannot read
 * another's scope), bounds (oversized text / non-finite vector rejected), forget, and persistence-replay
 * (close + reopen rebuilds the index from the log), and the embedding-model binding (a same-dimension
 * model swap must be refused, not silently served). Exit non-zero on any failure. */

#define _DEFAULT_SOURCE 1

#include "hc_memory.h"

#include <math.h>
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

static hc_mem_record rec(const char *scope, const char *text, const float *vec, int dim, uint64_t t)
{
    hc_mem_record r;
    memset(&r, 0, sizeof r);
    r.scope = scope;
    r.text = text;
    r.source = "test";
    r.vec = vec;
    r.dim = dim;
    r.importance = 0.5;
    r.created_ms = t;
    return r;
}

/* The embedding-model binding. The dangerous case is a DIFFERENT model with the SAME dimension: every
 * length check passes and recall silently scores vectors from two incompatible spaces against each
 * other. These checks pin that it is refused, that same-model reopen is unaffected, that a store
 * predating the field adopts rather than being condemned, and that an id containing JSON metacharacters
 * survives the round-trip (a mangled meta.json would read back as dim 0 and re-open the one-way door). */
static void test_model_binding(void)
{
    char dir[] = "/tmp/hcmemmodel_XXXXXX";
    if (!mkdtemp(dir)) {
        CHECK(0, "mkdtemp for model-binding test");
        return;
    }
    float        v[4] = {1, 0, 0, 0};
    float        q[4] = {1, 0, 0, 0};
    hc_mem_hit  *hits = NULL;
    size_t       n = 0;

    /* 1. a fresh store adopts the configured model along with the dimension */
    hc_memory *m = hc_memory_open_model(dir, "prov/one");
    CHECK(m != NULL, "model: open with a model");
    hc_mem_record r = rec("agent:A", "hello", v, 4, 1000);
    CHECK(m && hc_memory_write(m, &r, NULL) == 0, "model: first write succeeds");
    CHECK(m && strcmp(hc_memory_model(m), "prov/one") == 0, "model: recorded on first write");
    CHECK(m && hc_memory_model_mismatch(m) == 0, "model: no mismatch against itself");
    hc_memory_close(m);

    /* 2. reopening with the SAME model is business as usual */
    m = hc_memory_open_model(dir, "prov/one");
    CHECK(m && hc_memory_model_mismatch(m) == 0, "model: same model reopens clean");
    CHECK(m && hc_memory_count(m) == 1, "model: records survive the reopen");
    CHECK(m && hc_memory_query(m, q, 4, NULL, 0, 10, &hits, &n) == 0 && n == 1, "model: query works");
    hc_memory_hits_free(hits, n);
    hits = NULL;
    n = 0;
    hc_memory_close(m);

    /* 3. a DIFFERENT model of the SAME dimension — the silent-corruption case — is refused */
    m = hc_memory_open_model(dir, "prov/two");
    CHECK(m != NULL, "model: mismatched open still returns a handle to diagnose with");
    CHECK(m && hc_memory_model_mismatch(m) == 1, "model: mismatch detected");
    CHECK(m && strcmp(hc_memory_model(m), "prov/one") == 0, "model: reports the STORED id, not the wanted one");
    hc_mem_record r2 = rec("agent:A", "second", v, 4, 2000);
    CHECK(m && hc_memory_write(m, &r2, NULL) == -1, "model: write refused on mismatch");
    CHECK(m && hc_memory_query(m, q, 4, NULL, 0, 10, &hits, &n) == -1, "model: query refused on mismatch");
    CHECK(m && hc_memory_count(m) == 1, "model: refused write did not grow the store");
    /* Remediation must work IN PLACE, without a restart. list/forget stay legal during a mismatch
     * precisely so an operator can drop what they can no longer query -- and once the last record is
     * gone there are no vectors left for a foreign model to be incomparable with, which is the same
     * condition the write path already treats as a rebind. If the refusal outlived the emptying, an
     * operator who did exactly the prescribed thing would find the store still refusing. */
    if (m) {
        hc_mem_hit *rows = NULL;
        size_t      nr = 0;
        CHECK(hc_memory_list(m, 10, &rows, &nr) == 0 && nr == 1, "model: list works during a mismatch");
        if (rows && nr == 1) CHECK(hc_memory_forget(m, rows[0].id) == 0, "model: forget works during a mismatch");
        hc_memory_hits_free(rows, nr);
        CHECK(hc_memory_count(m) == 0, "model: the store is now empty");
        CHECK(hc_memory_model_mismatch(m) == 0, "model: emptying the store LIFTS the mismatch");
        hc_mem_record r2b = rec("agent:A", "after remediation", v, 4, 2500);
        CHECK(hc_memory_write(m, &r2b, NULL) == 0, "model: write accepted again after remediation");
        CHECK(strcmp(hc_memory_model(m), "prov/two") == 0, "model: rebound to the new id in place");
    }
    hc_memory_close(m);
    /* put the fixture back the way step 4 onwards expects it: one record, bound to prov/one */
    m = hc_memory_open_model(dir, "prov/two");
    if (m) {
        hc_mem_hit *rows = NULL;
        size_t      nr = 0;
        if (hc_memory_list(m, 10, &rows, &nr) == 0)
            for (size_t i = 0; i < nr; i++) hc_memory_forget(m, rows[i].id);
        hc_memory_hits_free(rows, nr);
        hc_memory_close(m);
    }
    m = hc_memory_open_model(dir, "prov/one");
    hc_mem_record r1b = rec("agent:A", "hello", v, 4, 1000);
    CHECK(m && hc_memory_write(m, &r1b, NULL) == 0, "model: fixture restored on prov/one");
    hc_memory_close(m);

    /* 4. no configured model = no enforcement (the pre-existing behaviour is preserved) */
    m = hc_memory_open(dir);
    CHECK(m && hc_memory_model_mismatch(m) == 0, "model: unconfigured caller is not enforced");
    CHECK(m && hc_memory_query(m, q, 4, NULL, 0, 10, &hits, &n) == 0 && n == 1, "model: unconfigured query works");
    hc_memory_hits_free(hits, n);
    hits = NULL;
    n = 0;
    hc_memory_close(m);

    /* 5. a store predating the field (meta.json with no "model") adopts rather than being condemned */
    char meta[1100];
    snprintf(meta, sizeof meta, "%s/meta.json", dir);
    FILE *f = fopen(meta, "w");
    CHECK(f != NULL, "model: rewrite meta.json as a pre-model store");
    if (f) {
        fputs("{\"dim\":4}", f);
        fclose(f);
    }
    m = hc_memory_open_model(dir, "prov/three");
    CHECK(m && hc_memory_model_mismatch(m) == 0, "model: legacy store is not a mismatch");
    CHECK(m && hc_memory_model(m)[0] == 0, "model: legacy store reports no recorded model");
    hc_mem_record r3 = rec("agent:A", "third", v, 4, 3000);
    CHECK(m && hc_memory_write(m, &r3, NULL) == 0, "model: legacy store accepts a write");
    CHECK(m && strcmp(hc_memory_model(m), "prov/three") == 0, "model: legacy store adopts on next write");
    CHECK(m && hc_memory_model_adopted(m) == 1, "model: an adopted id is flagged as an assumption");
    hc_memory_close(m);
    m = hc_memory_open_model(dir, "prov/three");
    CHECK(m && hc_memory_model_adopted(m) == 1, "model: the adopted flag survives a reopen");
    hc_memory_close(m);
    m = hc_memory_open_model(dir, "prov/four");
    CHECK(m && hc_memory_model_mismatch(m) == 1, "model: adopted id is enforced from then on");
    hc_memory_close(m);

    /* 6. an id carrying JSON metacharacters must round-trip, not mangle meta.json */
    char dir2[] = "/tmp/hcmemesc_XXXXXX";
    if (mkdtemp(dir2)) {
        const char *tricky = "prov/quote\"and\\back";
        m = hc_memory_open_model(dir2, tricky);
        hc_mem_record r4 = rec("agent:A", "esc", v, 4, 4000);
        CHECK(m && hc_memory_write(m, &r4, NULL) == 0, "model: write with a metacharacter id");
        hc_memory_close(m);
        m = hc_memory_open_model(dir2, tricky);
        CHECK(m && hc_memory_dim(m) == 4, "model: meta.json survived escaping (dim still 4)");
        CHECK(m && strcmp(hc_memory_model(m), tricky) == 0, "model: metacharacter id round-trips");
        CHECK(m && hc_memory_model_mismatch(m) == 0, "model: metacharacter id matches itself");
        hc_memory_close(m);
        char p2[1100];
        snprintf(p2, sizeof p2, "%s/records.jsonl", dir2); unlink(p2);
        snprintf(p2, sizeof p2, "%s/vectors.f32", dir2);   unlink(p2);
        snprintf(p2, sizeof p2, "%s/meta.json", dir2);     unlink(p2);
        rmdir(dir2);
    }

    /* 7. a store branded with a model but holding NO records must NOT latch a mismatch. This is the
     *    state an interrupted first write used to leave behind: refusing forever would strand it with
     *    a diagnostic naming vectors that were never written. */
    char dir3[] = "/tmp/hcmemempty_XXXXXX";
    if (mkdtemp(dir3)) {
        char m3[1100];
        snprintf(m3, sizeof m3, "%s/meta.json", dir3);
        FILE *f3 = fopen(m3, "w");
        if (f3) {
            fputs("{\"dim\":4,\"model\":\"prov/stale\"}", f3);
            fclose(f3);
        }
        m = hc_memory_open_model(dir3, "prov/fresh");
        CHECK(m && hc_memory_count(m) == 0, "model: branded-but-empty store has no records");
        CHECK(m && hc_memory_model_mismatch(m) == 0, "model: empty store does NOT latch a mismatch");
        hc_mem_record r5 = rec("agent:A", "after rebind", v, 4, 5000);
        CHECK(m && hc_memory_write(m, &r5, NULL) == 0, "model: empty store accepts a write and rebinds");
        CHECK(m && strcmp(hc_memory_model(m), "prov/fresh") == 0, "model: empty store rebound to the new id");
        hc_memory_close(m);
        m = hc_memory_open_model(dir3, "prov/stale");
        CHECK(m && hc_memory_model_mismatch(m) == 1, "model: after rebinding, the OLD id is now refused");
        hc_memory_close(m);
        char q3[1100];
        snprintf(q3, sizeof q3, "%s/records.jsonl", dir3); unlink(q3);
        snprintf(q3, sizeof q3, "%s/vectors.f32", dir3);   unlink(q3);
        unlink(m3);
        rmdir(dir3);
    }

    /* 8-10. an unusable id, an unconfigured write, and a hostile recorded id. */
    char dir4[] = "/tmp/hcmemlong_XXXXXX";
    if (mkdtemp(dir4)) {
        char huge[HC_MEM_MODEL_MAX + 32];
        memset(huge, 'x', sizeof huge - 1);
        huge[sizeof huge - 1] = 0;
        m = hc_memory_open_model(dir4, huge);
        CHECK(m != NULL, "model: an over-long id still opens the store");
        CHECK(m && hc_memory_model_mismatch(m) == 0, "model: over-long id leaves enforcement off");
        hc_mem_record r6 = rec("agent:A", "long id", v, 4, 6000);
        CHECK(m && hc_memory_write(m, &r6, NULL) == 0, "model: writes still work with an unusable id");
        CHECK(m && hc_memory_model(m)[0] == 0, "model: an unusable id is not recorded");
        hc_memory_close(m);

        char m4[1100];
        snprintf(m4, sizeof m4, "%s/meta.json", dir4);
        FILE *f4 = fopen(m4, "w");
        if (f4) {
            fputs("{\"model\":\"prov/keep\"}", f4); /* no dim -> the next write is a 'first' write */
            fclose(f4);
        }
        m = hc_memory_open(dir4); /* no configured model at all */
        CHECK(m && hc_memory_dim(m) == 0, "model: meta without dim reads as an unwritten store");
        hc_mem_record r7 = rec("agent:A", "unconfigured", v, 4, 7000);
        CHECK(m && hc_memory_write(m, &r7, NULL) == 0, "model: unconfigured write succeeds");
        hc_memory_close(m);
        m = hc_memory_open_model(dir4, "prov/keep");
        CHECK(m && hc_memory_model_mismatch(m) == 0,
              "model: an unconfigured write did not erase the recorded id");
        hc_memory_close(m);

        FILE *f5 = fopen(m4, "w");
        if (f5) {
            fputs("{\"dim\":4,\"model\":\"evil\\u000ahost: forged\"}", f5);
            fclose(f5);
        }
        m = hc_memory_open_model(dir4, "prov/x");
        CHECK(m && hc_memory_model(m)[0] == 0, "model: an unprintable recorded id is rejected");
        hc_memory_close(m);

        char q4[1100];
        snprintf(q4, sizeof q4, "%s/records.jsonl", dir4); unlink(q4);
        snprintf(q4, sizeof q4, "%s/vectors.f32", dir4);   unlink(q4);
        unlink(m4);
        rmdir(dir4);
    }

    char p[1100];
    snprintf(p, sizeof p, "%s/records.jsonl", dir); unlink(p);
    snprintf(p, sizeof p, "%s/vectors.f32", dir);   unlink(p);
    snprintf(p, sizeof p, "%s/meta.json", dir);     unlink(p);
    rmdir(dir);
}

int main(void)
{
    char dir[] = "/tmp/hcmem_XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }

    float a[4] = {1, 0, 0, 0};  /* agent:A */
    float b[4] = {0, 1, 0, 0};  /* agent:B */
    float s[4] = {1, 1, 0, 0};  /* shared (between a and b) */
    char  idA[HC_MEM_ID_LEN] = {0};

    hc_memory *m = hc_memory_open(dir);
    CHECK(m != NULL, "open");
    CHECK(hc_memory_dim(m) == 0 && hc_memory_count(m) == 0, "empty store: dim 0, count 0");

    hc_mem_record rA = rec("agent:A", "alpha", a, 4, 100);
    CHECK(hc_memory_write(m, &rA, idA) == 0, "write agent:A");
    CHECK(hc_memory_dim(m) == 4, "dimension adopted from first write");
    CHECK(hc_memory_count(m) == 1, "count is 1");

    /* dedup: the same scope|text yields the same id and does not grow the store */
    char          idA2[HC_MEM_ID_LEN] = {0};
    hc_mem_record rA2 = rec("agent:A", "alpha", a, 4, 199);
    CHECK(hc_memory_write(m, &rA2, idA2) == 0 && strcmp(idA, idA2) == 0, "dedup id stable");
    CHECK(hc_memory_count(m) == 1, "dedup keeps count 1");

    hc_mem_record rB = rec("agent:B", "beta", b, 4, 102);
    hc_mem_record rS = rec("shared", "gamma", s, 4, 103);
    CHECK(hc_memory_write(m, &rB, NULL) == 0 && hc_memory_write(m, &rS, NULL) == 0, "write B + shared");
    CHECK(hc_memory_count(m) == 3, "count is 3");

    /* a write at a different dimension is rejected (the one-way door) */
    float         c3[3] = {1, 0, 0};
    hc_mem_record rbad = rec("agent:A", "wrongdim", c3, 3, 104);
    CHECK(hc_memory_write(m, &rbad, NULL) == -1, "dim-mismatch write rejected");

    /* a non-finite vector is never stored (no poisoned vector) */
    float         inf4[4] = {1, 0, 0, 0};
    inf4[2] = INFINITY;
    hc_mem_record rinf = rec("agent:A", "poison", inf4, 4, 105);
    CHECK(hc_memory_write(m, &rinf, NULL) == -1, "non-finite vector rejected");

    /* a degenerate all-zero vector (norm 0, cosine 0) is also rejected on write */
    float         zero4[4] = {0, 0, 0, 0};
    hc_mem_record rzero = rec("agent:A", "zerovec", zero4, 4, 107);
    CHECK(hc_memory_write(m, &rzero, NULL) == -1, "zero-norm vector rejected");

    /* oversized text is rejected */
    char *big = malloc(HC_MEM_TEXT_MAX + 16);
    CHECK(big != NULL, "alloc big text");
    if (big) {
        memset(big, 'x', HC_MEM_TEXT_MAX + 8);
        big[HC_MEM_TEXT_MAX + 8] = '\0';
        hc_mem_record rbig = rec("agent:A", big, a, 4, 106);
        CHECK(hc_memory_write(m, &rbig, NULL) == -1, "oversized text rejected");
        free(big);
    }
    CHECK(hc_memory_count(m) == 3, "rejected writes did not grow the store");

    /* query: q ~ a. Scope filter {agent:A, shared} must EXCLUDE agent:B (scope isolation). */
    float        q[4] = {2, 0, 0, 0}; /* same direction as a */
    const char  *scopes[2] = {"agent:A", "shared"};
    hc_mem_hit  *hits = NULL;
    size_t       n = 0;
    CHECK(hc_memory_query(m, q, 4, scopes, 2, 10, &hits, &n) == 0, "query ok");
    CHECK(n == 2, "scope filter returns only agent:A + shared (not agent:B)");
    CHECK(n >= 1 && strcmp(hits[0].scope, "agent:A") == 0 && hits[0].cosine > 0.99f,
          "top hit is the exact-direction agent:A match");
    int leaked = 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(hits[i].scope, "agent:B") == 0) leaked = 1;
    CHECK(!leaked, "agent:B never leaks through the scope filter");
    hc_memory_hits_free(hits, n);

    /* an unrestricted query (n_scopes==0) sees all three, top still agent:A */
    CHECK(hc_memory_query(m, q, 4, NULL, 0, 10, &hits, &n) == 0 && n == 3, "unrestricted query sees all");
    hc_memory_hits_free(hits, n);

    /* dim-mismatch query rejected */
    float q3[3] = {1, 0, 0};
    CHECK(hc_memory_query(m, q3, 3, NULL, 0, 10, &hits, &n) == -1, "dim-mismatch query rejected");

    /* forget agent:A; persists across reopen */
    CHECK(hc_memory_forget(m, idA) == 0, "forget agent:A");
    CHECK(hc_memory_count(m) == 2, "count drops to 2 after forget");
    CHECK(hc_memory_forget(m, idA) == 0, "forget is idempotent");

    hc_memory_close(m);
    m = hc_memory_open(dir); /* rebuild from the log */
    CHECK(m != NULL, "reopen");
    CHECK(hc_memory_dim(m) == 4, "reopen keeps the dimension");
    CHECK(hc_memory_count(m) == 2, "reopen keeps 2 live records (tombstone honoured)");
    CHECK(hc_memory_query(m, q, 4, scopes, 2, 10, &hits, &n) == 0 && n == 1
              && strcmp(hits[0].scope, "shared") == 0,
          "after forget+reopen, agent:A is gone; shared remains");
    hc_memory_hits_free(hits, n);

    /* list is most-recent-first */
    CHECK(hc_memory_list(m, 10, &hits, &n) == 0 && n == 2, "list returns the 2 live records");
    CHECK(n == 2 && hits[0].created_ms >= hits[1].created_ms, "list is most-recent-first");
    hc_memory_hits_free(hits, n);

    hc_memory_close(m);

    test_model_binding();

    /* tidy the temp store */
    char p[1100];
    snprintf(p, sizeof p, "%s/records.jsonl", dir); unlink(p);
    snprintf(p, sizeof p, "%s/vectors.f32", dir);   unlink(p);
    snprintf(p, sizeof p, "%s/meta.json", dir);     unlink(p);
    rmdir(dir);

    if (g_fails) {
        fprintf(stderr, "hc_memory: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_memory: all checks passed\n");
    return 0;
}
