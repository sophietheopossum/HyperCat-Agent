/* hc_manifest — per-turn append-only audit + replay record. See hc_manifest.h.
 *
 * One manifest.jsonl per session (beside the transcript). Each line is one turn:
 *   {turn, model, input_sha256, assistant, tool_calls, tool_results, finish_reason, turn_sha256}
 * input_sha256 hashes the (growing) input history so it is captured without being stored; the output
 * literals are stored bounded for the replay backend AND folded into turn_sha256. turn_sha256 is the
 * SHA-256 of the CANONICAL record (computed before turn_sha256 itself is added), so two identical runs
 * produce byte-identical digests — the divergence oracle's compare key. Crash-safe + scan-to-rebuild via
 * hc_fs, exactly like hc_store/hc_artifacts. Never calls hc_llm. */

#define _DEFAULT_SOURCE 1 /* strnlen */

#include "hc_manifest.h"

#include "hc_fs.h"
#include "hc_hash.h"
#include "hc_json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HC_MANIFEST_LOG_MAX (64u * 1024u * 1024u) /* read cap — bounds host memory on read-back */

struct hc_manifest {
    char dir[1024];
};

hc_manifest *hc_manifest_open(const char *session_dir)
{
    if (!session_dir || !session_dir[0]) return NULL;
    hc_manifest *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if ((size_t)snprintf(m->dir, sizeof m->dir, "%s", session_dir) >= sizeof m->dir
        || hc_fs_mkdirs(m->dir) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

void hc_manifest_close(hc_manifest *m) { free(m); }

int hc_manifest_append(hc_manifest *m, const hc_manifest_turn *t)
{
    if (!m || !t) return -1;
    const char *model = t->model ? t->model : "";
    const char *input = t->input ? t->input : "";
    const char *atext = t->assistant_text ? t->assistant_text : "";
    const char *tcalls = t->tool_calls_json ? t->tool_calls_json : "";
    const char *tresults = t->tool_results_json ? t->tool_results_json : "";
    const char *finish = t->finish_reason ? t->finish_reason : "";
    /* bound the STORED literals (input is only hashed, so it needs no cap) */
    if (strnlen(atext, HC_MANIFEST_FIELD_MAX) >= HC_MANIFEST_FIELD_MAX
        || strnlen(tcalls, HC_MANIFEST_FIELD_MAX) >= HC_MANIFEST_FIELD_MAX
        || strnlen(tresults, HC_MANIFEST_FIELD_MAX) >= HC_MANIFEST_FIELD_MAX)
        return -1;

    char input_h[HC_SHA256_HEX_LEN];
    hc_sha256_hex_str(input, strlen(input), input_h);

    hc_json *o = hc_json_new_object();
    if (!o) return -1;
    bool ok = hc_json_obj_set_int(o, "turn", t->turn_index) && hc_json_obj_set_str(o, "model", model)
              && hc_json_obj_set_str(o, "input_sha256", input_h)
              && hc_json_obj_set_str(o, "assistant", atext)
              && hc_json_obj_set_str(o, "tool_calls", tcalls)
              && hc_json_obj_set_str(o, "tool_results", tresults)
              && hc_json_obj_set_str(o, "finish_reason", finish);
    if (!ok) {
        hc_json_free(o);
        return -1;
    }
    /* turn digest over the CANONICAL record (deterministic across runs), added back as a field */
    char *canon = hc_json_print_canonical(o);
    if (!canon) {
        hc_json_free(o);
        return -1;
    }
    char turn_h[HC_SHA256_HEX_LEN];
    hc_sha256_hex_str(canon, strlen(canon), turn_h);
    free(canon);
    if (!hc_json_obj_set_str(o, "turn_sha256", turn_h)) {
        hc_json_free(o);
        return -1;
    }

    char *line = hc_json_print(o, false);
    hc_json_free(o);
    if (!line) return -1;
    size_t len = strlen(line);
    char  *buf = malloc(len + 1); /* the line + a trailing '\n' (no NUL — hc_fs_append writes raw bytes) */
    if (!buf) {
        free(line);
        return -1;
    }
    memcpy(buf, line, len);
    buf[len] = '\n';
    char path[1200];
    int  rc = -1;
    if ((size_t)snprintf(path, sizeof path, "%s/manifest.jsonl", m->dir) < sizeof path)
        rc = hc_fs_append(path, buf, len + 1);
    free(buf);
    free(line);
    return rc;
}

static void copy_fixed(char *dst, size_t cap, const hc_json *o, const char *key)
{
    snprintf(dst, cap, "%s", hc_json_get_str(o, key, ""));
}

int hc_manifest_read(hc_manifest *m, hc_manifest_rec **out, size_t *n_out)
{
    *out = NULL;
    *n_out = 0;
    if (!m) return -1;
    char path[1200];
    if ((size_t)snprintf(path, sizeof path, "%s/manifest.jsonl", m->dir) >= sizeof path) return -1;
    size_t flen = 0;
    char  *data = hc_fs_read_file(path, HC_MANIFEST_LOG_MAX, &flen);
    if (!data) return 0; /* no log yet (or over-cap) → 0 rows */

    hc_manifest_rec *recs = NULL;
    size_t           n = 0, cap = 0;
    int              rc = 0;
    for (char *p = data; *p && rc == 0;) {
        char  *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        if (llen > 0) {
            hc_json *o = hc_json_parse(p, llen);
            if (o) {
                if (n == cap) {
                    size_t           ncap = cap ? cap * 2 : 16;
                    hc_manifest_rec *nr = realloc(recs, ncap * sizeof *nr);
                    if (!nr) {
                        rc = -1;
                    } else {
                        recs = nr;
                        cap = ncap;
                    }
                }
                if (rc == 0) {
                    hc_manifest_rec *r = &recs[n];
                    memset(r, 0, sizeof *r);
                    r->turn_index = (int)hc_json_get_int(o, "turn", -1);
                    copy_fixed(r->model, sizeof r->model, o, "model");
                    copy_fixed(r->input_sha256, sizeof r->input_sha256, o, "input_sha256");
                    copy_fixed(r->finish_reason, sizeof r->finish_reason, o, "finish_reason");
                    copy_fixed(r->turn_sha256, sizeof r->turn_sha256, o, "turn_sha256");
                    r->assistant_text = strdup(hc_json_get_str(o, "assistant", ""));
                    r->tool_calls_json = strdup(hc_json_get_str(o, "tool_calls", ""));
                    r->tool_results_json = strdup(hc_json_get_str(o, "tool_results", ""));
                    if (!r->assistant_text || !r->tool_calls_json || !r->tool_results_json) {
                        free(r->assistant_text);
                        free(r->tool_calls_json);
                        free(r->tool_results_json);
                        rc = -1;
                    } else {
                        n++;
                    }
                }
                hc_json_free(o);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(data);
    if (rc != 0) {
        hc_manifest_recs_free(recs, n);
        return -1;
    }
    *out = recs;
    *n_out = n;
    return 0;
}

void hc_manifest_recs_free(hc_manifest_rec *recs, size_t n)
{
    if (!recs) return;
    for (size_t i = 0; i < n; i++) {
        free(recs[i].assistant_text);
        free(recs[i].tool_calls_json);
        free(recs[i].tool_results_json);
    }
    free(recs);
}

void hc_manifest_diff(const hc_manifest_rec *want, size_t n_want, const hc_manifest_rec *got,
                      size_t n_got, hc_manifest_diff_report *out)
{
    if (!out) return;
    out->match = false;
    out->matched = 0;
    out->first_diff = -1;
    out->detail[0] = '\0';

    size_t n = n_want < n_got ? n_want : n_got;
    for (size_t i = 0; i < n; i++) {
        /* turn_sha256 is the canonical per-turn digest (model + input hash + literals + finish); one
         * compare per turn is the whole oracle — a divergence anywhere in a turn changes its digest. */
        if (strcmp(want[i].turn_sha256, got[i].turn_sha256) != 0) {
            out->matched = i;
            out->first_diff = want[i].turn_index;
            snprintf(out->detail, sizeof out->detail,
                     "turn %d: digest mismatch (want %.12s… got %.12s…)", want[i].turn_index,
                     want[i].turn_sha256, got[i].turn_sha256);
            return;
        }
    }
    out->matched = n;
    if (n_want != n_got) { /* a common prefix matched, but the runs have different lengths */
        out->first_diff = (int)n;
        snprintf(out->detail, sizeof out->detail, "turn count differs: recorded %zu, replayed %zu",
                 n_want, n_got);
        return;
    }
    out->match = true;
    snprintf(out->detail, sizeof out->detail, "match: %zu turn(s) identical", n);
}
