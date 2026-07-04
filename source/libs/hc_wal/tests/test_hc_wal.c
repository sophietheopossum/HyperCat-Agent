/* test_hc_wal — the P14 WAL primitive gate (offline, deterministic). Proves: an append/replay round-trip;
 * an IDEMPOTENT double-replay (replaying twice yields the same records — recovery can re-run safely); a
 * TORN-TAIL skip (a crash mid-append leaves a newline-less fragment that replay ignores); a traversal-
 * unsafe stream id is REJECTED; a newline inside a record is rejected (it would split the record); list
 * reports the *.wal streams; remove is idempotent. No domain logic — just bytes + paths. */

#include "hc_wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(c, m)                                                                                    \
    do {                                                                                               \
        if (!(c)) {                                                                                    \
            fprintf(stderr, "FAIL: %s\n", (m));                                                        \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

/* append a NUL-terminated literal as one record (its byte length via strlen — no manual counting) */
#define APP(w, s) hc_wal_append((w), (s), strlen(s))

/* a replay sink: collect each record into a fixed array */
struct sink {
    char  lines[64][256];
    size_t lens[64];
    int   n;
};
static int collect(const char *line, size_t len, void *user)
{
    struct sink *s = (struct sink *)user;
    if (s->n >= 64 || len >= 256) return 1; /* stop — bounded test buffer */
    memcpy(s->lines[s->n], line, len);
    s->lines[s->n][len] = '\0';
    s->lens[s->n] = len;
    s->n++;
    return 0;
}

static int count_ids(const char *id, void *user)
{
    (void)id;
    (*(int *)user)++;
    return 0;
}

int main(void)
{
    char dir[] = "/tmp/hc_wal_XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }

    /* --- append/replay round-trip + ordering --- */
    {
        hc_wal *w = hc_wal_open(dir, "ag1");
        CHECK(w != NULL, "open ag1");
        CHECK(APP(w, "{\"t\":\"open\"}") == 0, "append open");
        CHECK(APP(w, "{\"t\":\"done\",\"id\":\"t1\"}") == 0, "append done t1");
        CHECK(APP(w, "{\"t\":\"settled\"}") == 0, "append settled");
        hc_wal_close(w);

        struct sink s = {0};
        CHECK(hc_wal_replay(dir, "ag1", collect, &s) == 0, "replay ag1");
        CHECK(s.n == 3, "replay yields 3 records in order");
        CHECK(s.n == 3 && strcmp(s.lines[0], "{\"t\":\"open\"}") == 0, "record 0 is open");
        CHECK(s.n == 3 && strcmp(s.lines[1], "{\"t\":\"done\",\"id\":\"t1\"}") == 0, "record 1 is done t1");
        CHECK(s.n == 3 && strcmp(s.lines[2], "{\"t\":\"settled\"}") == 0, "record 2 is settled");

        /* IDEMPOTENT double-replay: a second replay yields the identical records (recovery is re-runnable) */
        struct sink s2 = {0};
        hc_wal_replay(dir, "ag1", collect, &s2);
        CHECK(s2.n == 3, "double-replay yields the same 3 records (idempotent)");
    }

    /* --- torn tail: a crash mid-append leaves a newline-less fragment -> replay skips it --- */
    {
        hc_wal *w = hc_wal_open(dir, "torn");
        APP(w, "{\"t\":\"open\"}"); /* a complete record (gets its '\n') */
        hc_wal_close(w);
        /* simulate a torn tail by appending raw bytes WITHOUT a trailing newline, as a crash would leave */
        char p[512];
        snprintf(p, sizeof p, "%s/torn.wal", dir);
        FILE *f = fopen(p, "a");
        CHECK(f != NULL, "reopen torn.wal to plant a torn tail");
        if (f) {
            fputs("{\"t\":\"done\",\"id\":\"part", f); /* a partial record, NO newline */
            fclose(f);
        }
        struct sink s = {0};
        hc_wal_replay(dir, "torn", collect, &s);
        CHECK(s.n == 1, "torn tail (newline-less fragment) is skipped — only the complete record replays");
        CHECK(s.n >= 1 && strcmp(s.lines[0], "{\"t\":\"open\"}") == 0, "the surviving record is the complete one");
    }

    /* --- a record containing a newline is REJECTED (it would split into two records on replay) --- */
    {
        hc_wal *w = hc_wal_open(dir, "nl");
        CHECK(APP(w, "a\nb") == -1, "a newline inside a record is rejected");
        /* and a result with an embedded newline stays ONE record once the caller has escaped it: a JSON
         * string escapes '\n' as the two bytes backslash-n, which contains no real '\n' -> accepted */
        CHECK(APP(w, "{\"r\":\"line1\\nline2\"}") == 0,
              "an escaped newline (\\n, 2 bytes) is fine — stays one record");
        hc_wal_close(w);
        struct sink s = {0};
        hc_wal_replay(dir, "nl", collect, &s);
        CHECK(s.n == 1, "the escaped-newline result is a single record");
    }

    /* --- traversal-unsafe / empty / over-long stream ids are REJECTED at open AND remove --- */
    {
        CHECK(hc_wal_open(dir, "../escape") == NULL, "open rejects a '..' id (traversal)");
        CHECK(hc_wal_open(dir, "a/b") == NULL, "open rejects a '/' id (traversal)");
        CHECK(hc_wal_open(dir, "") == NULL, "open rejects an empty id");
        char big[200];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        CHECK(hc_wal_open(dir, big) == NULL, "open rejects an over-long id");
        CHECK(hc_wal_remove(dir, "../escape") == -1, "remove rejects a traversal id");
    }

    /* --- list reports the *.wal streams; remove deletes one (idempotently) --- */
    {
        int n = 0;
        CHECK(hc_wal_list(dir, count_ids, &n) == 0, "list scans the dir");
        CHECK(n >= 3, "list finds the streams written above (ag1, torn, nl)");
        int before = n;
        CHECK(hc_wal_remove(dir, "ag1") == 0, "remove ag1");
        n = 0;
        hc_wal_list(dir, count_ids, &n);
        CHECK(n == before - 1, "list shows one fewer stream after remove");
        CHECK(hc_wal_remove(dir, "ag1") == 0, "remove is idempotent (absent = success)");

        struct sink s = {0};
        CHECK(hc_wal_replay(dir, "ag1", collect, &s) == 0 && s.n == 0,
              "replaying a removed stream yields nothing (absent = 0 records)");
    }

    /* cleanup (best-effort) */
    hc_wal_remove(dir, "torn");
    hc_wal_remove(dir, "nl");
    rmdir(dir);

    if (g_fails == 0) printf("test_hc_wal: OK\n");
    return g_fails ? 1 : 0;
}
