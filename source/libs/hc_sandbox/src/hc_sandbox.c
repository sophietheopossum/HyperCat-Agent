/* hc_sandbox.c — per-context POSIX filesystem jail. See hc_sandbox.h for the contract.
 *
 * Defense is three layers, all per-handle (no module globals — the deliberate departure from
 * the reference design's module-global root):
 *   1. open():    realpath() the root once, force a single trailing '/', and hold an
 *                 O_DIRECTORY fd open so the root inode cannot be swapped under us.
 *   2. resolve(): canonicalize the request with realpath() and require the result to sit under
 *                 the slashed root prefix. realpath() collapses ".." and resolves every
 *                 symlink, so a middle-or-final symlink pointing outside is caught here as
 *                 ERR_ESCAPE. When the target does not exist yet (a file to be created) the
 *                 parent is canonicalized instead and the basename re-attached.
 *   3. open_fd(): open the canonical path by walking it one component at a time from the
 *                 pinned root fd, with O_NOFOLLOW on EVERY component. realpath() in layer 2
 *                 only sees the filesystem as it was at resolve() time; the no-follow walk
 *                 additionally defeats a symlink swapped onto any component between resolve()
 *                 and the open — the residual TOCTOU a single final-component O_NOFOLLOW
 *                 would miss.
 *
 * The pure path helpers are static and exercised through the public API by the escape battery
 * in tests/test_hc_sandbox.c.
 */

#define _DEFAULT_SOURCE 1

#include "hc_sandbox.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

struct hc_sandbox {
    int    root_fd;                          /* pinned O_DIRECTORY fd; closed by close()        */
    size_t root_len;                         /* strlen(root_canon); includes the trailing '/'   */
    char   root_canon[HC_SANDBOX_PATH_MAX];  /* realpath'd root with a single trailing '/'       */
};

static hc_sandbox_status map_errno(int e)
{
    switch (e) {
    case ELOOP:        return HC_SANDBOX_ERR_SYMLINK;
    case ENOENT:       return HC_SANDBOX_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:        return HC_SANDBOX_ERR_ACCESS;
    case ENAMETOOLONG: return HC_SANDBOX_ERR_TOO_LONG;
    default:           return HC_SANDBOX_ERR_IO;
    }
}

/* Reject NULL/empty paths and any embedded control byte. A smuggled NUL already truncates the
 * C string; this additionally bars newlines and other control characters from path arguments. */
static hc_sandbox_status validate_input(const char *p)
{
    if (!p || p[0] == '\0') return HC_SANDBOX_ERR_INVALID;
    size_t n = strnlen(p, HC_SANDBOX_PATH_MAX);
    if (n == HC_SANDBOX_PATH_MAX) return HC_SANDBOX_ERR_TOO_LONG;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7f) return HC_SANDBOX_ERR_INVALID;
    }
    return HC_SANDBOX_OK;
}

/* True iff canonical absolute `canon` is the root directory itself or strictly below it. The
 * root is stored WITH a trailing '/', so the strict-below test compares the full slashed
 * prefix — "/ws/" can never prefix-match "/ws-evil". The only slash-free comparison is an
 * exact-length match against the root directory. */
static bool inside_root(const char *root_canon, size_t root_len, const char *canon)
{
    size_t m = strlen(canon);
    if (m > 0 && m == root_len - 1 && memcmp(canon, root_canon, m) == 0)
        return true;                                  /* canon == the root directory */
    if (m >= root_len && memcmp(canon, root_canon, root_len) == 0)
        return true;                                  /* canon is strictly below root */
    return false;
}

/* Split absolute `abs_path` (always begins with '/') into parent + basename. */
static hc_sandbox_status split_last_slash(const char *abs_path, char *parent, size_t pcap,
                                          char *base, size_t bcap)
{
    const char *slash = strrchr(abs_path, '/');
    if (!slash) return HC_SANDBOX_ERR_INVALID;
    size_t plen = (size_t)(slash - abs_path);
    if (plen == 0) {
        if (pcap < 2) return HC_SANDBOX_ERR_TOO_LONG;
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        if (plen + 1 > pcap) return HC_SANDBOX_ERR_TOO_LONG;
        memcpy(parent, abs_path, plen);
        parent[plen] = '\0';
    }
    size_t blen = strlen(slash + 1);
    if (blen + 1 > bcap) return HC_SANDBOX_ERR_TOO_LONG;
    memcpy(base, slash + 1, blen + 1);
    return HC_SANDBOX_OK;
}

