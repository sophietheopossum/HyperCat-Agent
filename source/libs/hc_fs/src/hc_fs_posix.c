/* hc_fs_posix.c — the shared POSIX filesystem primitives. See hc_fs.h.
 *
 * One implementation serves Linux + macOS (both POSIX). This is the consolidation of the primitives
 * that hc_store, hc_artifacts, and hc_memory had each mirrored; they now link this single source so a
 * crash-safety or bounds fix lives in one place. Public headers leak none of this. */

#define _DEFAULT_SOURCE 1

#include "hc_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int hc_fs_mkdirs(const char *path)
{
    char tmp[1100];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s", path) >= sizeof tmp) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int write_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

int hc_fs_atomic_write(const char *path, const char *data, size_t len)
{
    char tmp[1200];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return -1;
    /* O_NOFOLLOW on the temp: a same-uid attacker who pre-plants `<path>.tmp` as a SYMLINK cannot redirect the
     * write outside the store (the open fails ELOOP). The subsequent rename(tmp, path) operates on `path` itself
     * — it REPLACES a symlink there rather than following it — so the bytes always land at `path`. (O_NOFOLLOW
     * guards only the final component; a symlinked PARENT dir is the caller's no-follow-walk concern.) */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int rc = write_all(fd, data, len);
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && rename(tmp, path) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    return rc;
}

int hc_fs_append(const char *path, const char *data, size_t len)
{
    /* O_NOFOLLOW: refuse to append THROUGH a symlink at `path` (a same-uid attacker pre-planting e.g. a WAL or
     * index file as a symlink can't redirect the append outside the store). Guards the final component only. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int rc = write_all(fd, data, len);
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    return rc;
}

int hc_fs_append_line(const char *path, const char *line, size_t len)
{
    /* As hc_fs_append, for a newline-delimited log. A crash or a failed append can leave a fragment with no
     * '\n' at the tail, and appending straight after it would glue this record onto it: a replay that skips
     * the unparseable result loses the record. So when the last byte is not '\n' the record is written with
     * a leading '\n', leaving the fragment alone on its own line. A write that fails partway is rolled back,
     * so a record the caller was told failed cannot later be completed by the next call's '\n' and replayed.
     * For a single-writer log: the check and the append are two steps. O_RDWR (not O_WRONLY) only so the last
     * byte can be read and a partial write truncated. len 0 writes nothing (or a lone '\n' after a torn tail). */
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    char        last = '\n';
    int         rc = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : -1;
    off_t       size = rc == 0 ? st.st_size : -1; /* -1: st unset (fstat failed) or not a file */
    if (rc == 0 && st.st_size > 0 && pread(fd, &last, 1, st.st_size - 1) != 1) rc = -1;
    if (rc == 0 && last != '\n') rc = write_all(fd, "\n", 1);
    if (rc == 0) rc = write_all(fd, line, len);
    if (rc != 0 && size >= 0 && ftruncate(fd, size) != 0) {
        /* best effort: a chattr +a log refuses it, and the next call then ends the partial record */
    }
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    return rc;
}

int hc_fs_size(const char *path, size_t *size_out)
{
    /* lstat, not stat: hc_fs_append refuses to write through a symlink, so a symlink here is not the file
     * it appends to. An absent file is size 0 -- the next append creates it. */
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno != ENOENT) return -1;
        *size_out = 0;
        return 0;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) return -1;
    *size_out = (size_t)st.st_size;
    return 0;
}

char *hc_fs_read_file(const char *path, size_t max_bytes, size_t *len_out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)max_bytes) {
        close(fd); /* reject an oversized file BEFORE allocating — bounds host memory (uintmax cmp is
                    * 32-bit-safe: it cannot under-allocate via an off_t->size_t truncation) */
        return NULL;
    }
    size_t n = (size_t)st.st_size;
    char *buf = malloc(n + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);
    buf[off] = '\0';
    if (len_out) *len_out = off;
    return buf;
}

int hc_fs_list_dirs(const char *root, char ***out, size_t *n_out)
{
    *out = NULL;
    *n_out = 0;
    DIR *d = opendir(root);
    if (!d) return -1;

    char **list = NULL;
    size_t n = 0, cap = 0;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char full[1200];
        if ((size_t)snprintf(full, sizeof full, "%s/%s", root, e->d_name) >= sizeof full)
            continue;
        struct stat st;
        /* lstat (not stat): a SYMLINK — even one pointing at a real directory — is NOT a "real directory" and is
         * skipped. This honors the banner contract AND stops a planted symlink (e.g. projects/<name> -> /victim)
         * from being listed + adopted as a real subtree by a security-sensitive caller (hc_projects). */
        if (lstat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            char **nl = realloc(list, ncap * sizeof *nl);
            if (!nl) {
                rc = -1;
                break;
            }
            list = nl;
            cap = ncap;
        }
        list[n] = strdup(e->d_name);
        if (!list[n]) {
            rc = -1;
            break;
        }
        n++;
    }
    closedir(d);
    if (rc != 0) {
        hc_fs_free_list(list, n);
        return -1;
    }
    *out = list;
    *n_out = n;
    return 0;
}

void hc_fs_free_list(char **list, size_t n)
{
    for (size_t i = 0; i < n; i++) free(list[i]);
    free(list);
}

void hc_fs_now_iso8601(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}
