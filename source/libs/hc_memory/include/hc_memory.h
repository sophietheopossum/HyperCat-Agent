#ifndef HC_MEMORY_H
#define HC_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_memory — a scoped semantic-memory store: text records + embedding vectors + metadata, with a flat
 * exact cosine query. The storage + math primitive behind P01 (the host computes vectors via hc_llm and
 * hands them IN; this library NEVER calls hc_llm, keeping the dependency DAG acyclic).
 *
 * Purpose:   let the fleet RECALL — store a memory (a piece of model/operator-authored text + the
 *            embedding of that text + a scope + provenance) and answer "the K most similar memories to
 *            this query vector, within these scopes." The retrieval POLICY (recency x relevance x
 *            salience blend, the final top-k) lives in the host; this library returns raw cosine +
 *            metadata so the policy can evolve without an ABI change.
 * Owns:      the store root path + an in-RAM index of the LIVE records (rebuilt on open by scanning the
 *            log). Persistence: an append-only `records.jsonl` (upsert = append, last-wins; forget =
 *            a tombstone line) + a sidecar `vectors.f32` (dim x float32 rows, referenced by row index)
 *            + `meta.json` (the store's fixed dimension). Crash-safe by construction (O_APPEND+fsync
 *            lines, temp+rename meta) and rebuilt by scanning — the hc_store / hc_artifacts discipline,
 *            so no central mutable index to corrupt. Single-writer (the host broker thread).
 * Threading: an hc_memory handle is single-owner; not safe to share across threads at once.
 * Lifetime:  hc_mem_hit arrays are released with hc_memory_hits_free (each hit owns malloc'd text +
 *            source). Strings passed in (hc_mem_record fields) are borrowed for the call.
 * Security:  text is UNTRUSTED (model-authored) — the store only stores + returns it, never interprets
 *            it; the host renders it via "%s" and fences it as reference-not-instruction. The
 *            DIMENSION is fixed per store on first write (a one-way door) and a mismatched write is
 *            rejected. Text / scope / source lengths, the live record count, and recall size are all
 *            bounded so a hostile agent cannot balloon the store or exfiltrate it via a giant recall.
 *            The record id is a content-addressed DEDUP HANDLE (FNV-1a of scope|text), NOT a path
 *            component (there are no per-record files) and NOT a tamper-evidence checksum — the
 *            security of memory is the host's scope enforcement + mandatory provenance + the human gate
 *            on `shared`, not the id. The store lives under the host-private 0700 tree.
 * Portable:  POSIX (Linux + macOS). The small filesystem helpers mirror hc_store's proven primitives
 *            (a shared libs/hc_fs is the planned consolidation once several stores want them).
 */

#define HC_MEM_ID_LEN     17   /* 16 lowercase-hex (FNV-1a 64-bit) + NUL                          */
#define HC_MEM_SCOPE_MAX  64   /* "agent:A" | "shared" | "agenda:<id>"                            */
#define HC_MEM_SOURCE_MAX 256  /* "session:<id>#turn7" | "operator" | "distill:<id>"             */
#define HC_MEM_TEXT_MAX   (8u * 1024u) /* one memory record's text cap (untrusted)               */
#define HC_MEM_MAX_DIM    16384        /* defensive upper bound on the embedding dimension        */
#define HC_MEM_MAX_HITS   256          /* cap on rows a single query may return                   */
#define HC_MEM_MODEL_MAX  256          /* embedding model id recorded in meta.json (incl. NUL)    */

typedef struct hc_memory hc_memory;

/* A memory to store. `vec` is `dim` floats (the embedding of `text`, computed by the host); the store
 * copies it. The id is derived from scope|text (the caller does not supply one). All strings borrowed. */
typedef struct {
    const char  *scope;      /* recall domain; bounded, non-empty                       */
    const char  *text;       /* the memory itself; bounded (HC_MEM_TEXT_MAX), untrusted  */
    const char  *source;     /* provenance (where it came from); bounded                 */
    const float *vec;        /* embedding (the host computed it); copied in              */
    int          dim;        /* vector dimension; must match the store's fixed dim       */
    double       importance; /* 0..1 salience (host- or rule-assigned)                   */
    uint64_t     created_ms; /* creation time (for the host's recency factor)            */
} hc_mem_record;

/* One recall result: the stored memory + its RAW cosine to the query (the host applies the blend). */
typedef struct {
    char     id[HC_MEM_ID_LEN];
    char     scope[HC_MEM_SCOPE_MAX];
    char    *text;       /* malloc'd copy (freed by hc_memory_hits_free) */
    char    *source;     /* malloc'd copy (freed by hc_memory_hits_free) */
    double   importance;
    uint64_t created_ms;
    float    cosine;     /* raw relevance in [-1,1]; 0 for hc_memory_list */
} hc_mem_hit;

/* Open (creating if absent) a memory store rooted at `dir` (a host-private directory). The dimension is
 * adopted from the first write and persisted; a store reopened keeps it. NULL on failure. */
hc_memory *hc_memory_open(const char *dir);

