/* hc_memory — scoped semantic-memory store. See hc_memory.h.
 *
 * Storage layout under the store root (host-private 0700):
 *   records.jsonl  — one JSON object per line: {id,scope,text,source,importance,created_ms,row} for a
 *                    write (upsert = a new line, last-wins on rebuild) or {id,deleted} for a forget.
 *   vectors.f32    — the embeddings as raw dim x float32 rows; a record's "row" indexes into it. Raw
 *                    host-endian floats (the store is host-local + ephemeral, so no portable encoding).
 *   meta.json      — {"dim":N}, the store's fixed embedding dimension (adopted on the first write).
 *
 * On open the log is replayed to rebuild an in-RAM index of the LIVE records (latest line per id, minus
 * tombstones), each holding a copied vector + its precomputed L2 norm; queries are a flat exact cosine
 * over that index. Filesystem work goes through the shared libs/hc_fs (O_APPEND+fsync line, temp+rename
 * meta, size-capped read). This library NEVER calls hc_llm. */

#define _DEFAULT_SOURCE 1 /* strnlen */

#include "hc_memory.h"

#include "hc_fs.h"
#include "hc_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bounds (DoS guards on an untrusted multi-process store). The log has a SOFT cap (writes stop there)
 * below the HARD cap (the read cap), so a forget — which appends a tombstone to REMOVE content — always
 * fits in the headroom and is never blocked by a growth guard (a tombstone reducing content must not be
 * gated like a write that adds it). Reclaiming the dead log + orphan vector rows wants a compaction pass;
 * that is deferred (logged) — the MVP fails closed on growth and relies on forget for remediation. */
#define HC_MEM_MAX_RECORDS 5000u                       /* live records (RAM + flat-search bound)     */
#define HC_MEM_LOG_MAX     (64u * 1024u * 1024u)       /* records.jsonl read + hard cap (forget)     */
#define HC_MEM_LOG_SOFT    (HC_MEM_LOG_MAX - (1u << 20)) /* write cap; 1 MiB headroom for tombstones */
#define HC_MEM_VEC_MAX     (512u * 1024u * 1024u)      /* vectors.f32 read cap                       */

typedef struct {
    char     id[HC_MEM_ID_LEN];
    char     scope[HC_MEM_SCOPE_MAX];
    char    *text;   /* malloc'd */
    char    *source; /* malloc'd */
    double   importance;
    uint64_t created_ms;
    float   *vec;  /* malloc'd, dim floats */
    float    norm; /* precomputed L2 norm of vec */
} mem_entry;

struct hc_memory {
    char       root[1024];
    int        dim;       /* 0 until the first write adopts it */
    size_t     vec_rows;  /* physical rows in vectors.f32 (live + orphaned-by-upsert)        */
    size_t     log_bytes; /* current records.jsonl size, to bound growth without a per-write stat */
    mem_entry *entries;   /* the live index */
    size_t     n, cap;
};

/* ---- ids, validation, math ---- */

/* Content id = FNV-1a 64-bit of scope, a unit separator, then text -> 16 lowercase-hex. A dedup handle
 * (identical scope|text -> one record), never a path component (no per-record files exist). */
static void compute_id(const char *scope, const char *text, char out[HC_MEM_ID_LEN])
{
    uint64_t           h = 1469598103934665603ULL;
    const char        *parts[3] = {scope, "\x1f", text};
    for (int p = 0; p < 3; p++)
        for (const unsigned char *s = (const unsigned char *)parts[p]; *s; s++) {
            h ^= *s;
            h *= 1099511628211ULL;
        }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[i] = hex[(h >> ((15 - i) * 4)) & 0xF];
    out[16] = '\0';
}