hc_sandbox *hc_sandbox_open(const char *root_dir, hc_sandbox_status *err)
{
    if (!root_dir || root_dir[0] == '\0') {
        if (err) *err = HC_SANDBOX_ERR_ROOT;
        return NULL;
    }
    char buf[HC_SANDBOX_PATH_MAX];
    if (!realpath(root_dir, buf)) {
        if (err) *err = HC_SANDBOX_ERR_ROOT;
        return NULL;
    }
    struct stat st;
    if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (err) *err = HC_SANDBOX_ERR_ROOT;
        return NULL;
    }
    size_t n = strlen(buf);
    hc_sandbox *s = calloc(1, sizeof *s);
    if (!s) {
        if (err) *err = HC_SANDBOX_ERR_NOMEM;
        return NULL;
    }
    if (n + 2 > sizeof s->root_canon) {        /* room for the trailing '/' + NUL */
        free(s);
        if (err) *err = HC_SANDBOX_ERR_ROOT;
        return NULL;
    }
    memcpy(s->root_canon, buf, n);
    if (n == 0 || s->root_canon[n - 1] != '/') s->root_canon[n++] = '/';
    s->root_canon[n] = '\0';
    s->root_len = n;

    s->root_fd = open(buf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (s->root_fd < 0) {
        free(s);
        if (err) *err = HC_SANDBOX_ERR_ROOT;
        return NULL;
    }
    if (err) *err = HC_SANDBOX_OK;
    return s;
}

void hc_sandbox_close(hc_sandbox *s)
{
    if (!s) return;
    if (s->root_fd >= 0) close(s->root_fd);
    free(s);
}

const char *hc_sandbox_root(const hc_sandbox *s) { return s ? s->root_canon : NULL; }

hc_sandbox_status hc_sandbox_resolve(hc_sandbox *s, const char *user_path, char *out,
                                     size_t out_cap)
{
    if (!s || !out || out_cap == 0) return HC_SANDBOX_ERR_INVALID;
    hc_sandbox_status st = validate_input(user_path);
    if (st != HC_SANDBOX_OK) return st;

    /* Assemble an absolute candidate. An absolute input is taken as-is (an attempt to inject
     * "/etc/passwd" is then caught by the inside-root check, not silently jailed). */
    char abs[HC_SANDBOX_PATH_MAX];
    if (user_path[0] == '/') {
        if ((size_t)snprintf(abs, sizeof abs, "%s", user_path) >= sizeof abs)
            return HC_SANDBOX_ERR_TOO_LONG;
    } else if ((size_t)snprintf(abs, sizeof abs, "%s%s", s->root_canon, user_path) >= sizeof abs) {
        return HC_SANDBOX_ERR_TOO_LONG;
    }

    /* Layer 2a: canonicalize the whole path (follows symlinks, collapses ".."). */
    char res[HC_SANDBOX_PATH_MAX];
    if (realpath(abs, res)) {
        if (!inside_root(s->root_canon, s->root_len, res)) return HC_SANDBOX_ERR_ESCAPE;
        if ((size_t)snprintf(out, out_cap, "%s", res) >= out_cap) return HC_SANDBOX_ERR_TOO_LONG;
        return HC_SANDBOX_OK;
    }
    if (errno != ENOENT) return map_errno(errno);

    /* Layer 2b: target does not exist yet — canonicalize the PARENT (which must exist) and
     * re-attach the basename, then re-check containment. realpath() on the parent resolves any
     * symlink in the parent chain before we test inside-root, so a symlinked parent cannot
     * smuggle the new path outside. */
    char parent[HC_SANDBOX_PATH_MAX];
    char base[NAME_MAX + 1];
    st = split_last_slash(abs, parent, sizeof parent, base, sizeof base);
    if (st != HC_SANDBOX_OK) return st;
    if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
        return HC_SANDBOX_ERR_INVALID;

    char pcanon[HC_SANDBOX_PATH_MAX];
    if (!realpath(parent, pcanon)) return map_errno(errno);   /* missing parent -> NOT_FOUND */

    char joined[HC_SANDBOX_PATH_MAX];
    if ((size_t)snprintf(joined, sizeof joined, "%s/%s", pcanon, base) >= sizeof joined)
        return HC_SANDBOX_ERR_TOO_LONG;
    if (!inside_root(s->root_canon, s->root_len, joined)) return HC_SANDBOX_ERR_ESCAPE;

    /* realpath() failed because the leaf does not RESOLVE — but it may still EXIST as a dangling
     * symlink (target missing or outside). Such a leaf is symlink-shaped, so realpath could not
     * collapse it; returning it would hand back a path that escapes the jail the instant a
     * symlink-following call (notably open() with O_CREAT) touches it. Refuse it. A genuinely
     * absent leaf (lstat ENOENT) is the creatable-file case and is allowed. */
    struct stat ls;
    if (lstat(joined, &ls) == 0 && S_ISLNK(ls.st_mode)) return HC_SANDBOX_ERR_SYMLINK;

    if ((size_t)snprintf(out, out_cap, "%s", joined) >= out_cap) return HC_SANDBOX_ERR_TOO_LONG;
    return HC_SANDBOX_OK;
}

