#ifndef HC_ARTIFACTS_H
#define HC_ARTIFACTS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_artifacts — a content-addressed store for agent outputs, plus an append-only provenance log.
 *
 * Purpose:   give every agent output (today: an approved fs_write) a stable IDENTITY (the sha256 of its
 *            bytes) and a LINEAGE (which agent / task / agenda / tool produced it). Identical bytes
 *            dedupe to one object; a reference is tamper-evident (the id IS a checksum). This is the
 *            substrate the diff-review (P11) and trace (P12) surfaces read, and it generalizes the
 *            per-turn manifest-audit discipline from "what did this turn touch" to "what did this
 *            artifact come from."
 * Owns:      the store root path. Objects live under <root>/objects/<ab>/<rest> (git-style fan-out);
 *            provenance is one JSON object per line in <root>/provenance.jsonl. Crash-safe by
 *            construction: an object is fsync'd via temp+rename, a provenance line is O_APPEND+fsync'd;
 *            no central mutable index, so the store is rebuilt by scanning the log (the hc_store
 *            discipline). The store is single-writer (the host); reads are by value (copied out).
 * Threading: an hc_artifacts handle is single-owner; not safe to share across threads at once.
 * Lifetime:  buffers from hc_artifacts_get are malloc'd (caller free()s); record arrays are released
 *            with hc_artifacts_recs_free.
 * Security:  bytes are untrusted (model/agent output) — the store only hashes and writes them, never
 *            executes or interprets. An id is validated as 64 lowercase-hex BEFORE it touches a path, so
 *            it can never traverse out of objects/. Object size + the provenance log are bounded.
 * Portable:  POSIX (Linux + macOS). Filesystem work goes through the shared libs/hc_fs (hc_fs.h) used by
 *            hc_store, hc_artifacts, hc_memory, and hc_manifest, and SHA-256 through the shared hc_hash.
 */

#define HC_ARTIFACT_ID_LEN 65 /* 64 lowercase-hex sha256 chars + NUL */

typedef struct hc_artifacts hc_artifacts;

/* Open (creating if absent) an artifact store rooted at `dir` (a host-private directory). NULL on
 * failure. The caller owns the handle and closes it with hc_artifacts_close. */
hc_artifacts *hc_artifacts_open(const char *dir);
void          hc_artifacts_close(hc_artifacts *);

/* Store `n` bytes; writes the content id (HC_ARTIFACT_ID_LEN incl. NUL) to id_out. Idempotent —
 * identical bytes yield the identical id and are stored once. 0 on success, -1 on error (incl. n over
 * the internal size cap). id_out is written only on success. */
int hc_artifacts_put(hc_artifacts *, const void *bytes, size_t n, char id_out[HC_ARTIFACT_ID_LEN]);

/* Fetch an object by id into a malloc'd, NUL-terminated buffer the caller free()s; *n_out = byte
 * length. NULL on a bad id (not 64-hex), not-found, or an over-cap object. */
void *hc_artifacts_get(hc_artifacts *, const char *id, size_t *n_out);

/* The context that produced an artifact — recorded as one append-only provenance line. All strings are
 * borrowed for the call. `inputs` (the artifact ids this derived from) is reserved for the later lineage
 * slice; the MVP records the scalar fields (fs_write outputs have no tracked inputs). */
typedef struct {
    const char  *agent;  /* the broker-stamped producer id ("agent:A")        */
    const char  *task;   /* task id within the agenda                          */
    const char  *agenda; /* agenda id                                          */
    const char  *tool;   /* "fs_write" etc.                                    */
    const char  *label;  /* a human handle — for fs_write, the workspace path  */
    long         size;   /* object byte length (the host has it from put)      */
    const char **inputs; /* reserved (lineage slice); pass NULL for now        */
    size_t       n_inputs;
} hc_provenance;

/* Append a provenance record for `id`. 0 on success, -1 on a bad id or I/O failure. */
int hc_artifacts_record(hc_artifacts *, const char *id, const hc_provenance *);

/* A provenance row read back for the UI / audit. Self-contained (inline fixed buffers — no borrowed
 * pointers), so the array is released with a single hc_artifacts_recs_free. */
typedef struct {
    char id[HC_ARTIFACT_ID_LEN];
    char agent[64];
    char task[64];
    char agenda[64];
    char tool[32];
    char label[256];
    char created[32]; /* ISO-8601 UTC */
    long size;
} hc_artifact_rec;

/* All artifacts produced by `task` (most-recent-first), for the task-detail panel. 0 on success (a
 * missing log is success with 0 rows), fills *out (freed by hc_artifacts_recs_free) + *n_out. */
int hc_artifacts_by_task(hc_artifacts *, const char *task, hc_artifact_rec **out, size_t *n_out);

/* The version history of a `label` (every recorded id sharing that label, most-recent-first) — the read
 * source the diff-review (P11) version view will use. Same ownership contract as by_task. */
int hc_artifacts_history(hc_artifacts *, const char *label, hc_artifact_rec **out, size_t *n_out);

/* The most-recent `max` artifacts across all tasks (most-recent-first) — the host caches these for the
 * UI's per-task artifact view (it filters by task id). Same ownership contract as by_task. */
int  hc_artifacts_recent(hc_artifacts *, size_t max, hc_artifact_rec **out, size_t *n_out);
void hc_artifacts_recs_free(hc_artifact_rec *, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* HC_ARTIFACTS_H */
