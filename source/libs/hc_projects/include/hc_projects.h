#ifndef HC_PROJECTS_H
#define HC_PROJECTS_H

/* hc_projects — the on-disk PROJECT INDEX (Wave 3, the project-isolation foundation). A stable extern "C"
 * primitive that mirrors hc_wal: an opaque handle, traversal-safe ids (the hc_wal id_ok precedent), and an
 * append-only `index.jsonl` replayed last-wins and cross-checked against the on-disk directory listing.
 *
 * On disk, under the host data dir `root` it is given:
 *   <root>/projects/index.jsonl   — the append-only index (one JSON object per line; last-wins per id)
 *   <root>/projects/<id>/         — one sealed subtree per project (the host re-roots storage here)
 *   <root>/active_project         — the active project's id (a single atomic-written line)
 *
 * Ids are MINTED by slugifying the operator's display name (lowercase ASCII alnum, other runs -> '-') and then
 * validating with the id_ok rules — so the DISPLAY NAME is never a path component (anti-traversal). The id is
 * immutable; rename changes only the display name. Delete writes a tombstone to the index (the host removes the
 * subtree separately, jailed); a tombstoned id is never revived by orphan-adoption.
 *
 * Ownership: hc_project values are caller-owned PODs with fixed inline buffers (no heap inside). hc_projects_list
 * returns a malloc'd array the caller frees with hc_projects_free_list. The handle owns its replayed in-memory
 * index; close frees it. Threading: NOT thread-safe — a single host thread drives it (the project switch happens
 * at a host-loop frame boundary, never from a worker/conductor thread). Time is caller-supplied (now_ms) so the
 * library calls no clock and stays deterministically testable. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HC_PROJECT_ID_CAP = 129,  /* <=128 id bytes + NUL (the id_ok cap)            */
    HC_PROJECT_NAME_CAP = 256 /* <=255 display bytes + NUL (never a path element) */
};

/* One project's metadata — a caller-owned POD (fixed buffers, no heap). */
typedef struct {
    char    id[HC_PROJECT_ID_CAP];      /* the traversal-safe, immutable id            */
    char    display[HC_PROJECT_NAME_CAP]; /* the human display name (bounded; not a path) */
    int64_t created_ms;                 /* mint time (caller-supplied)                 */
    int64_t touched_ms;                 /* last-active time (rises on create/touch)    */
} hc_project;

typedef struct hc_projects hc_projects;

/* Open the project store rooted at `data_dir` (creates <data_dir>/projects/ 0700, replays the index, and
 * adopts any orphan project dir not yet in the index). NULL on a bad/oversized root or a provisioning failure. */
hc_projects *hc_projects_open(const char *data_dir);
void         hc_projects_close(hc_projects *p);

/* Slugify a display name into a traversal-safe id (lowercase ASCII alnum kept; other runs collapse to a single
 * '-'; leading/trailing '-' trimmed; capped at 128). 0 + writes id_out (NUL-terminated) on success; -1 if the
 * name yields no usable id or cap is too small. PUBLIC so the host can PREVIEW the minted id before creating. */
int hc_projects_slug(const char *display, char *id_out, size_t cap);

/* Create a project: mint an id from `display` (deduped with a "-2","-3"… suffix if taken, including vs tombstoned
 * ids so a stale dir is never reused), create <root>/projects/<id>/ 0700, and append an index line stamped
 * now_ms. Fills *out (if non-NULL) with the created project. Returns 0 / -1 (mint/dir/append failure). */
int hc_projects_create(hc_projects *p, const char *display, int64_t now_ms, hc_project *out);

/* Look up a LIVE project by id. 0 + fills *out if present and not tombstoned; -1 otherwise. */
int hc_projects_get(hc_projects *p, const char *id, hc_project *out);

/* List the live projects, most-recently-touched first. Returns the count (>=0) and, if count>0, sets *out to a
 * malloc'd array the caller frees with hc_projects_free_list. -1 on error. *out is set to NULL when count==0. */
int  hc_projects_count(hc_projects *p); /* live project count (no allocation) */
int  hc_projects_list(hc_projects *p, hc_project **out);
void hc_projects_free_list(hc_project *list);

/* Rename a project's DISPLAY name only (the id is immutable). Appends an index line. 0 / -1 (unknown/tombstoned
 * id, or an oversized display). */
int hc_projects_rename(hc_projects *p, const char *id, const char *display);

/* Bump a project's touched_ms (e.g. on activation). Appends an index line. 0 / -1 (unknown/tombstoned id). */
int hc_projects_touch(hc_projects *p, const char *id, int64_t now_ms);

/* Tombstone a project in the index (it stops appearing in get/list). The SUBTREE on disk is NOT removed here —
 * that is the host's jailed responsibility (P3.3). Refuses to delete the currently-active project (-1). 0 / -1. */
int hc_projects_delete(hc_projects *p, const char *id);

/* The absolute path of a project's subtree (<root>/projects/<id>) into path_out (NUL-terminated). 0 / -1 (a
 * bad id or too-small cap). Does NOT require the project to exist — used by the host to provision/open it. */
int hc_projects_dir(hc_projects *p, const char *id, char *path_out, size_t cap);

/* The active-project pointer (atomic). get: fills id_out and returns 0, or -1 if unset/absent. set: validates
 * `id` is a LIVE project, then atomic-writes the pointer + touches the project. 0 / -1. */
int hc_projects_get_active(hc_projects *p, char *id_out, size_t cap);
int hc_projects_set_active(hc_projects *p, const char *id, int64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* HC_PROJECTS_H */
