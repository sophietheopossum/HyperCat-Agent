/* Tests for hc_fs — the shared POSIX primitives. Roundtrips atomic-write/read, append-concatenation,
 * the size cap, mkdirs, and list_dirs against a temp dir. Exit non-zero on any failure. */

#define _DEFAULT_SOURCE 1

#include "hc_fs.h"

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

int main(void)
{
    char dir[] = "/tmp/hcfs_XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    char sub[1100], file[1200], log[1200];
    snprintf(sub, sizeof sub, "%s/a/b/c", dir);
    CHECK(hc_fs_mkdirs(sub) == 0, "mkdirs -p");

    /* atomic write + read roundtrip */
    snprintf(file, sizeof file, "%s/f.txt", sub);
    CHECK(hc_fs_atomic_write(file, "hello", 5) == 0, "atomic_write");
    size_t n = 0;
    char  *r = hc_fs_read_file(file, 1024, &n);
    CHECK(r && n == 5 && memcmp(r, "hello", 5) == 0, "read_file roundtrip");
    free(r);
    /* size cap: reading with a smaller cap than the file returns NULL */
    CHECK(hc_fs_read_file(file, 4, &n) == NULL, "read_file rejects over-cap");

    /* append concatenates durably */
    snprintf(log, sizeof log, "%s/log.jsonl", sub);
    CHECK(hc_fs_append(log, "one\n", 4) == 0 && hc_fs_append(log, "two\n", 4) == 0, "append x2");
    r = hc_fs_read_file(log, 1024, &n);
    CHECK(r && n == 8 && memcmp(r, "one\ntwo\n", 8) == 0, "append concatenation");
    free(r);

    /* list_dirs sees the subdirectory, not the file */
    char **dirs = NULL;
    size_t nd = 0;
    CHECK(hc_fs_list_dirs(dir, &dirs, &nd) == 0 && nd == 1 && strcmp(dirs[0], "a") == 0, "list_dirs");
    hc_fs_free_list(dirs, nd);

    char ts[32];
    hc_fs_now_iso8601(ts, sizeof ts);
    CHECK(strlen(ts) == 20 && ts[19] == 'Z', "now_iso8601 shape");

    /* SECURITY (P3.0 hardening): the write primitives open the final component O_NOFOLLOW, so a same-uid peer
     * who pre-plants a SYMLINK at the target cannot redirect the write outside the store. */
    char victim[1200], vfull[1400], link[1300], linktmp[1400];
    snprintf(victim, sizeof victim, "%s/victim_dir", dir);
    CHECK(hc_fs_mkdirs(victim) == 0, "make a victim dir");
    snprintf(vfull, sizeof vfull, "%s/stolen", victim);
    /* append THROUGH a planted symlink is refused */
    snprintf(link, sizeof link, "%s/evil_append", sub);
    CHECK(symlink(vfull, link) == 0, "plant evil_append -> victim/stolen");
    CHECK(hc_fs_append(link, "x", 1) != 0, "append refuses a symlinked target (O_NOFOLLOW)");
    CHECK(access(vfull, F_OK) != 0, "the append did NOT land in the victim dir");
    /* atomic_write THROUGH a planted symlink at <path>.tmp is refused */
    snprintf(link, sizeof link, "%s/evil_atomic", sub);
    snprintf(linktmp, sizeof linktmp, "%s/evil_atomic.tmp", sub);
    CHECK(symlink(vfull, linktmp) == 0, "plant evil_atomic.tmp -> victim/stolen");
    CHECK(hc_fs_atomic_write(link, "x", 1) != 0, "atomic_write refuses a symlinked .tmp (O_NOFOLLOW)");
    CHECK(access(vfull, F_OK) != 0, "the atomic_write did NOT land in the victim dir");
    /* list_dirs skips a symlink-to-dir (it is not a real directory) */
    char symdir[1300];
    snprintf(symdir, sizeof symdir, "%s/sym_to_victim", dir);
    CHECK(symlink(victim, symdir) == 0, "plant sym_to_victim -> victim_dir");
    dirs = NULL;
    nd = 0;
    CHECK(hc_fs_list_dirs(dir, &dirs, &nd) == 0, "list_dirs after planting a symlinked dir");
    int saw_sym = 0, saw_victim = 0;
    for (size_t i = 0; i < nd; i++) {
        if (strcmp(dirs[i], "sym_to_victim") == 0) saw_sym = 1;
        if (strcmp(dirs[i], "victim_dir") == 0) saw_victim = 1;
    }
    CHECK(!saw_sym && saw_victim, "list_dirs returns the real dir but SKIPS the symlink");
    hc_fs_free_list(dirs, nd);
    snprintf(link, sizeof link, "%s/evil_append", sub);
    unlink(link);
    unlink(linktmp);
    unlink(symdir);
    rmdir(victim);

    /* tidy */
    unlink(file);
    unlink(log);
    snprintf(file, sizeof file, "%s/a/b/c", dir); rmdir(file);
    snprintf(file, sizeof file, "%s/a/b", dir);   rmdir(file);
    snprintf(file, sizeof file, "%s/a", dir);     rmdir(file);
    rmdir(dir);

    if (g_fails) {
        fprintf(stderr, "hc_fs: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_fs: all checks passed\n");
    return 0;
}
