/* Tests for hc_memory — fully offline (vectors are hand-crafted fixtures; no hc_llm). Covers write +
 * dedup, dim adoption + mismatch reject, the cosine query + the scope filter (an agent cannot read
 * another's scope), bounds (oversized text / non-finite vector rejected), forget, and persistence-replay
 * (close + reopen rebuilds the index from the log). Exit non-zero on any failure. */

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
