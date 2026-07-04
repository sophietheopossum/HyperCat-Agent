#ifndef HC_STORE_H
#define HC_STORE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_store — append-only JSONL session / transcript store.
 *
 * Purpose:   persist agent sessions as a per-session directory holding an append-only
 *            transcript.jsonl (one message object per line) plus an atomically-written
 *            meta.json. Crash-safe by construction: a message is appended+fsynced before it
 *            is mirrored in memory, and meta.json is replaced atomically (temp + rename).
 * Owns:      hc_store owns the storage root path; hc_session owns its in-memory message
 *            mirror. Closing/freeing one does not touch the other.
 * Threading: a store or session is single-owner; not safe to share across threads at once.
 * Lifetime:  const char* from hc_session_message points INTO the session and is valid until
 *            hc_session_free. hc_session_info arrays are released with hc_store_list_free.
 * Portable:  all filesystem specifics live behind the shared libs/hc_fs (hc_fs.h),
 *            shared by Linux and macOS — see Docs/Plan_HyperCat/13-platform-portability.md.
 */

typedef struct hc_store   hc_store;
typedef struct hc_session hc_session;

typedef struct {
    char id[80];
    char title[64];
    char model[128];
    char created[32];
    char updated[32];
    int  turns;
} hc_session_info;

/* Open a store rooted at `root_or_null`, or NULL to use $HC_STORE_DIR, else the per-user
 * default (XDG ~/.config/hypercat/sessions on POSIX). Creates the root if absent. */
hc_store   *hc_store_open(const char *root_or_null);
void        hc_store_close(hc_store *);
const char *hc_store_root(const hc_store *);

/* List sessions in the store (scans per-session meta.json). Returns 0 on success and fills
 * `*out` (freed by hc_store_list_free) and `*n_out`. */
int  hc_store_list(hc_store *, hc_session_info **out, size_t *n_out);
void hc_store_list_free(hc_session_info *, size_t n);

hc_session *hc_session_new (hc_store *, const char *title, const char *model);
/* Load a session by id. `id` MUST be a single path component — it becomes a directory name under the
 * store root, so an id containing '/' or ".." is REFUSED (returns NULL), not allowed to escape the
 * root (a caller may pass an id read from a same-uid-writable listing). NULL on not-found / bad id. */
hc_session *hc_session_load(hc_store *, const char *id);
void        hc_session_free(hc_session *);

/* Append a message (role: "user"|"assistant"|"tool"|"system"). Persists immediately, then
 * mirrors in memory. Returns false on I/O or allocation failure. */
bool hc_session_append   (hc_session *, const char *role, const char *content);
bool hc_session_save_meta(hc_session *);
/* Rename: set the session's title (bounded to the title field; ""/NULL => "Untitled", as hc_session_new) and persist
 * meta.json atomically. The hc_session is single-owner, so the caller must hold it (the conductor renames its LIVE
 * session on its own thread; the host renames an INACTIVE one via load -> set_title -> free). Returns false on I/O. */
bool hc_session_set_title(hc_session *, const char *title);

const char *hc_session_id   (const hc_session *);
/* The session's own directory (`<store root>/<id>`), where its transcript + manifest live. Valid until
 * hc_session_free; callers must not assume the `root/id` layout themselves — use this accessor. */
const char *hc_session_dir  (const hc_session *);
size_t      hc_session_count(const hc_session *);
bool        hc_session_message(const hc_session *, size_t i,
                               const char **role_out, const char **content_out);

#ifdef __cplusplus
}
#endif
#endif /* HC_STORE_H */
