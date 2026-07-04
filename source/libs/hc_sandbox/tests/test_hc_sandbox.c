/* Escape-battery tests for hc_sandbox. Builds a per-pid scratch jail under /tmp with a sibling
 * "evil" tree and real symlinks, then asserts that every traversal / symlink / absolute-path
 * escape is refused and every legitimate in-jail access works. Includes a bounded live TOCTOU
 * race (a path component flips between a real in-jail file and a symlink to an outside file) to
 * guard the no-follow walk. Exit non-zero on any failure (CTest reads the exit code). Offline. */

#define _DEFAULT_SOURCE 1

#include "hc_sandbox.h"

#include <fcntl.h>
#include <pthread.h>
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

static void write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        ssize_t n = write(fd, content, strlen(content));
        (void)n;
        close(fd);
    }
}

/* Create a symlink for test setup, FAILING the suite if it could not be made — otherwise a
 * silently-absent symlink would turn an escape assertion into a false pass-by-absence. */
static void mk_symlink(const char *target, const char *linkpath)
{
    unlink(linkpath); /* clear any leftover from a crashed prior run with the same pid */
    if (symlink(target, linkpath) != 0) {
        fprintf(stderr, "FAIL: setup symlink %s -> %s\n", linkpath, target);
        g_fails++;
    }
}

/* Flips `slot` between a real file containing "INSIDE" and a symlink pointing at `outlink`
 * (a file outside the jail containing "OUTSIDE"), as fast as it can, until told to stop. */
struct race_ctx {
    char slot[512];
    char outlink[512];
    volatile int stop;
};

static void *race_swapper(void *arg)
{
    struct race_ctx *c = arg;
    for (unsigned i = 0; !c->stop; i++) {
        unlink(c->slot);
        if (i & 1u) {
            symlink(c->outlink, c->slot); /* slot becomes a symlink pointing outside the jail */
        } else {
            int fd = open(c->slot, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) {
                ssize_t w = write(fd, "INSIDE", 6);
                (void)w;
                close(fd);
            }
        }
    }
    return NULL;
}