/* Open `rel` (relative to root_fd; already canonical, so no "."/".." or symlinks existed at
 * resolve() time) one component at a time, refusing a symlink at ANY component. */
static hc_sandbox_status open_nofollow_walk(int root_fd, const char *rel, int flags,
                                            mode_t mode, hc_sandbox_fd *out_fd)
{
    if (rel[0] == '\0' || strcmp(rel, ".") == 0) {
        int fd = openat(root_fd, ".", flags | O_NOFOLLOW | O_CLOEXEC, mode);
        if (fd < 0) return map_errno(errno);
        *out_fd = fd;
        return HC_SANDBOX_OK;
    }
    char path[HC_SANDBOX_PATH_MAX];
    if ((size_t)snprintf(path, sizeof path, "%s", rel) >= sizeof path)
        return HC_SANDBOX_ERR_TOO_LONG;

    int cur = root_fd;                    /* never closed here while equal to root_fd */
    hc_sandbox_status st = HC_SANDBOX_OK;
    char *save = NULL;
    for (char *tok = strtok_r(path, "/", &save); tok;) {
        char *peek = strtok_r(NULL, "/", &save);
        if (strcmp(tok, ".") == 0 || strcmp(tok, "..") == 0) {  /* canonical paths have none */
            st = HC_SANDBOX_ERR_ESCAPE;
            break;
        }
        if (peek == NULL) {               /* final component honours the caller's flags */
            int fd = openat(cur, tok, flags | O_NOFOLLOW | O_CLOEXEC, mode);
            if (fd < 0) st = map_errno(errno);
            else *out_fd = fd;
            break;
        }
        int nfd = openat(cur, tok, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nfd < 0) {                    /* ELOOP here == a dir swapped for a symlink (TOCTOU) */
            st = map_errno(errno);
            break;
        }
        if (cur != root_fd) close(cur);
        cur = nfd;
        tok = peek;
    }
    if (cur != root_fd) close(cur);
    return st;
}

hc_sandbox_status hc_sandbox_open_fd(hc_sandbox *s, const char *user_path, int flags, int mode,
                                     hc_sandbox_fd *out_fd)
{
    if (out_fd) *out_fd = HC_SANDBOX_FD_INVALID;
    if (!s || !out_fd) return HC_SANDBOX_ERR_INVALID;

    char canon[HC_SANDBOX_PATH_MAX];
    hc_sandbox_status st = hc_sandbox_resolve(s, user_path, canon, sizeof canon);
    if (st != HC_SANDBOX_OK) return st;
    if (!inside_root(s->root_canon, s->root_len, canon)) return HC_SANDBOX_ERR_ESCAPE; /* defensive */

    const char *rel = (strlen(canon) == s->root_len - 1) ? "." : canon + s->root_len;
    return open_nofollow_walk(s->root_fd, rel, flags, (mode_t)mode, out_fd);
}