/* As hc_memory_open, but also binds the store to the embedding model that produced its vectors.
 *
 * WHY THIS EXISTS: the dimension check alone is not a safety property. Two DIFFERENT embedding models
 * that happen to share an output dimension produce entirely incompatible vector spaces -- each arranges
 * meaning by its own learned coordinates -- so mixing them passes every length check and silently
 * returns nonsense from recall. The loud failure (a dimension mismatch) was always the harmless one;
 * this closes the quiet one.
 *
 * `model` is the caller's CONFIGURED embedding model id (NULL or "" = unknown, no enforcement, exactly
 * the old behaviour). On the first write it is persisted into meta.json alongside the dimension. If the
 * store already records a DIFFERENT id AND holds at least one live record, the store opens in a
 * refusing state: hc_memory_write and hc_memory_query fail with -1 and hc_memory_model_mismatch()
 * reports 1, so the host can say which two ids disagree instead of serving corrupt recall. Opening
 * still succeeds so that the caller can report the problem -- returning NULL here would be
 * indistinguishable from "memory is switched off", which is precisely the diagnosis this exists to
 * make possible. For that same reason an over-long or unprintable `model` does NOT fail the open: it
 * is a configuration error, so the store opens with enforcement OFF and the host is expected to
 * validate the id and report it.
 *
 * NOT gated by a mismatch: hc_memory_list and hc_memory_forget stay legal, deliberately. They are the
 * remediation surface -- an operator must be able to see and drop what is in a store they can no
 * longer query. Only the two operations that would score or add vectors are refused.
 *
 * REBIND: a store with no live records (never successfully written, or everything since forgotten) has
 * no vectors for a foreign model to be incomparable with, so it rebinds on the next write instead of
 * refusing forever. A store that was already refusing when it was opened stops refusing the moment
 * hc_memory_forget drops its last record -- remediation through the panel therefore works without a
 * restart. Note this covers only the model; the DIMENSION remains a one-way door, because
 * vectors.f32 is a flat array of fixed-width rows and re-adopting a width would misalign every
 * existing row. A store stuck on an unwanted dimension must be deleted.
 *
 * ADOPT: a store written before this field existed records no model; it adopts the configured one on
 * its next write rather than being condemned, since its vectors are in fact from whatever model was in
 * use then. That id is an ASSUMPTION, not a record -- hc_memory_model_adopted() reports 1 for it, and
 * a host must not present it to a user as though the store had witnessed it. */
hc_memory *hc_memory_open_model(const char *dir, const char *model);

void       hc_memory_close(hc_memory *);

/* The embedding model id recorded in the store, or "" if none is recorded (a pre-model store, or one
 * that has never been written to). Borrowed, valid until hc_memory_close. */
const char *hc_memory_model(const hc_memory *);

/* 1 when the configured model disagrees with the one recorded in the store -- hc_memory_write and
 * hc_memory_query are being refused (list/forget are not). 0 otherwise. Pair with hc_memory_model()
 * to report both sides. */
int hc_memory_model_mismatch(const hc_memory *);

/* 1 when hc_memory_model() was ADOPTED retroactively (a store predating the field, or one rebound
 * while empty) rather than recorded at the store's own first write. The id is then this library's
 * assumption about vectors it did not witness being made, so report it as such. */
int hc_memory_model_adopted(const hc_memory *);

/* The store's fixed embedding dimension, or 0 if nothing has been written yet. */
int    hc_memory_dim(const hc_memory *);
/* Number of LIVE records (after dedup + tombstones). */
size_t hc_memory_count(const hc_memory *);

/* Upsert a memory (dedup by content id = hash(scope|text); an existing id is refreshed). Sets the
 * store's dimension AND embedding model on the first write; a later write whose `dim` differs, or which
 * is made against a store bound to a different embedding model, is REJECTED. id_out (optional;
 * HC_MEM_ID_LEN incl. NUL) receives the content id. 0 on success, -1 on a bad arg / dim mismatch /
 * non-finite-or-zero vector / a bound exceeded (text/scope too long, the live or log cap reached) / I/O
 * failure. NOTE on the one durability subtlety: if the bytes were written but the in-RAM index update
 * then OOM'd, this returns -1 yet the record IS persisted — `hc_memory_count()` will lag until the next
 * `hc_memory_open` recovers it. A plain I/O-failure -1 means nothing was persisted. The caller may treat
 * either -1 as retriable. */
int hc_memory_write(hc_memory *, const hc_mem_record *, char id_out[HC_MEM_ID_LEN]);

/* Top matches to `qvec` (dim must equal the store's), restricted to the given `scopes` (the recall
 * domain — `n_scopes`==0 means all; the filter is applied BEFORE scoring so a non-matching record can
 * never leak via ranking). Returns up to `max_hits` (capped at HC_MEM_MAX_HITS) rows, sorted by cosine
 * descending, as the candidate pool for the host's recency x salience blend. 0 on success (*out malloc'd,
 * freed by hc_memory_hits_free; *n_out set), -1 on a bad arg / dim mismatch / OOM. A query against an
 * empty store is success with 0 rows. */
int hc_memory_query(hc_memory *, const float *qvec, int dim, const char *const *scopes, size_t n_scopes,
                    size_t max_hits, hc_mem_hit **out, size_t *n_out);

/* The live records, most-recent-first, up to `max` (capped at HC_MEM_MAX_HITS) — for the UI Memory
 * panel. cosine is 0. Same ownership contract as hc_memory_query. */
int hc_memory_list(hc_memory *, size_t max, hc_mem_hit **out, size_t *n_out);

/* Delete a record by id (appends a tombstone + drops it from the index). Idempotent: 0 whether the id
 * was live or already absent; -1 only on a bad id / I/O failure. */
int hc_memory_forget(hc_memory *, const char *id);

void hc_memory_hits_free(hc_mem_hit *, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* HC_MEMORY_H */
