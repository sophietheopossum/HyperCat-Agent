#ifndef HC_FS_H
#define HC_FS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_fs — the shared POSIX filesystem primitives behind HyperCat's append-only stores (hc_store,
 * hc_artifacts, hc_memory, hc_manifest, ...). Promoted from hc_store's private hc_store_fs once the
 * same crash-safe primitives had several consumers (the planned consolidation the store/artifacts
 * banners pointed at — so a hardening fix lives in ONE place, not N).
 *
 * Purpose:   crash-safe directory + file primitives — mkdir -p (0700), a temp+fsync+rename atomic
 *            write, an O_APPEND+fsync line append, a size-capped whole-file read (bounds host memory
 *            against an oversized/untrusted file), a subdirectory listing, and a UTC timestamp. No
 *            store/JSON/LLM semantics — just bytes + paths.
 * Owns:      nothing persistent; each call operates on caller-supplied paths. hc_fs_list_dirs returns a
 *            malloc'd array of strdup'd names the caller releases with hc_fs_free_list. hc_fs_read_file
 *            returns a malloc'd NUL-terminated buffer the caller free()s.
 * Threading: stateless + reentrant; concurrent calls on DIFFERENT paths are safe (the OS serializes
 *            same-path access; callers that need atomicity across calls coordinate themselves).
 * Security:  files/dirs are created 0700/0600; hc_fs_read_file rejects a file larger than max_bytes
 *            BEFORE allocating; hc_fs_list_dirs LSTATs each entry and returns only real directories (a
 *            symlink — even to a dir — is skipped). hc_fs_atomic_write/hc_fs_append open the final path
 *            component O_NOFOLLOW, so a pre-planted symlink there cannot redirect the write outside the
 *            store (a same-uid hardening; a symlinked PARENT dir remains the caller's no-follow-walk job).
 *            Paths are NOT traversal-validated here — the CALLER must validate any untrusted path
 *            component before passing it (the stores do, e.g. a 64-hex id / a percent-encoded scope).
 * Portable:  one POSIX implementation serves Linux + macOS (Docs/Plan_HyperCat/13-platform-portability.md).
 * Returns:   the int functions return 0 on success, -1 on failure. */

int   hc_fs_mkdirs(const char *path);                                      /* mkdir -p, 0700          */
int   hc_fs_atomic_write(const char *path, const char *data, size_t len);  /* temp + fsync + rename   */
int   hc_fs_append(const char *path, const char *data, size_t len);        /* O_APPEND + fsync, 0600  */
/* Read `path` whole into a malloc'd, NUL-terminated buffer (*len_out = byte length). NULL on error OR
 * when the file exceeds `max_bytes` (the cap bounds host memory against an oversized/planted file). */
char *hc_fs_read_file(const char *path, size_t max_bytes, size_t *len_out);
int   hc_fs_list_dirs(const char *root, char ***out, size_t *n_out);       /* subdirectory names      */
void  hc_fs_free_list(char **list, size_t n);
void  hc_fs_now_iso8601(char *out, size_t cap);                            /* "YYYY-MM-DDTHH:MM:SSZ"  */

#ifdef __cplusplus
}
#endif
#endif /* HC_FS_H */
