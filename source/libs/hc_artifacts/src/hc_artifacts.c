/* hc_artifacts — content-addressed object store + append-only provenance log. See hc_artifacts.h.
 *
 * Storage layout under the store root:
 *   objects/<id[0:2]>/<id[2:]>   — one file per artifact, named by its sha256 (git-style fan-out)
 *   provenance.jsonl             — one JSON object per line: {id,agent,task,agenda,tool,label,size,created}
 *
 * Filesystem work goes through the shared libs/hc_fs (the consolidation the earlier banner pointed at):
 * an object is an atomic temp+fsync+rename, a provenance line is O_APPEND+fsync'd, a whole-file read is
 * size-capped — one crash-safe implementation shared by all the append-only stores. */

#define _DEFAULT_SOURCE 1

#include "hc_artifacts.h"

#include "hc_fs.h"
#include "hc_hash.h"
#include "hc_json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* An artifact (an fs_write output, today) is capped exactly like fs_write itself; the provenance log is
 * bounded so reading it back can never balloon host memory against a long or hostile run. */
#define HC_ARTIFACT_MAX_BYTES (256u * 1024u)
#define HC_PROV_MAX_BYTES (16u * 1024u * 1024u)

struct hc_artifacts {
    char root[1024];
};

/* An id is usable in a path ONLY if it is exactly 64 lowercase-hex chars — which by construction
 * contains no '/' or '.', so it can never traverse out of objects/. */
static bool valid_id(const char *id)
{
    if (!id) return false;
    size_t n = 0;
    for (; id[n]; n++) {
        char c = id[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return n == 64;
}

/* ---- public API ---- */

hc_artifacts *hc_artifacts_open(const char *dir)
{
    if (!dir || !dir[0]) return NULL;
    hc_artifacts *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    if ((size_t)snprintf(a->root, sizeof a->root, "%s", dir) >= sizeof a->root) {
        free(a);
        return NULL;
    }
    char obj[1100];
    if ((size_t)snprintf(obj, sizeof obj, "%s/objects", a->root) >= sizeof obj || hc_fs_mkdirs(obj) != 0) {
        free(a);
        return NULL;
    }
    return a;
}

void hc_artifacts_close(hc_artifacts *a) { free(a); }

int hc_artifacts_put(hc_artifacts *a, const void *bytes, size_t n, char id_out[HC_ARTIFACT_ID_LEN])
{
    if (!a || (!bytes && n) || n > HC_ARTIFACT_MAX_BYTES) return -1;
    unsigned char dg[32];
    hc_sha256(bytes, n, dg);
    char id[HC_ARTIFACT_ID_LEN];
    hc_sha256_hex(dg, id);

    char dir[1100], path[1200];
    if ((size_t)snprintf(dir, sizeof dir, "%s/objects/%.2s", a->root, id) >= sizeof dir) return -1;
    if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, id + 2) >= sizeof path) return -1;

    if (access(path, F_OK) == 0) { /* dedup: identical content already stored */
        memcpy(id_out, id, HC_ARTIFACT_ID_LEN);
        return 0;
    }
    if (hc_fs_mkdirs(dir) != 0) return -1;
    if (hc_fs_atomic_write(path, (const char *)bytes, n) != 0) return -1;
    memcpy(id_out, id, HC_ARTIFACT_ID_LEN);
    return 0;
}

void *hc_artifacts_get(hc_artifacts *a, const char *id, size_t *n_out)
{
    if (!a || !valid_id(id)) return NULL;
    char path[1200];
    if ((size_t)snprintf(path, sizeof path, "%s/objects/%.2s/%s", a->root, id, id + 2) >= sizeof path)
        return NULL;
    return hc_fs_read_file(path, HC_ARTIFACT_MAX_BYTES, n_out);
}

int hc_artifacts_record(hc_artifacts *a, const char *id, const hc_provenance *p)
{
    if (!a || !valid_id(id) || !p) return -1;
    hc_json *o = hc_json_new_object();
    if (!o) return -1;
    char created[32];
    hc_fs_now_iso8601(created, sizeof created);
    bool ok = hc_json_obj_set_str(o, "id", id) &&
              hc_json_obj_set_str(o, "agent", p->agent ? p->agent : "") &&
              hc_json_obj_set_str(o, "task", p->task ? p->task : "") &&
              hc_json_obj_set_str(o, "agenda", p->agenda ? p->agenda : "") &&
              hc_json_obj_set_str(o, "tool", p->tool ? p->tool : "") &&
              hc_json_obj_set_str(o, "label", p->label ? p->label : "") &&
              hc_json_obj_set_int(o, "size", (int64_t)p->size) &&
              hc_json_obj_set_str(o, "created", created);
    /* inputs[] (lineage) reserved for a later slice — fs_write outputs have no tracked inputs. */
    char *line = ok ? hc_json_print(o, false) : NULL;
    hc_json_free(o);
    if (!line) return -1;

    size_t len = strlen(line);
    char  *buf = malloc(len + 2);
    if (!buf) {
        free(line);
        return -1;
    }
    memcpy(buf, line, len);
    buf[len] = '\n';
    char path[1200];
    int  rc = -1;
    if ((size_t)snprintf(path, sizeof path, "%s/provenance.jsonl", a->root) < sizeof path)
        rc = hc_fs_append(path, buf, len + 1);
    free(buf);
    free(line);
    return rc;
}