static bool valid_id(const char *id)
{
    if (!id) return false;
    size_t n = 0;
    for (; id[n]; n++) {
        char c = id[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return n == 16;
}

/* A required string field: non-empty and shorter than `cap` (so it fits with a NUL). */
static bool bounded_nonempty(const char *s, size_t cap)
{
    return s && s[0] && strnlen(s, cap) < cap;
}

static float l2norm(const float *v, int dim)
{
    double s = 0.0;
    for (int i = 0; i < dim; i++) s += (double)v[i] * (double)v[i];
    return (float)sqrt(s);
}

static float cosine(const float *a, float na, const float *b, float nb, int dim)
{
    if (!(na > 0.0f) || !(nb > 0.0f)) return 0.0f; /* zero vector -> undefined; treat as no match */
    double dot = 0.0;
    for (int i = 0; i < dim; i++) dot += (double)a[i] * (double)b[i];
    double c = dot / ((double)na * (double)nb);
    if (c > 1.0) c = 1.0;
    else if (c < -1.0) c = -1.0;
    return (float)c;
}

static bool scope_allowed(const char *scope, const char *const *scopes, size_t n_scopes)
{
    if (n_scopes == 0) return true; /* unrestricted */
    for (size_t i = 0; i < n_scopes; i++)
        if (scopes[i] && strcmp(scope, scopes[i]) == 0) return true;
    return false;
}

/* ---- in-RAM index ---- */

static void entry_free(mem_entry *e)
{
    free(e->text);
    free(e->source);
    free(e->vec);
    e->text = e->source = NULL;
    e->vec = NULL;
}

static mem_entry *find_entry(hc_memory *m, const char *id)
{
    for (size_t i = 0; i < m->n; i++)
        if (strcmp(m->entries[i].id, id) == 0) return &m->entries[i];
    return NULL;
}

static void remove_at(hc_memory *m, size_t i)
{
    entry_free(&m->entries[i]);
    m->entries[i] = m->entries[m->n - 1]; /* swap-remove (order rebuilt from created_ms when listing) */
    m->n--;
}

/* Upsert {id,scope,text,source,importance,created_ms,vec(dim)} into the index (copies all). -1 on OOM. */
static int index_upsert(hc_memory *m, const char *id, const char *scope, const char *text,
                        const char *source, double importance, uint64_t created_ms, const float *vec)
{
    char  *t = strdup(text);
    char  *s = strdup(source ? source : "");
    float *v = malloc((size_t)m->dim * sizeof *v);
    if (!t || !s || !v) {
        free(t);
        free(s);
        free(v);
        return -1;
    }
    memcpy(v, vec, (size_t)m->dim * sizeof *v);

    mem_entry *e = find_entry(m, id);
    if (e) {
        entry_free(e); /* refresh in place */
    } else {
        if (m->n == m->cap) {
            size_t     ncap = m->cap ? m->cap * 2 : 64;
            mem_entry *ne = realloc(m->entries, ncap * sizeof *ne);
            if (!ne) {
                free(t);
                free(s);
                free(v);
                return -1;
            }
            m->entries = ne;
            m->cap = ncap;
        }
        e = &m->entries[m->n++];
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", id);
        snprintf(e->scope, sizeof e->scope, "%s", scope);
    }
    e->text = t;
    e->source = s;
    e->vec = v;
    e->norm = l2norm(v, m->dim);
    e->importance = importance;
    e->created_ms = created_ms;
    return 0;
}

/* ---- open / rebuild ---- */

/* Replay the log into the in-RAM index. Returns 0 on success, -1 ONLY on an OOM building the index (the
 * open then fails rather than hand back a silently-truncated store). A missing/empty/over-cap file is a
 * clean empty store, not an error. The live cap is honoured here too (not just on the write path), so a
 * hostile/corrupt log that names more than HC_MEM_MAX_RECORDS distinct ids cannot balloon the index — the
 * first HC_MEM_MAX_RECORDS distinct ids win (deterministic, log order). */
static int rebuild(hc_memory *m)
{
    char meta[1200];
    if ((size_t)snprintf(meta, sizeof meta, "%s/meta.json", m->root) < sizeof meta) {
        size_t mlen = 0;
        char  *md = hc_fs_read_file(meta, 64u * 1024u, &mlen);
        if (md) {
            hc_json *o = hc_json_parse(md, mlen);
            if (o) {
                int64_t d = hc_json_get_int(o, "dim", 0);
                if (d > 0 && d <= HC_MEM_MAX_DIM) m->dim = (int)d;
                hc_json_free(o);
            }
            free(md);
        }
    }
    if (m->dim <= 0) return 0; /* a store with no meta has no records yet */

    char vpath[1200], rpath[1200];
    if ((size_t)snprintf(vpath, sizeof vpath, "%s/vectors.f32", m->root) >= sizeof vpath
        || (size_t)snprintf(rpath, sizeof rpath, "%s/records.jsonl", m->root) >= sizeof rpath)
        return 0;

    size_t vlen = 0;
    char  *vecs = hc_fs_read_file(vpath, HC_MEM_VEC_MAX, &vlen);
    size_t row_bytes = (size_t)m->dim * sizeof(float);
    size_t rows = vecs ? vlen / row_bytes : 0;
    m->vec_rows = rows;

    int    rc = 0;
    size_t rlen = 0;
    char  *data = hc_fs_read_file(rpath, HC_MEM_LOG_MAX, &rlen);
    if (data) {
        m->log_bytes = rlen;
        for (char *p = data; *p;) {
            char  *nl = strchr(p, '\n');
            size_t llen = nl ? (size_t)(nl - p) : strlen(p);
            if (llen > 0) {
                hc_json *o = hc_json_parse(p, llen);
                if (o) {
                    const char *id = hc_json_get_str(o, "id", "");
                    if (valid_id(id)) {
                        if (hc_json_get_bool(o, "deleted", false)) {
                            mem_entry *e = find_entry(m, id);
                            if (e) remove_at(m, (size_t)(e - m->entries));
                        } else {
                            int64_t row = hc_json_get_int(o, "row", -1);
                            bool    exists = find_entry(m, id) != NULL;
                            if (vecs && row >= 0 && (size_t)row < rows
                                && (exists || m->n < HC_MEM_MAX_RECORDS) /* H1: cap on rebuild */
                                && index_upsert(m, id, hc_json_get_str(o, "scope", ""),
                                                hc_json_get_str(o, "text", ""),
                                                hc_json_get_str(o, "source", ""),
                                                hc_json_get_double(o, "importance", 0.0),
                                                (uint64_t)hc_json_get_int(o, "created_ms", 0),
                                                (const float *)(vecs + (size_t)row * row_bytes))
                                       != 0)
                                rc = -1; /* OOM — fail the open (M2: never silently truncate) */
                        }
                    }
                    hc_json_free(o);
                }
            }
            if (rc != 0 || !nl) break;
            p = nl + 1;
        }
        free(data);
    }
    free(vecs);
    return rc;
}

hc_memory *hc_memory_open(const char *dir)
{
    if (!dir || !dir[0]) return NULL;
    hc_memory *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if ((size_t)snprintf(m->root, sizeof m->root, "%s", dir) >= sizeof m->root || hc_fs_mkdirs(m->root) != 0) {
        free(m);
        return NULL;
    }
    if (rebuild(m) != 0) { /* an OOM building the index — fail rather than serve a truncated store */
        hc_memory_close(m);
        return NULL;
    }
    return m;
}

void hc_memory_close(hc_memory *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->n; i++) entry_free(&m->entries[i]);
    free(m->entries);
    free(m);
}

int    hc_memory_dim(const hc_memory *m) { return m ? m->dim : 0; }
size_t hc_memory_count(const hc_memory *m) { return m ? m->n : 0; }

/* ---- write ---- */

int hc_memory_write(hc_memory *m, const hc_mem_record *r, char id_out[HC_MEM_ID_LEN])
{
    if (!m || !r || !r->vec || r->dim <= 0 || r->dim > HC_MEM_MAX_DIM) return -1;
    if (!bounded_nonempty(r->scope, HC_MEM_SCOPE_MAX) || !bounded_nonempty(r->text, HC_MEM_TEXT_MAX))
        return -1;
    if (r->source && strnlen(r->source, HC_MEM_SOURCE_MAX) >= HC_MEM_SOURCE_MAX) return -1;
    for (int i = 0; i < r->dim; i++)
        if (!isfinite(r->vec[i])) return -1;          /* never store a poisoned (NaN/Inf) vector  */
    if (!(l2norm(r->vec, r->dim) > 0.0f)) return -1;  /* nor a degenerate all-zero one (cosine 0) */

    if (m->dim == 0) { /* first write fixes the dimension (one-way door) */
        char meta[1200], body[64];
        int  bl = snprintf(body, sizeof body, "{\"dim\":%d}\n", r->dim);
        if (bl < 0 || (size_t)snprintf(meta, sizeof meta, "%s/meta.json", m->root) >= sizeof meta) return -1;
        if (hc_fs_atomic_write(meta, body, (size_t)bl) != 0) return -1;
        m->dim = r->dim;
    } else if (r->dim != m->dim) {
        return -1; /* dimension mismatch — rejected */
    }

    char id[HC_MEM_ID_LEN];
    compute_id(r->scope, r->text, id);
    bool exists = find_entry(m, id) != NULL;
    if (!exists && m->n >= HC_MEM_MAX_RECORDS) return -1; /* live cap (new ids only) */

    /* build the record line first so we can bound the log before touching the vector file */
    hc_json *o = hc_json_new_object();
    if (!o) return -1;
    bool ok = hc_json_obj_set_str(o, "id", id) && hc_json_obj_set_str(o, "scope", r->scope)
              && hc_json_obj_set_str(o, "text", r->text)
              && hc_json_obj_set_str(o, "source", r->source ? r->source : "")
              && hc_json_obj_set_double(o, "importance", r->importance)
              && hc_json_obj_set_int(o, "created_ms", (int64_t)r->created_ms)
              && hc_json_obj_set_int(o, "row", (int64_t)m->vec_rows);
    char *line = ok ? hc_json_print(o, false) : NULL;
    hc_json_free(o);
    if (!line) return -1;
    size_t llen = strlen(line);
    if (m->log_bytes + llen + 1 > HC_MEM_LOG_SOFT) { /* soft write cap; headroom left for forgets */
        free(line);
        return -1;
    }

    char vpath[1200], rpath[1200];
    if ((size_t)snprintf(vpath, sizeof vpath, "%s/vectors.f32", m->root) >= sizeof vpath
        || (size_t)snprintf(rpath, sizeof rpath, "%s/records.jsonl", m->root) >= sizeof rpath) {
        free(line);
        return -1;
    }
    /* append the vector (its physical row is m->vec_rows), then the record line referencing that row.
     * vec_rows tracks the physical file, so a torn write leaves only a harmless orphan row. */
    if (hc_fs_append(vpath, (const char *)r->vec, (size_t)m->dim * sizeof(float)) != 0) {
        free(line);
        return -1;
    }
    m->vec_rows++;
    char *buf = malloc(llen + 1);
    if (!buf) {
        free(line);
        return -1;
    }
    memcpy(buf, line, llen);
    buf[llen] = '\n';
    int rc = hc_fs_append(rpath, buf, llen + 1);
    free(buf);
    free(line);
    if (rc != 0) return -1;
    m->log_bytes += llen + 1;

    if (index_upsert(m, id, r->scope, r->text, r->source, r->importance, r->created_ms, r->vec) != 0)
        return -1; /* persisted but index update OOM'd — a reopen would recover it */
    if (id_out) memcpy(id_out, id, HC_MEM_ID_LEN);
    return 0;
}

/* ---- query / list ---- */

typedef struct {
    size_t idx;
    float  cosine;
} scored;

static int by_cosine_desc(const void *a, const void *b)
{
    float ca = ((const scored *)a)->cosine, cb = ((const scored *)b)->cosine;
    return (ca < cb) - (ca > cb);
}

static int emit_hits(hc_memory *m, const scored *sc, size_t n, hc_mem_hit **out, size_t *n_out)
{
    hc_mem_hit *hits = n ? calloc(n, sizeof *hits) : NULL;
    if (n && !hits) return -1;
    for (size_t i = 0; i < n; i++) {
        const mem_entry *e = &m->entries[sc[i].idx];
        hc_mem_hit      *h = &hits[i];
        snprintf(h->id, sizeof h->id, "%s", e->id);
        snprintf(h->scope, sizeof h->scope, "%s", e->scope);
        h->text = strdup(e->text);
        h->source = strdup(e->source);
        h->importance = e->importance;
        h->created_ms = e->created_ms;
        h->cosine = sc[i].cosine;
        if (!h->text || !h->source) {
            hc_memory_hits_free(hits, i + 1);
            return -1;
        }
    }
    *out = hits;
    *n_out = n;
    return 0;
}

int hc_memory_query(hc_memory *m, const float *qvec, int dim, const char *const *scopes, size_t n_scopes,
                    size_t max_hits, hc_mem_hit **out, size_t *n_out)
{
    if (out) *out = NULL;
    if (n_out) *n_out = 0;
    if (!m || !qvec || !out || !n_out || dim <= 0) return -1;
    if (m->dim == 0 || m->n == 0) return 0; /* empty store */
    if (dim != m->dim) return -1;
    if (max_hits == 0) return 0;
    if (max_hits > HC_MEM_MAX_HITS) max_hits = HC_MEM_MAX_HITS;

    float qn = l2norm(qvec, m->dim);
    scored *sc = malloc(m->n * sizeof *sc);
    if (!sc) return -1;
    size_t k = 0;
    for (size_t i = 0; i < m->n; i++) {
        if (!scope_allowed(m->entries[i].scope, scopes, n_scopes)) continue; /* filter BEFORE scoring */
        sc[k].idx = i;
        sc[k].cosine = cosine(qvec, qn, m->entries[i].vec, m->entries[i].norm, m->dim);
        k++;
    }
    qsort(sc, k, sizeof *sc, by_cosine_desc);
    if (k > max_hits) k = max_hits;
    int rc = emit_hits(m, sc, k, out, n_out);
    free(sc);
    return rc;
}

int hc_memory_list(hc_memory *m, size_t max, hc_mem_hit **out, size_t *n_out)
{
    if (out) *out = NULL;
    if (n_out) *n_out = 0;
    if (!m || !out || !n_out) return -1;
    if (m->n == 0) return 0;
    if (max == 0) return 0;
    if (max > HC_MEM_MAX_HITS) max = HC_MEM_MAX_HITS;

    scored *sc = malloc(m->n * sizeof *sc);
    if (!sc) return -1;
    for (size_t i = 0; i < m->n; i++) {
        sc[i].idx = i;
        sc[i].cosine = 0.0f; /* list has no query; cosine is reported as 0 */
    }
    /* most-recent-first: insertion sort the index array by each entry's created_ms, descending (n is
     * bounded by HC_MEM_MAX_RECORDS, so O(n^2) worst case is fine for this UI-facing listing). */
    for (size_t i = 1; i < m->n; i++) {
        scored   key = sc[i];
        uint64_t kc = m->entries[key.idx].created_ms;
        size_t   j = i;
        while (j > 0 && m->entries[sc[j - 1].idx].created_ms < kc) {
            sc[j] = sc[j - 1];
            j--;
        }
        sc[j] = key;
    }
    size_t k = m->n < max ? m->n : max;
    int    rc = emit_hits(m, sc, k, out, n_out);
    free(sc);
    return rc;
}

int hc_memory_forget(hc_memory *m, const char *id)
{
    if (!m || !valid_id(id)) return -1;
    mem_entry *e = find_entry(m, id);
    if (!e) return 0; /* idempotent — already absent */

    char    rpath[1200], line[64];
    int     ll = snprintf(line, sizeof line, "{\"id\":\"%s\",\"deleted\":true}\n", id);
    if (ll < 0 || (size_t)snprintf(rpath, sizeof rpath, "%s/records.jsonl", m->root) >= sizeof rpath)
        return -1;
    /* gated by the HARD cap, not the soft write cap: a tombstone REMOVES content, so it draws on the
     * 1 MiB headroom above the write cap and is effectively never refused (live records are bounded, so
     * the total tombstone bytes are too) — remediation must not fail closed. */
    if (m->log_bytes + (size_t)ll > HC_MEM_LOG_MAX) return -1;
    if (hc_fs_append(rpath, line, (size_t)ll) != 0) return -1;
    m->log_bytes += (size_t)ll;
    remove_at(m, (size_t)(e - m->entries));
    return 0;
}

void hc_memory_hits_free(hc_mem_hit *hits, size_t n)
{
    if (!hits) return;
    for (size_t i = 0; i < n; i++) {
        free(hits[i].text);
        free(hits[i].source);
    }
    free(hits);
}
