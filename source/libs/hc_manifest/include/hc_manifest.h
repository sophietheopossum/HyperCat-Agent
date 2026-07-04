#ifndef HC_MANIFEST_H
#define HC_MANIFEST_H

#include "hc_hash.h" /* HC_SHA256_HEX_LEN (the per-turn digest fields) */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_manifest — a per-turn, append-only audit + replay record for one agent session. The substrate that
 * makes a recorded session deterministically REPLAYABLE (P03) and a turn's behaviour reproducible.
 *
 * Purpose:   record, per agent-loop turn, (a) the LITERAL assistant output + tool calls + tool results
 *            (so a replay backend can feed them back by turn index, offline) and (b) compact HASHES of
 *            the turn's input + the whole turn (so a replay's re-derived manifest can be compared to the
 *            stored one — the divergence oracle). The bulky INPUT (the growing message history) is
 *            hashed, never stored, so the manifest does not bloat O(turns^2).
 * Owns:      one `manifest.jsonl` beside the session's transcript (the hc_store session dir). Append-only
 *            JSONL, one object per turn, crash-safe via hc_fs (O_APPEND+fsync) and rebuilt by scanning —
 *            the same discipline as hc_store/hc_artifacts. The `turn_sha256` over the canonical record is
 *            stable across identical runs (canonical JSON + a deterministic hash), which is what the
 *            oracle and the P2 gate test rely on. Single-writer (the worker recording its own turns).
 * Threading: an hc_manifest handle is single-owner; not safe to share across threads at once.
 * Lifetime:  hc_manifest_rec arrays are released with hc_manifest_recs_free (each rec owns malloc'd
 *            literal strings). Strings passed to hc_manifest_append are borrowed for the call.
 * Security:  the recorded text is UNTRUSTED model output — stored + hashed, never interpreted. The
 *            literal fields are length-bounded; the log read is size-capped (bounds host memory). The
 *            session dir is host-private; the manifest holds no secret (the input is hashed, the api_key
 *            never reaches here).
 * Depends:   PUBLIC ABI dep hc_hash (HC_SHA256_HEX_LEN appears in hc_manifest_rec); PRIVATE impl deps
 *            hc_fs (append/read) + hc_json (record build/parse). Never hc_llm. */

#define HC_MANIFEST_FIELD_MAX (256u * 1024u) /* per-field literal cap (== the worker's fs/text bound) */

typedef struct hc_manifest hc_manifest;

/* One turn to record. `input` (the full prompt/message history at this turn) is HASHED, not stored. The
 * output literals are stored (bounded) for replay AND hashed into the turn digest. Empty/NULL fields are
 * recorded as "". */
typedef struct {
    int         turn_index;
    const char *model;
    const char *input;             /* hashed -> input_sha256 (the growing history; not stored)        */
    const char *assistant_text;    /* the turn's assistant text (stored, bounded)                     */
    const char *tool_calls_json;   /* assembled tool_calls as raw JSON (stored, bounded); "" if none  */
    const char *tool_results_json; /* tool results as raw JSON (stored, bounded); "" if none          */
    const char *finish_reason;
} hc_manifest_turn;

/* One turn read back (for the replay backend + the oracle). Literals are malloc'd. */
typedef struct {
    int    turn_index;
    char   model[128];
    char   input_sha256[HC_SHA256_HEX_LEN];
    char  *assistant_text;     /* malloc'd literal */
    char  *tool_calls_json;    /* malloc'd literal */
    char  *tool_results_json;  /* malloc'd literal */
    char   finish_reason[32];
    char   turn_sha256[HC_SHA256_HEX_LEN]; /* the oracle's per-turn compare key */
} hc_manifest_rec;

/* Open (creating the dir if needed) a manifest for the session rooted at `session_dir`. NULL on failure. */
hc_manifest *hc_manifest_open(const char *session_dir);
void         hc_manifest_close(hc_manifest *);

/* Append one turn's record. The literal fields are bounded (over-long is rejected). 0 on success, -1 on
 * a bad arg / a bound exceeded / I/O failure. */
int hc_manifest_append(hc_manifest *, const hc_manifest_turn *);

/* Read back every recorded turn, in turn order. 0 on success (*out malloc'd, freed by
 * hc_manifest_recs_free; *n_out set; a missing log is success with 0 rows), -1 on OOM / over-cap log. */
int  hc_manifest_read(hc_manifest *, hc_manifest_rec **out, size_t *n_out);
void hc_manifest_recs_free(hc_manifest_rec *, size_t n);

/* The replay divergence ORACLE (P03). Compare a re-derived manifest (`got`, from re-driving a recorded
 * session) against the recorded one (`want`), turn by turn, on the stable `turn_sha256` digest. Pure +
 * offline. */
typedef struct {
    bool   match;        /* true iff same length AND every turn_sha256 matches                       */
    size_t matched;      /* number of leading turns that matched                                     */
    int    first_diff;   /* turn index of the first divergence, or -1 if none                        */
    char   detail[256];  /* human-readable description of the first divergence (or "match")          */
} hc_manifest_diff_report;

void hc_manifest_diff(const hc_manifest_rec *want, size_t n_want, const hc_manifest_rec *got,
                      size_t n_got, hc_manifest_diff_report *out);

#ifdef __cplusplus
}
#endif
#endif /* HC_MANIFEST_H */
