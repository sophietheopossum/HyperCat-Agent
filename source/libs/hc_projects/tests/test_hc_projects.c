/* test_hc_projects — the project index over a tmp dir: slugify + traversal rejection, create/get/list, dedup,
 * rename/touch/delete (+ the touched ordering), the active pointer, index replay across reopen, and orphan
 * adoption (a dir with no index line). No network, no clock (now_ms is passed in). Exit non-zero on any fail. */

#include "hc_projects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c, m)                                                                                            \
    do {                                                                                                       \
        if (!(c)) {                                                                                            \
            fprintf(stderr, "FAIL: %s\n", (m));                                                                \
            g_fail++;                                                                                          \
        }                                                                                                      \
    } while (0)

int main(void)
{
    char dir[] = "/tmp/hc_projg_XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }

    /* --- slugify: display -> traversal-safe id; the display name is never a path component --- */
    char id[HC_PROJECT_ID_CAP];
    CHECK(hc_projects_slug("My Project!", id, sizeof id) == 0 && strcmp(id, "my-project") == 0,
          "slug 'My Project!' -> 'my-project'");
    CHECK(hc_projects_slug("  ../../etc/passwd  ", id, sizeof id) == 0 && !strchr(id, '/') &&
              !strstr(id, ".."),
          "slug of a traversal string strips '/' and '..'");
    CHECK(hc_projects_slug("Trailing---", id, sizeof id) == 0 && strcmp(id, "trailing") == 0,
          "slug trims trailing dashes");
    CHECK(hc_projects_slug("!!!", id, sizeof id) != 0, "a name with no usable chars -> slug fails");

    hc_projects *p = hc_projects_open(dir);
    CHECK(p != NULL, "open");
    if (!p) return 1;

    /* --- create + get --- */
    hc_project a;
    CHECK(hc_projects_create(p, "Alpha Build", 1000, &a) == 0 && strcmp(a.id, "alpha-build") == 0,
          "create 'Alpha Build' -> id 'alpha-build'");
    CHECK(a.created_ms == 1000 && a.touched_ms == 1000, "create stamps created/touched");
    hc_project got;
    CHECK(hc_projects_get(p, "alpha-build", &got) == 0 && strcmp(got.display, "Alpha Build") == 0,
          "get returns the created project with its display name");
    CHECK(hc_projects_get(p, "nope", &got) != 0, "get of an unknown id fails");

    /* --- dedup: same display -> distinct id --- */
    hc_project a2;
    CHECK(hc_projects_create(p, "Alpha Build", 1100, &a2) == 0 && strcmp(a2.id, "alpha-build-2") == 0,
          "a second 'Alpha Build' dedups to 'alpha-build-2'");

    /* --- the project dir is <root>/projects/<id> (id, never the display) --- */
    char pdir[1200];
    CHECK(hc_projects_dir(p, "alpha-build", pdir, sizeof pdir) == 0, "dir() builds a path");
    struct stat st;
    CHECK(stat(pdir, &st) == 0 && S_ISDIR(st.st_mode), "create made the project subtree dir");
    char tiny[4];
    CHECK(hc_projects_dir(p, "alpha-build", tiny, sizeof tiny) != 0, "dir() returns -1 when the path won't fit");
    CHECK(hc_projects_dir(p, "../escape", pdir, sizeof pdir) != 0, "dir() rejects a non-id_ok id");

    /* --- a third project, then list is most-recently-touched first --- */
    hc_project b;
    CHECK(hc_projects_create(p, "Beta", 1200, &b) == 0, "create 'Beta'");
    hc_project *list = NULL;
    int         n = hc_projects_list(p, &list);
    CHECK(n == 3, "list has 3 live projects");
    CHECK(n == 3 && strcmp(list[0].id, "beta") == 0, "list is touched-desc: 'beta' (1200) first");
    hc_projects_free_list(list);

    /* --- touch bumps the order --- */
    CHECK(hc_projects_touch(p, "alpha-build", 9000) == 0, "touch alpha-build to 9000");
    n = hc_projects_list(p, &list);
    CHECK(n == 3 && strcmp(list[0].id, "alpha-build") == 0, "after touch, alpha-build sorts first");
    hc_projects_free_list(list);

    /* --- rename changes display only; id immutable --- */
    CHECK(hc_projects_rename(p, "beta", "Beta Prime") == 0, "rename beta");
    CHECK(hc_projects_get(p, "beta", &got) == 0 && strcmp(got.display, "Beta Prime") == 0,
          "rename updated the display; id is still 'beta'");

    /* --- active pointer: set/get; set rejects an unknown id (now_ms stays monotonic: activation TOUCHES) --- */
    CHECK(hc_projects_set_active(p, "ghost", 9400) != 0, "set_active rejects an unknown id");
    CHECK(hc_projects_set_active(p, "alpha-build", 9500) == 0, "set_active to a live project (touches it to 9500)");
    char act[HC_PROJECT_ID_CAP];
    CHECK(hc_projects_get_active(p, act, sizeof act) == 0 && strcmp(act, "alpha-build") == 0,
          "get_active returns the set id");

    /* --- delete: refuses the active project, tombstones a non-active one --- */
    CHECK(hc_projects_delete(p, "alpha-build") != 0, "delete refuses the ACTIVE project");
    CHECK(hc_projects_delete(p, "beta") == 0, "delete a non-active project");
    CHECK(hc_projects_get(p, "beta", &got) != 0, "a deleted project is gone from get");
    CHECK(hc_projects_count(p) == 2, "count excludes the tombstoned project");

    hc_projects_close(p);

    /* --- replay across reopen: the state persists from index.jsonl --- */
    p = hc_projects_open(dir);
    CHECK(p != NULL, "reopen");
    CHECK(p && hc_projects_get(p, "alpha-build", &got) == 0 && got.touched_ms == 9500,
          "reopen replays alpha-build with touched=9500 (the activation touch)");
    CHECK(p && hc_projects_get(p, "beta", &got) != 0, "reopen keeps beta tombstoned (no revival)");
    CHECK(p && hc_projects_get_active(p, act, sizeof act) == 0 && strcmp(act, "alpha-build") == 0,
          "reopen reads the active pointer");

    /* --- orphan adoption: a projects/<id> dir with no index line is adopted on open --- */
    char orphan[1200];
    snprintf(orphan, sizeof orphan, "%s/projects/orphan-x", dir);
    CHECK(mkdir(orphan, 0700) == 0, "make an orphan project dir by hand");
    hc_projects_close(p);
    p = hc_projects_open(dir);
    CHECK(p != NULL, "reopen after planting an orphan");
    CHECK(p && hc_projects_get(p, "orphan-x", &got) == 0, "the orphan dir was adopted into the index");

    /* --- a tombstoned project's lingering dir is NOT revived as an orphan (delete only tombstones; the dir
     *     from create('Beta') still lingers on disk) --- */
    CHECK(hc_projects_dir(p, "beta", pdir, sizeof pdir) == 0 && stat(pdir, &st) == 0 && S_ISDIR(st.st_mode),
          "the tombstoned 'beta' dir still lingers on disk (delete tombstones, doesn't remove)");
    hc_projects_close(p);
    p = hc_projects_open(dir);
    CHECK(p && hc_projects_get(p, "beta", &got) != 0, "the tombstoned 'beta' stays deleted despite its dir");

    /* --- SECURITY (P3.0 review): a SYMLINK planted in projects/ is NOT adopted as a project (it is not a real
     *     owned dir) — so a same-uid peer can't smuggle an out-of-jail path in as a project subtree --- */
    char sympath[1200];
    snprintf(sympath, sizeof sympath, "%s/projects/evil-link", dir);
    CHECK(symlink("/tmp", sympath) == 0, "plant a symlink projects/evil-link -> /tmp");
    hc_projects_close(p);
    p = hc_projects_open(dir);
    CHECK(p != NULL, "reopen after planting a symlink");
    CHECK(p && hc_projects_get(p, "evil-link", &got) != 0, "a planted symlink is NOT adopted as a project");

    /* --- torn tail: a crash or a failed append can leave index.jsonl ending mid-line. The next record must
     *     not be glued onto it, or replay skips both: a lost tombstone brings a deleted project back, and a
     *     lost create or rename never happened. Both ways in: a tail found at open, and one left while open. */
    char idx[1200];
    snprintf(idx, sizeof idx, "%s/projects/index.jsonl", dir);
    const char frag[] = "{\"id\":\"half-writ"; /* a line cut off before its '\n' */
    hc_project g;
    CHECK(p && hc_projects_create(p, "Gamma", 6000, &g) == 0, "create gamma");
    hc_projects_close(p);
    FILE *tf = fopen(idx, "ab");
    CHECK(tf && fwrite(frag, 1, sizeof frag - 1, tf) == sizeof frag - 1, "plant a torn index tail");
    if (tf) fclose(tf);
    p = hc_projects_open(dir);
    CHECK(p && hc_projects_get(p, "gamma", &got) == 0, "a torn tail does not cost the lines before it");
    CHECK(p && hc_projects_delete(p, "gamma") == 0, "delete gamma after a torn tail");
    hc_projects_close(p);
    p = hc_projects_open(dir);
    CHECK(p && hc_projects_get(p, "gamma", &got) != 0, "a tombstone written after a torn tail stays deleted");
    /* now torn while the handle is open; a rename shows the loss (a lost create would be re-adopted from
     * its dir on reopen, which hides it) */
    CHECK(p && hc_projects_create(p, "Delta", 7000, &g) == 0, "create delta");
    tf = fopen(idx, "ab");
    CHECK(tf && fwrite(frag, 1, sizeof frag - 1, tf) == sizeof frag - 1, "plant a torn tail under an open handle");
    if (tf) fclose(tf);
    CHECK(p && hc_projects_rename(p, "delta", "Delta Renamed") == 0, "rename delta after a torn tail");
    hc_projects_close(p);
    p = hc_projects_open(dir);
    CHECK(p && hc_projects_get(p, "delta", &got) == 0 && strcmp(got.display, "Delta Renamed") == 0,
          "a rename written after a torn tail survives a reopen");

    /* a COMPLETE record torn only before its '\n' (a crash between the record and its newline) is applied:
     * the next append ends it instead of gluing onto it, and both records replay */
    const char whole[] = "{\"created\":8000,\"deleted\":false,\"display\":\"Eps\",\"id\":\"eps\",\"touched\":8000}";
    tf = fopen(idx, "ab");
    CHECK(tf && fwrite(whole, 1, sizeof whole - 1, tf) == sizeof whole - 1, "plant a complete record with no newline");
    if (tf) fclose(tf);
    CHECK(p && hc_projects_rename(p, "delta", "Delta Again") == 0, "rename delta after a newline-less record");
    hc_projects_close(p);
    p = hc_projects_open(dir);
    CHECK(p && hc_projects_get(p, "eps", &got) == 0 && strcmp(got.display, "Eps") == 0,
          "the newline-less complete record is applied");
    CHECK(p && hc_projects_get(p, "delta", &got) == 0 && strcmp(got.display, "Delta Again") == 0,
          "and the record after it is too");

    if (p) hc_projects_close(p);

    if (g_fail) {
        fprintf(stderr, "test_hc_projects: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_hc_projects: all checks passed\n");
    return 0;
}