/* Create `rel` (workspace-relative) as a directory chain from root_fd, mkdir -p style: mkdirat each component
 * (EEXIST tolerated), then descend with openat(O_DIRECTORY|O_NOFOLLOW) — so a symlink (ELOOP) or a non-dir
 * (ENOTDIR) at any component fails rather than being followed or treated as a directory, and the walk can
 * never rise above root (openat from the pinned root fd, "."/".." refused). Mirrors open_nofollow_walk; it
 * does NOT use resolve() because the intermediate dirs do not exist yet (resolve requires an existing parent).
 * Crucially: O_NOFOLLOW means an EXISTING symlink component is refused here just as the open walk refuses it,
 * so this opens no new escape that the open path does not already close. */
static hc_sandbox_status mkdirs_walk(int root_fd, const char *rel, mode_t mode)
{
    if (rel[0] == '\0' || strcmp(rel, ".") == 0) return HC_SANDBOX_OK; /* the root already exists */
    char path[HC_SANDBOX_PATH_MAX];
    if ((size_t)snprintf(path, sizeof path, "%s", rel) >= sizeof path) return HC_SANDBOX_ERR_TOO_LONG;

    int               cur = root_fd; /* never closed here while equal to root_fd */
    hc_sandbox_status st = HC_SANDBOX_OK;
    char             *save = NULL;
    for (char *tok = strtok_r(path, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0 || strcmp(tok, "..") == 0) { /* a canonical rel has none, but be safe */
            st = HC_SANDBOX_ERR_ESCAPE;
            break;
        }
        if (mkdirat(cur, tok, mode) < 0 && errno != EEXIST) { /* EEXIST: the dir is already there (idempotent) */
            st = map_errno(errno);
            break;
        }
        /* descend WITHOUT following a symlink: an existing symlink/file at this component fails here */
        int nfd = openat(cur, tok, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nfd < 0) {
            /* Classify the refusal so a SECURITY log can tell an escape attempt from a benign error. With
             * O_DIRECTORY|O_NOFOLLOW, Linux returns ENOTDIR for BOTH a symlink and a regular file (the
             * type check precedes the no-follow ELOOP), so disambiguate with an lstat: a symlink component
             * is an escape attempt -> ERR_SYMLINK; any other non-directory -> ERR_NOT_DIR; else map_errno. */
            int         oe = errno; /* preserve across the classifying fstatat */
            struct stat ls;
            if (fstatat(cur, tok, &ls, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(ls.st_mode))
                st = HC_SANDBOX_ERR_SYMLINK;
            else if (oe == ENOTDIR)
                st = HC_SANDBOX_ERR_NOT_DIR;
            else
                st = map_errno(oe);
            break;
        }
        if (cur != root_fd) close(cur);
        cur = nfd;
    }
    if (cur != root_fd) close(cur);
    return st;
}

hc_sandbox_status hc_sandbox_mkdirs(hc_sandbox *s, const char *user_path, int mode)
{
    if (!s) return HC_SANDBOX_ERR_INVALID;
    hc_sandbox_status st = validate_input(user_path);
    if (st != HC_SANDBOX_OK) return st;
    /* Relative only: the caller passes a path under the jail root. An absolute input is refused rather than
     * silently re-jailed (the same conservatism resolve() applies via the inside-root check). */
    if (user_path[0] == '/') return HC_SANDBOX_ERR_INVALID;
    return mkdirs_walk(s->root_fd, user_path, (mode_t)mode);
}

/* Split a RELATIVE in-jail path on its LAST '/': `parent` (possibly "") gets the directory part, `base` the
 * final component. The base must be a single, non-empty, non-"."/".." component — so the destructive *at calls
 * below operate on a concrete name, never a traversal token. (Unlike split_last_slash, this works on the raw
 * user-relative path and does NOT canonicalize — the final component is deliberately left un-resolved so a
 * symlink THERE is acted on as a link, not followed.) */
static hc_sandbox_status split_rel(const char *rel, char *parent, size_t pcap, char *base, size_t bcap)
{
    const char *slash = strrchr(rel, '/');
    if (!slash) {
        if (pcap < 1) return HC_SANDBOX_ERR_TOO_LONG;
        parent[0] = '\0';
        size_t blen = strlen(rel);
        if (blen + 1 > bcap) return HC_SANDBOX_ERR_TOO_LONG;
        memcpy(base, rel, blen + 1);
    } else {
        size_t plen = (size_t)(slash - rel);
        if (plen + 1 > pcap) return HC_SANDBOX_ERR_TOO_LONG;
        memcpy(parent, rel, plen);
        parent[plen] = '\0';
        size_t blen = strlen(slash + 1);
        if (blen + 1 > bcap) return HC_SANDBOX_ERR_TOO_LONG;
        memcpy(base, slash + 1, blen + 1);
    }
    if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
        return HC_SANDBOX_ERR_INVALID;
    return HC_SANDBOX_OK;
}

hc_sandbox_status hc_sandbox_unlink(hc_sandbox *s, const char *user_path, int is_dir)
{
    if (!s) return HC_SANDBOX_ERR_INVALID;
    hc_sandbox_status st = validate_input(user_path);
    if (st != HC_SANDBOX_OK) return st;
    if (user_path[0] == '/') return HC_SANDBOX_ERR_INVALID; /* relative only */
    char parent[HC_SANDBOX_PATH_MAX], base[256];
    st = split_rel(user_path, parent, sizeof parent, base, sizeof base);
    if (st != HC_SANDBOX_OK) return st;
    /* reach the PARENT via the no-follow walk (refuses a symlink/".."/non-dir at any parent component) */
    hc_sandbox_fd pfd = HC_SANDBOX_FD_INVALID;
    st = open_nofollow_walk(s->root_fd, parent, O_RDONLY | O_DIRECTORY, 0, &pfd);
    if (st != HC_SANDBOX_OK) return st;
    /* unlinkat the FINAL NAME — a symlink there is removed as a link, never followed out of the jail */
    st = (unlinkat(pfd, base, is_dir ? AT_REMOVEDIR : 0) == 0) ? HC_SANDBOX_OK : map_errno(errno);
    close(pfd);
    return st;
}

hc_sandbox_status hc_sandbox_rename(hc_sandbox *s, const char *old_path, const char *new_path)
{
    if (!s) return HC_SANDBOX_ERR_INVALID;
    hc_sandbox_status st = validate_input(old_path);
    if (st != HC_SANDBOX_OK) return st;
    st = validate_input(new_path);
    if (st != HC_SANDBOX_OK) return st;
    if (old_path[0] == '/' || new_path[0] == '/') return HC_SANDBOX_ERR_INVALID; /* relative only */
    char op[HC_SANDBOX_PATH_MAX], ob[256], np[HC_SANDBOX_PATH_MAX], nb[256];
    st = split_rel(old_path, op, sizeof op, ob, sizeof ob);
    if (st != HC_SANDBOX_OK) return st;
    st = split_rel(new_path, np, sizeof np, nb, sizeof nb);
    if (st != HC_SANDBOX_OK) return st;
    /* reach BOTH parents via the no-follow walk; renameat then operates on the final NAMES (the old entry is
     * moved as itself; any entry at the new name is replaced — neither final component is followed out) */
    hc_sandbox_fd ofd = HC_SANDBOX_FD_INVALID, nfd = HC_SANDBOX_FD_INVALID;
    st = open_nofollow_walk(s->root_fd, op, O_RDONLY | O_DIRECTORY, 0, &ofd);
    if (st != HC_SANDBOX_OK) return st;
    st = open_nofollow_walk(s->root_fd, np, O_RDONLY | O_DIRECTORY, 0, &nfd);
    if (st != HC_SANDBOX_OK) {
        close(ofd);
        return st;
    }
    st = (renameat(ofd, ob, nfd, nb) == 0) ? HC_SANDBOX_OK : map_errno(errno);
    close(ofd);
    close(nfd);
    return st;
}

hc_sandbox_status hc_sandbox_list(hc_sandbox *s, const char *user_path, hc_sandbox_dirent **out,
                                  size_t *n_out)
{
    if (out) *out = NULL;
    if (n_out) *n_out = 0;
    if (!s || !out || !n_out) return HC_SANDBOX_ERR_INVALID;

    /* Reuse the no-follow walk: open the target as a directory with O_NOFOLLOW on every component, so a
     * symlink swapped onto any component is refused rather than followed out of the jail. */
    hc_sandbox_fd     dir_fd = HC_SANDBOX_FD_INVALID;
    hc_sandbox_status st = hc_sandbox_open_fd(s, user_path, O_RDONLY | O_DIRECTORY, 0, &dir_fd);
    if (st != HC_SANDBOX_OK) return st;

    /* fdopendir TAKES OWNERSHIP of dir_fd: from here the fd is touched ONLY via the DIR* (closedir
     * closes it, and dirfd(d) is the blessed accessor for fstatat's base). On failure we still own it. */
    DIR *d = fdopendir(dir_fd);
    if (!d) {
        hc_sandbox_status e = map_errno(errno);
        close(dir_fd);
        return e;
    }

    hc_sandbox_dirent *arr = NULL;
    size_t             cap = 0, n = 0;
    struct dirent     *de;
    errno = 0;
    while (n < HC_SANDBOX_LIST_MAX && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (strlen(de->d_name) >= sizeof arr[0].name) continue; /* skip a pathological over-long name */
        /* type/size via the dir's fd (dirfd(d), the POSIX accessor), NOT following a symlink leaf — a
         * symlink reports is_dir==0. Skip an entry that vanished/denied between readdir and fstatat
         * (a same-uid race) rather than emitting a misleading 0-byte row. */
        struct stat est;
        if (fstatat(dirfd(d), de->d_name, &est, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (n == cap) {
            size_t             ncap = cap ? cap * 2 : 32;
            hc_sandbox_dirent *na = realloc(arr, ncap * sizeof *na);
            if (!na) {
                free(arr);
                closedir(d);
                return HC_SANDBOX_ERR_NOMEM;
            }
            arr = na;
            cap = ncap;
        }
        memset(&arr[n], 0, sizeof arr[n]);
        snprintf(arr[n].name, sizeof arr[n].name, "%s", de->d_name);
        arr[n].is_dir = S_ISDIR(est.st_mode) ? 1 : 0;
        arr[n].size = S_ISREG(est.st_mode) ? (int64_t)est.st_size : 0;
        n++;
    }
    int read_errno = errno; /* readdir sets errno on a real error (vs 0 at end-of-dir) */
    closedir(d);            /* closes the owned fd */
    if (read_errno != 0 && n == 0) {
        free(arr);
        return map_errno(read_errno);
    }
    *out = arr;
    *n_out = n;
    return HC_SANDBOX_OK;
}

void hc_sandbox_list_free(hc_sandbox_dirent *entries) { free(entries); }

const char *hc_sandbox_strerror(hc_sandbox_status s)
{
    switch (s) {
    case HC_SANDBOX_OK:            return "ok";
    case HC_SANDBOX_ERR_INVALID:   return "invalid path argument";
    case HC_SANDBOX_ERR_TOO_LONG:  return "path too long";
    case HC_SANDBOX_ERR_ESCAPE:    return "path escapes the sandbox root";
    case HC_SANDBOX_ERR_SYMLINK:   return "refused to follow a symlink";
    case HC_SANDBOX_ERR_NOT_FOUND: return "path component does not exist";
    case HC_SANDBOX_ERR_ACCESS:    return "permission denied";
    case HC_SANDBOX_ERR_IO:        return "filesystem error";
    case HC_SANDBOX_ERR_NOMEM:     return "out of memory";
    case HC_SANDBOX_ERR_ROOT:      return "invalid sandbox root";
    case HC_SANDBOX_ERR_NOT_DIR:   return "a non-directory blocks the path";
    }
    return "unknown";
}
