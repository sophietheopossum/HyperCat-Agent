#ifndef HC_SANDBOX_H
#define HC_SANDBOX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_sandbox — per-context filesystem jail (POSIX: Linux + macOS).
 *
 * Purpose:   confine an agent's file access to one canonicalized root. Turns an untrusted,
 *            attacker-controlled path into either (a) a canonical absolute path PROVEN inside
 *            the root, or (b) an open fd for it, opened without following a symlink at any
 *            component — closing the realpath()-then-open() TOCTOU symlink-swap race.
 * Owns:      one jail = one hc_sandbox handle, holding the canonical root string and a
 *            persistent open directory fd that PINS the root for the handle's lifetime. There
 *            is NO module-global state: two handles with two roots are fully independent.
 * Threading: a single handle is single-owner — do not call into one handle from two threads
 *            at once (a close() racing a resolve()/open_fd() is a data race). Distinct handles
 *            are independent.
 * Lifetime:  hc_sandbox_open allocates; hc_sandbox_close frees and closes the root fd. Paths
 *            written to caller buffers are owned by the caller. An fd from hc_sandbox_open_fd
 *            is owned by the CALLER, who must close() it.
 * Portable:  POSIX only. realpath(3) + openat(2)/O_NOFOLLOW + an O_DIRECTORY root fd behave
 *            identically on Linux and macOS, so one implementation serves both. The fd is a
 *            named token (hc_sandbox_fd), not a bare int in signatures, so the ABI stays
 *            backend-neutral. See Docs/Plan_HyperCat/07-sandbox-and-policy.md.
 */

/* Longest path hc_sandbox accepts or emits, including the NUL. Equals PATH_MAX on Linux (4096)
 * and exceeds it on macOS (1024); buffers sized to this are safe arguments for realpath(3). */
#define HC_SANDBOX_PATH_MAX 4096

/* An open OS file handle. POSIX: a file descriptor. Named so the public ABI carries no bare
 * platform type. The caller owns it and must close() it. */
typedef int hc_sandbox_fd;
#define HC_SANDBOX_FD_INVALID (-1)

typedef struct hc_sandbox hc_sandbox;

/* Result of a jail operation. Module-local by design — there is no shared status type, and a
 * dedicated taxonomy keeps each failure mode explicit. ERR_ESCAPE is kept distinct from
 * ERR_NOT_FOUND/ERR_ACCESS so a policy/log layer can alert on an escape ATTEMPT (a security
 * event) versus a benign missing file. */
typedef enum {
    HC_SANDBOX_OK = 0,
    HC_SANDBOX_ERR_INVALID,    /* NULL/empty path, embedded control byte, or NULL out-param   */
    HC_SANDBOX_ERR_TOO_LONG,   /* path or a component exceeds the length limit                */
    HC_SANDBOX_ERR_ESCAPE,     /* resolved path lies OUTSIDE the root (.. , symlink, abs path) */
    HC_SANDBOX_ERR_SYMLINK,    /* a symlink was refused at open time (O_NOFOLLOW -> ELOOP)     */
    HC_SANDBOX_ERR_NOT_FOUND,  /* a path component does not exist (ENOENT)                     */
    HC_SANDBOX_ERR_ACCESS,     /* permission denied on an in-jail path (EACCES/EPERM)          */
    HC_SANDBOX_ERR_IO,         /* other OS / realpath / openat failure                        */
    HC_SANDBOX_ERR_NOMEM,      /* allocation failure                                          */
    HC_SANDBOX_ERR_ROOT,       /* root is missing, not a directory, unresolvable, or too long  */
    HC_SANDBOX_ERR_NOT_DIR     /* a non-directory blocks a path component (mkdirs: a file where a dir must be) */
} hc_sandbox_status;

/* A static human-readable string for `s` (never NULL, never freed). */
const char *hc_sandbox_strerror(hc_sandbox_status s);

/* Open a jail rooted at `root_dir`, which must already exist and be a directory — the jail
 * does NOT create it (provisioning the workspace is the caller's concern; a jail that creates
 * directories is a jail that can be tricked into creating them). The root is realpath()'d once
 * and a directory fd is held open to pin it. Returns NULL on failure; if `err` is non-NULL it
 * receives the reason. */
hc_sandbox *hc_sandbox_open(const char *root_dir, hc_sandbox_status *err);

/* Close a jail and release its pinned root fd. NULL-safe. */
void hc_sandbox_close(hc_sandbox *);

/* The canonical root, WITH a trailing '/'. Borrowed; valid until close. NULL if `s` is NULL. */
const char *hc_sandbox_root(const hc_sandbox *);

/* Resolve untrusted `user_path` (relative to the root, or absolute) to a canonical absolute
 * path proven inside the root, written NUL-terminated into `out` (capacity `out_cap`; use
 * HC_SANDBOX_PATH_MAX). Returns HC_SANDBOX_OK or a typed error; on error `out` is unspecified.
 * The returned path is canonical and its final component is never a symlink. It is meant for
 * hc_sandbox_open_fd, or for display / logging / manifest audit — NOT for opening the raw path
 * with a symlink-following call: a TOCTOU swap after this returns can still insert a symlink, so
 * only hc_sandbox_open_fd (or *at calls relative to hc_sandbox_root with O_NOFOLLOW) re-validates
 * safely. The jailed read-listing (hc_sandbox_list) is built on this same walk; the jailed mutators
 * (unlink/mkdir/rename) with the same guarantee are added as the agent-tool layer that needs them. */