static void copy_field(char *dst, size_t cap, const hc_json *o, const char *key)
{
    snprintf(dst, cap, "%s", hc_json_get_str(o, key, ""));
}

/* The cap on rows materialized by any single query — bounds the transient allocation regardless of how
 * large the append-only log has grown (a hostile agent could stream many approved writes). A query keeps
 * only the newest `max` matches in a fixed ring, so memory is O(max), not O(log size). */
#define HC_RECS_MAX 4096u

/* Scan provenance.jsonl, collecting up to `max` rows where `key`==`val` (key==NULL matches all),
 * most-recent-first. The log is append-only, so the LAST `max` matches written are the newest — keep
 * them in a fixed-size ring (overwriting the oldest once full) so a huge log cannot balloon host memory
 * (the prior grow-the-whole-array form let a 16 MiB log allocate ~150 MB transiently on the 2 Hz UI
 * path — the security pass flagged it). A missing log is success with zero rows. */
static int read_recs(hc_artifacts *a, const char *key, const char *val, size_t max,
                     hc_artifact_rec **out, size_t *n_out)
{
    *out = NULL;
    *n_out = 0;
    if (max == 0) return 0;
    char path[1200];
    if ((size_t)snprintf(path, sizeof path, "%s/provenance.jsonl", a->root) >= sizeof path) return -1;
    size_t flen = 0;
    char  *data = hc_fs_read_file(path, HC_PROV_MAX_BYTES, &flen);
    if (!data) return 0; /* no log yet (or over-cap) → no rows */

    hc_artifact_rec *ring = calloc(max, sizeof *ring); /* fixed: the newest `max` matches only */
    if (!ring) {
        free(data);
        return -1;
    }
    size_t head = 0, total = 0;
    for (char *p = data; *p;) {
        char  *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        if (llen > 0) {
            hc_json *o = hc_json_parse(p, llen);
            if (o) {
                if (!key || strcmp(hc_json_get_str(o, key, ""), val) == 0) {
                    hc_artifact_rec *r = &ring[head];
                    memset(r, 0, sizeof *r);
                    copy_field(r->id, sizeof r->id, o, "id");
                    copy_field(r->agent, sizeof r->agent, o, "agent");
                    copy_field(r->task, sizeof r->task, o, "task");
                    copy_field(r->agenda, sizeof r->agenda, o, "agenda");
                    copy_field(r->tool, sizeof r->tool, o, "tool");
                    copy_field(r->label, sizeof r->label, o, "label");
                    copy_field(r->created, sizeof r->created, o, "created");
                    r->size = (long)hc_json_get_int(o, "size", -1);
                    head = (head + 1) % max; /* advance; once full this overwrites the oldest */
                    total++;
                }
                hc_json_free(o);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(data);

    size_t           n = total < max ? total : max;
    hc_artifact_rec *recs = n ? malloc(n * sizeof *recs) : NULL;
    if (n && !recs) {
        free(ring);
        return -1;
    }
    for (size_t i = 0; i < n; i++) /* newest-first: walk back from the most-recently-written slot */
        recs[i] = ring[(head + max - 1 - i) % max];
    free(ring);
    *out = recs;
    *n_out = n;
    return 0;
}

int hc_artifacts_by_task(hc_artifacts *a, const char *task, hc_artifact_rec **out, size_t *n_out)
{
    if (!a || !task) return -1;
    return read_recs(a, "task", task, HC_RECS_MAX, out, n_out);
}

int hc_artifacts_history(hc_artifacts *a, const char *label, hc_artifact_rec **out, size_t *n_out)
{
    if (!a || !label) return -1;
    return read_recs(a, "label", label, HC_RECS_MAX, out, n_out);
}

int hc_artifacts_recent(hc_artifacts *a, size_t max, hc_artifact_rec **out, size_t *n_out)
{
    if (!a) return -1;
    if (max > HC_RECS_MAX) max = HC_RECS_MAX; /* bound even an over-large request */
    return read_recs(a, NULL, NULL, max, out, n_out);
}

void hc_artifacts_recs_free(hc_artifact_rec *recs, size_t n)
{
    (void)n; /* reserved — hc_artifact_rec is a flat struct, so free(recs) releases the whole array */
    free(recs);
}