int main(void)
{
    long pid = (long)getpid();
    char root[256], evil[256], path[512], target[512], link[512];

    snprintf(root, sizeof root, "/tmp/hcsb_%ld", pid);
    snprintf(evil, sizeof evil, "/tmp/hcsb_%ld_evil", pid); /* sibling: root is a name-prefix */

    mkdir(root, 0700);
    mkdir(evil, 0700);
    snprintf(path, sizeof path, "%s/sub", root);          mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/sub/inner", root);    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/sub/file.txt", root); write_file(path, "hello-jail");
    snprintf(path, sizeof path, "%s/secret.txt", evil);   write_file(path, "TOPSECRET");

    snprintf(link, sizeof link, "%s/link_out", root);
    mk_symlink(evil, link);                               /* -> outside dir   (middle escape) */
    snprintf(link, sizeof link, "%s/link_file", root);
    snprintf(target, sizeof target, "%s/secret.txt", evil);
    mk_symlink(target, link);                             /* -> outside file  (final escape)  */
    snprintf(link, sizeof link, "%s/link_inner", root);
    snprintf(target, sizeof target, "%s/sub/inner", root);
    mk_symlink(target, link);                             /* -> inside dir    (legitimate)    */
    snprintf(link, sizeof link, "%s/dangling", root);
    snprintf(target, sizeof target, "%s/planted.txt", evil); /* creatable, OUTSIDE the jail   */
    mk_symlink(target, link);                             /* dangling -> O_CREAT must not escape */

    hc_sandbox_status err = HC_SANDBOX_OK;
    hc_sandbox *sb = hc_sandbox_open(root, &err);
    CHECK(sb != NULL && err == HC_SANDBOX_OK, "sandbox open");
    if (!sb) return 1;

    char out[HC_SANDBOX_PATH_MAX];
    hc_sandbox_fd fd = HC_SANDBOX_FD_INVALID;

    /* --- legitimate access --- */
    CHECK(hc_sandbox_resolve(sb, "sub/file.txt", out, sizeof out) == HC_SANDBOX_OK,
          "resolve legit file");
    CHECK(hc_sandbox_open_fd(sb, "sub/file.txt", O_RDONLY, 0, &fd) == HC_SANDBOX_OK && fd >= 0,
          "open legit file");
    if (fd >= 0) {
        char buf[32];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n < 0) n = 0;
        buf[n] = '\0';
        CHECK(strcmp(buf, "hello-jail") == 0, "read legit file contents");
        close(fd);
    }
    CHECK(hc_sandbox_resolve(sb, ".", out, sizeof out) == HC_SANDBOX_OK, "resolve root '.'");
    CHECK(hc_sandbox_open_fd(sb, ".", O_RDONLY, 0, &fd) == HC_SANDBOX_OK, "open root '.'");
    if (fd >= 0) close(fd);

    /* --- traversal / absolute-path escapes --- */
    CHECK(hc_sandbox_resolve(sb, "..", out, sizeof out) == HC_SANDBOX_ERR_ESCAPE,
          "'..' escapes");
    CHECK(hc_sandbox_resolve(sb, "../../../etc", out, sizeof out) == HC_SANDBOX_ERR_ESCAPE,
          "deep '..' to /etc escapes");
    CHECK(hc_sandbox_resolve(sb, "/etc/passwd", out, sizeof out) == HC_SANDBOX_ERR_ESCAPE,
          "absolute /etc/passwd escapes");

    /* --- symlink escapes and the resolve()/open_fd() divergence --- */
    CHECK(hc_sandbox_resolve(sb, "link_out/secret.txt", out, sizeof out) == HC_SANDBOX_ERR_ESCAPE,
          "middle-component symlink to outside escapes");
    CHECK(hc_sandbox_open_fd(sb, "link_file", O_RDONLY, 0, &fd) == HC_SANDBOX_ERR_ESCAPE,
          "final-component symlink to outside escapes");
    CHECK(hc_sandbox_open_fd(sb, "dangling", O_RDONLY, 0, &fd) == HC_SANDBOX_ERR_SYMLINK,
          "dangling symlink refused at open");
    CHECK(hc_sandbox_resolve(sb, "dangling", out, sizeof out) == HC_SANDBOX_ERR_SYMLINK,
          "resolve refuses a dangling final-component symlink (emits no escaping path)");
    CHECK(hc_sandbox_open_fd(sb, "dangling", O_WRONLY | O_CREAT, 0600, &fd) == HC_SANDBOX_ERR_SYMLINK,
          "O_CREAT through a dangling symlink is refused");
    snprintf(path, sizeof path, "%s/planted.txt", evil);
    CHECK(access(path, F_OK) != 0, "O_CREAT did not create the file outside the jail");
    CHECK(hc_sandbox_resolve(sb, "link_inner", out, sizeof out) == HC_SANDBOX_OK,
          "inside-jail symlink resolves to its in-jail target");
    CHECK(hc_sandbox_open_fd(sb, "link_inner", O_RDONLY, 0, &fd) == HC_SANDBOX_OK,
          "inside-jail symlink opens");
    if (fd >= 0) close(fd);

    /* --- prefix confusion: /tmp/hcsb_<pid> must not match sibling /tmp/hcsb_<pid>_evil --- */
    snprintf(path, sizeof path, "../hcsb_%ld_evil/secret.txt", pid);
    CHECK(hc_sandbox_resolve(sb, path, out, sizeof out) == HC_SANDBOX_ERR_ESCAPE,
          "sibling-prefix path escapes");

    /* --- non-existent targets --- */
    CHECK(hc_sandbox_resolve(sb, "newfile.txt", out, sizeof out) == HC_SANDBOX_OK,
          "creatable leaf resolves");
    CHECK(hc_sandbox_resolve(sb, "nodir/file.txt", out, sizeof out) == HC_SANDBOX_ERR_NOT_FOUND,
          "missing intermediate dir is NOT_FOUND");
    CHECK(hc_sandbox_open_fd(sb, "made.txt", O_WRONLY | O_CREAT, 0600, &fd) == HC_SANDBOX_OK,
          "create file inside jail");
    if (fd >= 0) close(fd);
    snprintf(path, sizeof path, "%s/made.txt", root);
    struct stat stt;
    CHECK(stat(path, &stt) == 0 && S_ISREG(stt.st_mode), "created file exists inside root");

    /* --- invalid inputs --- */
    CHECK(hc_sandbox_resolve(sb, "", out, sizeof out) == HC_SANDBOX_ERR_INVALID, "empty invalid");
    CHECK(hc_sandbox_resolve(sb, NULL, out, sizeof out) == HC_SANDBOX_ERR_INVALID, "NULL invalid");

    /* embedded NUL truncates to "a"; it must not smuggle a traversal */
    {
        char smuggle[16];
        memcpy(smuggle, "a\0/../../etc", 12);
        CHECK(hc_sandbox_resolve(sb, smuggle, out, sizeof out) != HC_SANDBOX_ERR_ESCAPE,
              "embedded-NUL does not smuggle an escape");
    }

    /* over-long path */
    {
        char big[HC_SANDBOX_PATH_MAX + 64];
        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        CHECK(hc_sandbox_resolve(sb, big, out, sizeof out) == HC_SANDBOX_ERR_TOO_LONG,
              "over-long path rejected");
    }

    /* --- jailed directory listing (hc_sandbox_list) --- */
    {
        hc_sandbox_dirent *ents = NULL;
        size_t             ne = 0;
        CHECK(hc_sandbox_list(sb, "sub", &ents, &ne) == HC_SANDBOX_OK, "list sub/ ok");
        CHECK(ne == 2, "sub/ lists two entries (inner, file.txt)");
        int saw_dir = 0, saw_file = 0;
        for (size_t i = 0; i < ne; i++) {
            if (strcmp(ents[i].name, "inner") == 0 && ents[i].is_dir) saw_dir = 1;
            if (strcmp(ents[i].name, "file.txt") == 0 && !ents[i].is_dir && ents[i].size > 0)
                saw_file = 1;
        }
        CHECK(saw_dir, "listing reports the subdirectory as a dir");
        CHECK(saw_file, "listing reports the file with a non-zero size");
        hc_sandbox_list_free(ents);

        /* listing must not escape the jail: an escaping symlink is refused like open_fd */
        ents = NULL;
        ne = 0;
        CHECK(hc_sandbox_list(sb, "link_file", &ents, &ne) != HC_SANDBOX_OK,
              "list refuses an escaping symlink");
        hc_sandbox_list_free(ents); /* NULL-safe on the error path */

        /* listing a regular file (not a directory) errors, returns nothing */
        ents = NULL;
        ne = 0;
        CHECK(hc_sandbox_list(sb, "sub/file.txt", &ents, &ne) != HC_SANDBOX_OK,
              "list of a non-directory errors");
        CHECK(ents == NULL && ne == 0, "failed list yields no entries");
    }

    /* --- hc_sandbox_mkdirs (mkdir -p inside the jail; the deliverable-subdir enabler) --- */
    {
        /* nested creation succeeds, is idempotent, and a file then lands under the new chain */
        CHECK(hc_sandbox_mkdirs(sb, "md/a/b", 0700) == HC_SANDBOX_OK, "mkdirs nested chain");
        CHECK(hc_sandbox_mkdirs(sb, "md/a/b", 0700) == HC_SANDBOX_OK, "mkdirs idempotent");
        snprintf(path, sizeof path, "%s/md/a/b", root);
        CHECK(stat(path, &stt) == 0 && S_ISDIR(stt.st_mode), "mkdirs created the directory chain on disk");
        CHECK(hc_sandbox_open_fd(sb, "md/a/b/out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd) ==
                  HC_SANDBOX_OK,
              "a subdir deliverable write succeeds after mkdirs");
        if (fd >= 0) close(fd);

        /* escapes are refused exactly like the open path */
        CHECK(hc_sandbox_mkdirs(sb, "../escape", 0700) == HC_SANDBOX_ERR_ESCAPE, "mkdirs '..' escapes");
        CHECK(hc_sandbox_mkdirs(sb, "/abs/dir", 0700) == HC_SANDBOX_ERR_INVALID, "mkdirs absolute refused");
        CHECK(hc_sandbox_mkdirs(sb, "", 0700) == HC_SANDBOX_ERR_INVALID, "mkdirs empty refused");
        /* a symlink component is NEVER followed (not even a legit in-jail one) — no escape, no surprise.
         * It is classified as ERR_SYMLINK (an escape ATTEMPT, distinct from a benign error) so a security
         * log can alert on it, even though O_DIRECTORY|O_NOFOLLOW makes the kernel return ENOTDIR here. */
        CHECK(hc_sandbox_mkdirs(sb, "link_out/x", 0700) == HC_SANDBOX_ERR_SYMLINK,
              "mkdirs through an escaping symlink is classified ERR_SYMLINK");
        CHECK(hc_sandbox_mkdirs(sb, "link_inner/x", 0700) == HC_SANDBOX_ERR_SYMLINK,
              "mkdirs refuses to descend through ANY symlink component (even a legit in-jail one)");
        /* a regular file in the path is not a directory -> the distinct ERR_NOT_DIR (not a generic IO error) */
        CHECK(hc_sandbox_mkdirs(sb, "sub/file.txt/x", 0700) == HC_SANDBOX_ERR_NOT_DIR,
              "mkdirs through a non-directory is classified ERR_NOT_DIR");
        /* the escapes created nothing outside */
        snprintf(path, sizeof path, "%s/escape", evil);
        CHECK(access(path, F_OK) != 0, "mkdirs '..' created nothing outside the jail");
    }

    /* --- no global state: a second, independent jail at a different root --- */
    hc_sandbox *sb2 = hc_sandbox_open(evil, &err);
    CHECK(sb2 != NULL, "second jail opens");
    if (sb2) {
        char o1[HC_SANDBOX_PATH_MAX], o2[HC_SANDBOX_PATH_MAX];
        hc_sandbox_status r1 = hc_sandbox_resolve(sb, "x", o1, sizeof o1);
        hc_sandbox_status r2 = hc_sandbox_resolve(sb2, "x", o2, sizeof o2);
        CHECK(r1 == HC_SANDBOX_OK && r2 == HC_SANDBOX_OK && strcmp(o1, o2) != 0,
              "two jails resolve the same path under different roots (no global state)");
        hc_sandbox_close(sb2);
    }

    /* --- live TOCTOU race: a final component flips between a real in-jail file and a symlink to
     *     an outside file; open_fd must NEVER hand back a descriptor that reads "OUTSIDE". --- */
    {
        struct race_ctx rc;
        rc.stop = 0;
        snprintf(rc.slot, sizeof rc.slot, "%s/raceslot", root);
        snprintf(rc.outlink, sizeof rc.outlink, "%s/out.txt", evil);
        snprintf(path, sizeof path, "%s/out.txt", evil);
        write_file(path, "OUTSIDE");

        pthread_t th;
        int escaped = 0;
        if (pthread_create(&th, NULL, race_swapper, &rc) == 0) {
            for (int i = 0; i < 30000; i++) {
                if (hc_sandbox_open_fd(sb, "raceslot", O_RDONLY, 0, &fd) == HC_SANDBOX_OK
                    && fd >= 0) {
                    char buf[16];
                    ssize_t n = read(fd, buf, sizeof buf - 1);
                    if (n < 0) n = 0;
                    buf[n] = '\0';
                    if (strncmp(buf, "OUTSIDE", 7) == 0) escaped = 1;
                    close(fd);
                }
            }
            rc.stop = 1;
            pthread_join(th, NULL);
        }
        CHECK(!escaped, "TOCTOU race never yields a descriptor to the outside file");
        unlink(rc.slot);
    }

    /* --- W4: jailed unlink + rename --- */
    {
        snprintf(path, sizeof path, "%s/sub/del.txt", root);
        write_file(path, "x");
        CHECK(hc_sandbox_unlink(sb, "sub/del.txt", 0) == HC_SANDBOX_OK, "unlink a jailed file");
        CHECK(access(path, F_OK) != 0, "the unlinked file is gone");

        CHECK(hc_sandbox_mkdirs(sb, "sub/deldir", 0700) == HC_SANDBOX_OK, "mkdirs a dir to remove");
        CHECK(hc_sandbox_unlink(sb, "sub/deldir", 1) == HC_SANDBOX_OK, "unlink (rmdir) an empty dir");

        CHECK(hc_sandbox_unlink(sb, "../planted", 0) == HC_SANDBOX_ERR_ESCAPE, "unlink refuses a '..' parent");
        CHECK(hc_sandbox_unlink(sb, "/etc/passwd", 0) == HC_SANDBOX_ERR_INVALID, "unlink refuses an absolute path");
        CHECK(hc_sandbox_unlink(sb, "link_out/secret.txt", 0) != HC_SANDBOX_OK,
              "unlink refuses a symlinked PARENT (no escape out of the jail)");

        /* unlink a SYMLINK removes the LINK, never its outside target */
        CHECK(hc_sandbox_unlink(sb, "link_file", 0) == HC_SANDBOX_OK, "unlink a symlink (the link itself)");
        snprintf(path, sizeof path, "%s/secret.txt", evil);
        CHECK(access(path, F_OK) == 0, "the symlink's OUTSIDE target SURVIVES (unlinked the link, not the target)");
        snprintf(path, sizeof path, "%s/link_file", root);
        CHECK(access(path, F_OK) != 0, "the symlink entry itself is gone");

        snprintf(path, sizeof path, "%s/sub/mv.txt", root);
        write_file(path, "y");
        CHECK(hc_sandbox_rename(sb, "sub/mv.txt", "sub/moved.txt") == HC_SANDBOX_OK, "rename within the jail");
        snprintf(path, sizeof path, "%s/sub/moved.txt", root);
        CHECK(access(path, F_OK) == 0, "the renamed file exists at the new name");
        CHECK(hc_sandbox_rename(sb, "sub/moved.txt", "../escaped.txt") == HC_SANDBOX_ERR_ESCAPE,
              "rename refuses a '..' destination parent");
        CHECK(hc_sandbox_rename(sb, "link_out/x", "sub/y") != HC_SANDBOX_OK,
              "rename refuses a symlinked source parent");

        /* rename ONTO a symlink destination REPLACES the link (never follows it to the outside target) */
        snprintf(target, sizeof target, "%s/secret.txt", evil);
        snprintf(link, sizeof link, "%s/sub/destlink", root);
        mk_symlink(target, link); /* sub/destlink -> evil/secret.txt (OUTSIDE the jail) */
        snprintf(path, sizeof path, "%s/sub/src.txt", root);
        write_file(path, "SRC");
        CHECK(hc_sandbox_rename(sb, "sub/src.txt", "sub/destlink") == HC_SANDBOX_OK, "rename onto a symlink dest");
        struct stat dls;
        snprintf(path, sizeof path, "%s/sub/destlink", root);
        CHECK(lstat(path, &dls) == 0 && S_ISREG(dls.st_mode), "the dest symlink was REPLACED by the moved file");
        char tc[16] = {0};
        int  tfd = open(target, O_RDONLY); /* the OUTSIDE target must be untouched (not clobbered through the link) */
        ssize_t tn = tfd >= 0 ? read(tfd, tc, sizeof tc - 1) : -1;
        if (tfd >= 0) close(tfd);
        CHECK(tn > 0 && strcmp(tc, "TOPSECRET") == 0, "rename onto a symlink did NOT clobber the outside target");

        /* a NON-empty directory refuses removal (no recursion) */
        CHECK(hc_sandbox_mkdirs(sb, "sub/full/inner", 0700) == HC_SANDBOX_OK, "mkdirs a non-empty dir");
        CHECK(hc_sandbox_unlink(sb, "sub/full", 1) != HC_SANDBOX_OK, "unlink refuses a NON-empty dir (no recursion)");
    }

    hc_sandbox_close(sb);

    /* best-effort cleanup (per-pid names, so leftovers are harmless) */
    snprintf(path, sizeof path, "%s/sub/file.txt", root); unlink(path);
    snprintf(path, sizeof path, "%s/sub/inner", root);    rmdir(path);
    snprintf(path, sizeof path, "%s/sub", root);          rmdir(path);
    snprintf(path, sizeof path, "%s/link_out", root);     unlink(path);
    snprintf(path, sizeof path, "%s/link_file", root);    unlink(path);
    snprintf(path, sizeof path, "%s/link_inner", root);   unlink(path);
    snprintf(path, sizeof path, "%s/dangling", root);     unlink(path);
    snprintf(path, sizeof path, "%s/made.txt", root);     unlink(path);
    rmdir(root);
    snprintf(path, sizeof path, "%s/secret.txt", evil);   unlink(path);
    snprintf(path, sizeof path, "%s/planted.txt", evil);  unlink(path);
    snprintf(path, sizeof path, "%s/out.txt", evil);      unlink(path);
    rmdir(evil);

    if (g_fails) {
        fprintf(stderr, "hc_sandbox: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_sandbox: all checks passed\n");
    return 0;
}