hc_sandbox_status hc_sandbox_resolve(hc_sandbox *, const char *user_path,
                                     char *out, size_t out_cap);

/* Resolve `user_path` and open it, walking the canonical path one component at a time with
 * O_NOFOLLOW forced on at EVERY component — so a symlink swapped onto any component after
 * canonicalization is refused (ELOOP -> ERR_SYMLINK) rather than followed out of the jail.
 * `flags`/`mode` are POSIX open(2) values (e.g. O_RDONLY, O_WRONLY|O_CREAT). O_NOFOLLOW and
 * O_CLOEXEC are always added and cannot be cleared by the caller. On success writes the owned
 * fd to *out_fd (caller close()s it); on failure *out_fd is HC_SANDBOX_FD_INVALID. */
hc_sandbox_status hc_sandbox_open_fd(hc_sandbox *, const char *user_path,
                                     int flags, int mode, hc_sandbox_fd *out_fd);

/* Create `user_path` as a directory, creating any MISSING parent components too (like `mkdir -p`).
 * WORKSPACE-RELATIVE only (an absolute path is refused — the caller passes a path under the jail root).
 * Walked component-by-component from the pinned root fd with O_NOFOLLOW + O_DIRECTORY on every descend, so
 * a symlink or a non-directory at ANY component is refused (never followed, never escapes) and a "."/".."
 * component is rejected. An already-existing directory at any level is fine (idempotent — EEXIST tolerated);
 * a component that exists as a non-directory fails. `mode` (e.g. 0700) applies to the components this call
 * creates. An empty/"." path is a no-op success (the root already exists). This is the jailed `mkdir` of the
 * destructive-op layer the open API banner anticipates — it lets a worker materialize a deliverable into a
 * subdirectory (e.g. "src/app.py") that does not exist yet. */
hc_sandbox_status hc_sandbox_mkdirs(hc_sandbox *, const char *user_path, int mode);

/* One entry of a jailed directory listing. `name` is a single path component (no '/'); join it to
 * the listed directory's path to descend. `is_dir` distinguishes directories (navigable) from files;
 * a symlink is reported with is_dir==0 and is NEVER followed (its own lstat type). */
typedef struct {
    char    name[256]; /* the entry name (NUL-terminated; "." and ".." are omitted)     */
    int     is_dir;    /* 1 if a directory, 0 otherwise (regular file, symlink, ...)     */
    int64_t size;      /* byte size for a regular file; 0 otherwise (fixed width by ABI) */
} hc_sandbox_dirent;

/* Upper bound on entries returned by hc_sandbox_list — bounds host memory against a hostile same-uid
 * peer that fills a jailed directory with a huge number of names. Entries past this are omitted. */
#define HC_SANDBOX_LIST_MAX 8192

/* List the entries of the in-jail directory `user_path`. TOCTOU-safe: the directory is opened with
 * O_NOFOLLOW forced on EVERY component (via hc_sandbox_open_fd), and each entry's type is taken with
 * fstatat(AT_SYMLINK_NOFOLLOW) — symlinks are reported, never traversed. On success sets *n_out
 * (0..HC_SANDBOX_LIST_MAX) and *out (NULL when the directory is empty, else a malloc'd array); on
 * failure *out is NULL and *n_out is 0. Either way the caller calls hc_sandbox_list_free (NULL-safe).
 * The order is the filesystem's (unsorted) — the caller sorts for display. */
hc_sandbox_status hc_sandbox_list(hc_sandbox *, const char *user_path,
                                  hc_sandbox_dirent **out, size_t *n_out);
void hc_sandbox_list_free(hc_sandbox_dirent *entries); /* NULL-safe */

/* Jailed DELETE (W4): remove the in-jail entry `user_path`. WORKSPACE-RELATIVE only (absolute refused). The
 * PARENT directory is reached via the no-follow walk (a symlink / "." / ".." at ANY parent component is refused,
 * never followed), then `unlinkat` removes the FINAL NAME — operating on the entry itself, so a symlink at the
 * final component is unlinked (its link, never its target) and cannot redirect the delete outside the jail. Pass
 * is_dir!=0 to remove an (empty) directory (AT_REMOVEDIR); 0 for a file/symlink. A "."/".." or empty final
 * component is refused. Returns HC_SANDBOX_OK or a typed error (NOT_FOUND, NOT_DIR, ERR_IO for a non-empty dir). */
hc_sandbox_status hc_sandbox_unlink(hc_sandbox *, const char *user_path, int is_dir);

/* Jailed RENAME/move (W4): rename `old_path` -> `new_path`, both WORKSPACE-RELATIVE (absolute refused). BOTH
 * parent directories are reached via the no-follow walk; `renameat` then operates on the final NAMES — it
 * renames the old entry itself (a symlink is moved as a link, never followed) and replaces any entry at the new
 * name (a symlink there is replaced, never followed out), so neither endpoint can escape the jail. A "."/".." or
 * empty final component on either side is refused. Returns HC_SANDBOX_OK or a typed error. */
hc_sandbox_status hc_sandbox_rename(hc_sandbox *, const char *old_path, const char *new_path);

#ifdef __cplusplus
}
#endif
#endif /* HC_SANDBOX_H */
