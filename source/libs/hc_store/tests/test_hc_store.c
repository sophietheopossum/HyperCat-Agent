/* Unit tests for hc_store. Uses a per-pid scratch root so reruns and parallel tests do not
 * collide. Exit non-zero on any failure (CTest reads the exit code). */

#include "hc_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL: %s\n", (msg));                                           \
            g_fails++;                                                                      \
        }                                                                                   \
    } while (0)

int main(void)
{
    char root[256];
    snprintf(root, sizeof root, "hc_store_test_%ld", (long)getpid());

    hc_store *st = hc_store_open(root);
    CHECK(st != NULL, "store open");
    if (!st) return 1;

    hc_session *s = hc_session_new(st, "Test Session", "openrouter/test-model");
    CHECK(s != NULL, "session new");
    if (!s) {
        hc_store_close(st);
        return 1;
    }
    char id[80];
    snprintf(id, sizeof id, "%s", hc_session_id(s));

    CHECK(hc_session_append(s, "user", "hello"), "append user");
    CHECK(hc_session_append(s, "assistant", "line one\nline two"), "append assistant (multiline)");
    CHECK(hc_session_count(s) == 2, "count after append");

    const char *role = NULL, *content = NULL;
    CHECK(hc_session_message(s, 0, &role, &content) && strcmp(role, "user") == 0
              && strcmp(content, "hello") == 0,
          "message 0 contents");
    hc_session_free(s);

    /* reload from disk and confirm the transcript round-trips, including the embedded newline */
    hc_session *r = hc_session_load(st, id);
    CHECK(r != NULL, "session load");
    CHECK(r && hc_session_count(r) == 2, "reloaded count");
    CHECK(r && hc_session_message(r, 1, &role, &content) && strcmp(role, "assistant") == 0
              && strcmp(content, "line one\nline two") == 0,
          "reloaded multiline message survives JSONL");
    hc_session_free(r);

    /* a traversal id must be refused (root/<id> path escape): load returns NULL, not an outside read */
    CHECK(hc_session_load(st, "../../etc") == NULL, "load refuses a traversal id");
    CHECK(hc_session_load(st, "a/b") == NULL, "load refuses an id with a slash");
    CHECK(hc_session_load(st, "") == NULL, "load refuses an empty id");

    /* listing finds the session with its turn count */
    hc_session_info *infos = NULL;
    size_t n = 0;
    CHECK(hc_store_list(st, &infos, &n) == 0, "list ok");
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(infos[i].id, id) == 0) {
            found = 1;
            CHECK(infos[i].turns == 1, "one user turn counted");
            CHECK(strcmp(infos[i].title, "Test Session") == 0, "title persisted");
        }
    }
    CHECK(found, "session present in listing");
    hc_store_list_free(infos, n);

    /* P3a: rename — set a new title (incl. a quote, to prove JSON escaping) and confirm it round-trips via the
     * listing while the turn count is preserved. */
    {
        hc_session *rn = hc_session_load(st, id);
        CHECK(rn != NULL, "rename: load");
        CHECK(rn && hc_session_set_title(rn, "Renamed \"chat\""), "rename: set_title persists");
        hc_session_free(rn);
        hc_session_info *ri = NULL;
        size_t           rnn = 0;
        int              rfound = 0;
        CHECK(hc_store_list(st, &ri, &rnn) == 0, "rename: list ok");
        for (size_t i = 0; i < rnn; i++)
            if (strcmp(ri[i].id, id) == 0) {
                rfound = 1;
                CHECK(strcmp(ri[i].title, "Renamed \"chat\"") == 0, "rename: new title persisted (JSON-safe)");
                CHECK(ri[i].turns == 1, "rename: turn count preserved");
            }
        CHECK(rfound, "rename: session still listed");
        hc_store_list_free(ri, rnn);
    }

    /* P3b/F1 regression: hc_store_list must report the DIRECTORY NAME as the id, never the in-file "id" field —
     * else a same-uid attacker who plants a meta.json could decouple the displayed row from the conversation that
     * delete/rename actually act on (a confused-deputy). Plant a dir whose meta.json "id" disagrees with its name;
     * the listing must surface the name, not the planted value. */
    {
        char planted_dir[320];
        snprintf(planted_dir, sizeof planted_dir, "%s/sess-planted", root);
        CHECK(mkdir(planted_dir, 0700) == 0, "plant: mkdir session dir");
        char planted_meta[400];
        snprintf(planted_meta, sizeof planted_meta, "%s/meta.json", planted_dir);
        FILE *pf = fopen(planted_meta, "w");
        CHECK(pf != NULL, "plant: open meta.json");
        if (pf) {
            fputs("{\"id\":\"sess-SPOOFED\",\"title\":\"planted\",\"turns\":0}", pf);
            fclose(pf);
        }
        hc_session_info *pi = NULL;
        size_t           pnn = 0;
        int              name_found = 0, spoof_found = 0;
        CHECK(hc_store_list(st, &pi, &pnn) == 0, "plant: list ok");
        for (size_t i = 0; i < pnn; i++) {
            if (strcmp(pi[i].id, "sess-planted") == 0) name_found = 1;
            if (strcmp(pi[i].id, "sess-SPOOFED") == 0) spoof_found = 1;
        }
        CHECK(name_found, "plant: listing reports the directory name as id");
        CHECK(!spoof_found, "plant: listing never reports the in-file (spoofable) id");
        hc_store_list_free(pi, pnn);
    }

    hc_store_close(st);

    if (g_fails) {
        fprintf(stderr, "hc_store: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_store: all checks passed\n");
    return 0;
}
