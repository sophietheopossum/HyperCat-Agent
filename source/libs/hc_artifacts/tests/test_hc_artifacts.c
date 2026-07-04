/* Unit tests for hc_artifacts. Uses a per-pid /tmp scratch root so reruns/parallel tests don't collide.
 * SHA-256 correctness is checked end-to-end through put() against the standard NIST vectors (the hash
 * header is private to the module). Exit non-zero on any failure (CTest reads the exit code). */

#include "hc_artifacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            fprintf(stderr, "FAIL: %s\n", (msg));                                                   \
            g_fails++;                                                                              \
        }                                                                                           \
    } while (0)

int main(void)
{
    char root[256];
    snprintf(root, sizeof root, "/tmp/hc_artifacts_test_%ld", (long)getpid());

    hc_artifacts *a = hc_artifacts_open(root);
    CHECK(a != NULL, "open");
    if (!a) return 1;

    char id[HC_ARTIFACT_ID_LEN];

    /* --- SHA-256 correctness (the content id) against the canonical NIST vectors --- */
    CHECK(hc_artifacts_put(a, "", 0, id) == 0 &&
              strcmp(id, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "sha256(\"\") id");
    CHECK(hc_artifacts_put(a, "abc", 3, id) == 0 &&
              strcmp(id, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "sha256(\"abc\") id (single block)");
    /* the standard 56-byte two-block vector — exercises the cross-boundary padding path */
    const char *nist2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hc_artifacts_put(a, nist2, 56, id) == 0 &&
              strcmp(id, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
          "sha256 NIST 2-block vector (56 bytes)");

    /* --- dedup + get roundtrip --- */
    char id2[HC_ARTIFACT_ID_LEN];
    CHECK(hc_artifacts_put(a, "hello world", 11, id) == 0, "put hello");
    CHECK(hc_artifacts_put(a, "hello world", 11, id2) == 0 && strcmp(id, id2) == 0,
          "identical bytes dedup to the same id");
    size_t n = 0;
    void  *got = hc_artifacts_get(a, id, &n);
    CHECK(got && n == 11 && memcmp(got, "hello world", 11) == 0, "get roundtrips the bytes");
    free(got);

    /* --- id validation / traversal refusal --- */
    CHECK(hc_artifacts_get(a, "not-a-valid-id", &n) == NULL, "get rejects a non-hex id");
    CHECK(hc_artifacts_get(a, "../../etc/passwd", &n) == NULL, "get refuses a traversal id");
    CHECK(hc_artifacts_get(a, "deadbeef", &n) == NULL, "get rejects a too-short id");

    /* --- provenance: record + by_task + history --- */
    hc_provenance p = {0};
    p.agent = "agent:A";
    p.task = "t1";
    p.agenda = "ag";
    p.tool = "fs_write";
    p.label = "hello.txt";
    p.size = 11;
    CHECK(hc_artifacts_record(a, id, &p) == 0, "record provenance");

    char id3[HC_ARTIFACT_ID_LEN];
    hc_artifacts_put(a, "other content", 13, id3);
    hc_provenance p2 = {0};
    p2.agent = "agent:B";
    p2.task = "t2";
    p2.tool = "fs_write";
    p2.label = "other.txt";
    p2.size = 13;
    CHECK(hc_artifacts_record(a, id3, &p2) == 0, "record second provenance");

    hc_artifact_rec *recs = NULL;
    size_t           nr = 0;
    CHECK(hc_artifacts_by_task(a, "t1", &recs, &nr) == 0 && nr == 1, "by_task t1 -> 1 row");
    CHECK(nr == 1 && strcmp(recs[0].id, id) == 0 && strcmp(recs[0].agent, "agent:A") == 0 &&
              strcmp(recs[0].label, "hello.txt") == 0 && recs[0].size == 11,
          "by_task t1 fields");
    hc_artifacts_recs_free(recs, nr);

    CHECK(hc_artifacts_by_task(a, "absent", &recs, &nr) == 0 && nr == 0, "by_task absent -> 0 rows");
    hc_artifacts_recs_free(recs, nr);

    CHECK(hc_artifacts_history(a, "hello.txt", &recs, &nr) == 0 && nr == 1, "history by label");
    hc_artifacts_recs_free(recs, nr);

    /* --- most-recent-first: a new version of hello.txt is returned before the old --- */
    char idv2[HC_ARTIFACT_ID_LEN];
    hc_artifacts_put(a, "hello world v2", 14, idv2);
    hc_provenance pv2 = {0};
    pv2.agent = "agent:A";
    pv2.task = "t1";
    pv2.tool = "fs_write";
    pv2.label = "hello.txt";
    pv2.size = 14;
    hc_artifacts_record(a, idv2, &pv2);
    CHECK(hc_artifacts_history(a, "hello.txt", &recs, &nr) == 0 && nr == 2 &&
              strcmp(recs[0].id, idv2) == 0,
          "history is most-recent-first");
    hc_artifacts_recs_free(recs, nr);

    /* --- recent (across all tasks): all rows, then capped to max --- */
    CHECK(hc_artifacts_recent(a, 100, &recs, &nr) == 0 && nr == 3, "recent returns all 3 rows");
    CHECK(nr == 3 && strcmp(recs[0].id, idv2) == 0, "recent is most-recent-first");
    hc_artifacts_recs_free(recs, nr);
    CHECK(hc_artifacts_recent(a, 1, &recs, &nr) == 0 && nr == 1, "recent caps to max");
    hc_artifacts_recs_free(recs, nr);

    /* --- persistence across reopen (object + provenance survive) --- */
    hc_artifacts_close(a);
    a = hc_artifacts_open(root);
    CHECK(a != NULL, "reopen");
    if (a) {
        got = hc_artifacts_get(a, id, &n);
        CHECK(got && n == 11, "object persists across reopen");
        free(got);
        CHECK(hc_artifacts_by_task(a, "t1", &recs, &nr) == 0 && nr == 2,
              "provenance persists across reopen");
        hc_artifacts_recs_free(recs, nr);
        hc_artifacts_close(a);
    }

    if (g_fails) {
        fprintf(stderr, "hc_artifacts: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_artifacts: all checks passed\n");
    return 0;
}
