#ifndef HC_WAL_H
#define HC_WAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_wal — a per-stream append-only write-ahead log (the durable substrate behind P14 crash-recoverable
 * agendas). A GENERIC, domain-agnostic line log: each record is ONE caller-supplied line; hc_wal owns only
 * the crash-safe append + the torn-tail-skipping replay + the stream scan. It knows NOTHING about
 * agendas/tasks — the orchestrator defines the record schema and the fold, so hc_wal stays a reusable
 * primitive (one concern: durable ordered lines per stream).
 *
 * Purpose:   open (create on first append) / append (O_APPEND + fsync, via hc_fs) / replay (skip a torn
 *            tail) / list (scan *.wal) / remove (unlink on completion). One file per stream:
 *            <dir>/<stream_id>.wal.
 * Owns:      hc_wal_open returns a small handle holding the validated path; hc_wal_close frees it. replay /
 *            list / remove are stateless (path-based). No fd is held across calls (append reopens via
 *            hc_fs_append — one writer, infrequent records, so the reopen cost is irrelevant and there is
 *            no fd lifetime to leak).
 * Threading: a stream is SINGLE-WRITER by contract (the orchestrator's driver thread is the sole appender).
 *            hc_wal adds no lock; concurrent appenders to one stream would interleave at OS O_APPEND
 *            granularity — callers own ordering. Different streams are independent.
 * Security:  stream_id becomes a path component — it is traversal-validated (reject '/' / ".." / empty /
 *            over-long / control bytes), exactly like hc_store's id guard, so a stream id read from a
 *            (same-uid-writable) listing can never escape `dir`. Every read is byte-capped
 *            (HC_WAL_MAX_FILE) BEFORE allocating; a record is length-capped (HC_WAL_MAX_LINE) and rejected
 *            if it contains a newline (which would split the record on replay). Replay NEVER executes a
 *            record — it only hands the bytes to `cb`. `dir` must already exist and be host-private (the
 *            caller provisions it 0700 under the persistent data dir, outside every agent sandbox).
 * Returns:   the int functions return 0 on success, -1 on failure (unless noted). */

#define HC_WAL_MAX_LINE    (512u * 1024u)        /* per-record line cap (a task result is worker-capped 240K) */
#define HC_WAL_MAX_FILE    (16u * 1024u * 1024u) /* per-stream total read cap (bounds replay memory)          */
#define HC_WAL_MAX_STREAMS 4096u                 /* max streams hc_wal_list reports (bounds a recovery scan)   */

typedef struct hc_wal hc_wal;

/* Open the WAL for `stream_id` under `dir` (which must already exist); the file is created on first append.
 * NULL if stream_id is unsafe (traversal / empty / over-long / control bytes) or on allocation failure. */
hc_wal *hc_wal_open(const char *dir, const char *stream_id);

/* Append one record: `line` (len bytes) followed by a '\n', crash-safe (O_APPEND + fsync). Returns -1
 * (writing nothing) if `line` is NULL, exceeds HC_WAL_MAX_LINE, or contains a '\n'. 0 on a durable append. */
int hc_wal_append(hc_wal *w, const char *line, size_t len);

void hc_wal_close(hc_wal *w);

/* Replay callback: one COMPLETE record `line` (NUL-terminated, `len` bytes, no trailing newline). Return 0
 * to continue, non-zero to stop the replay early. */
typedef int (*hc_wal_line_cb)(const char *line, size_t len, void *user);

/* Replay `stream_id`'s records in append order, SKIPPING a torn tail (trailing bytes with no terminating
 * newline = a partial record from a crash mid-append). An absent stream is 0 records (returns 0). Returns
 * -1 on a hard read error or an over-cap file. */
int hc_wal_replay(const char *dir, const char *stream_id, hc_wal_line_cb cb, void *user);

/* Stream-scan callback: a `stream_id` that has a WAL under `dir`. Return 0 to continue, non-zero to stop. */
typedef int (*hc_wal_id_cb)(const char *stream_id, void *user);

/* List the stream ids with a WAL file under `dir` (the *.wal basenames). Since a settled stream's WAL is
 * removed, these are exactly the INCOMPLETE streams a recovery must fold. Bounded at HC_WAL_MAX_STREAMS.
 * An absent `dir` is 0 streams (returns 0). */
int hc_wal_list(const char *dir, hc_wal_id_cb cb, void *user);

/* Remove `stream_id`'s WAL (call once the stream has durably settled — recovery is no longer needed).
 * Traversal-validated; an absent file is success (idempotent). */
int hc_wal_remove(const char *dir, const char *stream_id);

#ifdef __cplusplus
}
#endif
#endif /* HC_WAL_H */
