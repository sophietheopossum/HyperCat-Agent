/* hc_store.c — portable session-store logic. See hc_store.h for the contract.
 *
 * This file holds no OS calls: filesystem work goes through the shared hc_fs.h, and serialization
 * through hc_json. That keeps the store testable and the platform split (Docs/Plan_HyperCat/13)
 * confined to libs/hc_fs.
 *
 * Multi-process invariant: several worker processes may share ONE store root concurrently, so a
 * session id embeds the caller's pid (`sess-<time>-<pid>-<counter>`) — without it two workers
 * persisting in the same clock second (time() granularity) would generate the same id and clobber
 * each other. Files are read with a size cap (the root is same-uid-writable; an oversized/planted
 * file must not exhaust host memory), and hc_session_load rejects a traversal id.
 */

#define _DEFAULT_SOURCE 1

#include "hc_store.h"

#include "hc_fs.h"
#include "hc_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> /* getpid — session ids must be unique ACROSS worker processes, not just within one */

/* Read caps for files in the (multi-process, same-uid-writable) store — bound host memory against an
 * oversized or planted file. meta.json is tiny; a transcript holds a few 240 KiB-capped messages. */
#define HC_STORE_META_MAX (256u * 1024)
#define HC_STORE_TRANSCRIPT_MAX (8u * 1024 * 1024)

struct hc_store {
    char     root[1024];
    unsigned counter;
};

typedef struct {
    char *role;
    char *content;
    char  ts[32];
} hc_msg;

struct hc_session {
    hc_store *store;
    char      id[80];
    char      title[64];
    char      model[128];
    char      created[32];
    char      updated[32];
    char      dir[1100];
    char      transcript[1200];
    hc_msg   *msgs;
    size_t    n, cap;
};

/* ---- store ---- */

/* The session store's default-root policy ($HC_STORE_DIR > XDG_CONFIG_HOME > HOME). Store-specific policy,
 * not a generic filesystem primitive, so it lives here rather than in hc_fs. 0 on success, -1 on error.
 * NOTE: this fallback is only reached on hc_store_open(NULL) (direct API users / tests) — in the running
 * app BOTH the host (app/host_storage) and agentd pass an explicit root, so this resolver is bypassed.
 * It deliberately uses CONFIG-domain XDG (~/.config/hypercat/sessions), distinct from the host's unified
 * DATA-domain dir (HC_DATA_DIR > XDG_DATA_HOME > ~/.local/share/hypercat); the two are not the same tree. */
static int store_resolve_root(char *out, size_t cap)
{
    const char *env = getenv("HC_STORE_DIR");
    if (env && env[0]) return (size_t)snprintf(out, cap, "%s", env) < cap ? 0 : -1;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) return (size_t)snprintf(out, cap, "%s/hypercat/sessions", xdg) < cap ? 0 : -1;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;
    return (size_t)snprintf(out, cap, "%s/.config/hypercat/sessions", home) < cap ? 0 : -1;
}

hc_store *hc_store_open(const char *root_or_null)
{
    hc_store *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (root_or_null && root_or_null[0]) {
        if ((size_t)snprintf(s->root, sizeof s->root, "%s", root_or_null) >= sizeof s->root) {
            free(s);
            return NULL;
        }
    } else if (store_resolve_root(s->root, sizeof s->root) != 0) {
        free(s);
        return NULL;
    }
    if (hc_fs_mkdirs(s->root) != 0) {
        free(s);
        return NULL;
    }
    return s;
}

void hc_store_close(hc_store *s) { free(s); }

const char *hc_store_root(const hc_store *s) { return s ? s->root : NULL; }

/* ---- session helpers ---- */

static bool session_paths(hc_session *se)
{
    if ((size_t)snprintf(se->dir, sizeof se->dir, "%s/%s", se->store->root, se->id)
        >= sizeof se->dir)
        return false;
    if ((size_t)snprintf(se->transcript, sizeof se->transcript, "%s/transcript.jsonl", se->dir)
        >= sizeof se->transcript)
        return false;
    return true;
}

static bool msgs_push(hc_session *se, const char *role, const char *content, const char *ts)
{
    if (se->n == se->cap) {
        size_t ncap = se->cap ? se->cap * 2 : 8;
        hc_msg *nm = realloc(se->msgs, ncap * sizeof *nm);
        if (!nm) return false;
        se->msgs = nm;
        se->cap = ncap;
    }
    hc_msg *m = &se->msgs[se->n];
    m->role = strdup(role ? role : "");
    m->content = strdup(content ? content : "");
    snprintf(m->ts, sizeof m->ts, "%s", ts ? ts : "");
    if (!m->role || !m->content) {
        free(m->role);
        free(m->content);
        return false;
    }
    se->n++;
    return true;
}

static bool write_meta(hc_session *se)
{
    int turns = 0;
    for (size_t i = 0; i < se->n; i++)
        if (strcmp(se->msgs[i].role, "user") == 0) turns++;

    hc_json *o = hc_json_new_object();
    if (!o) return false;
    bool ok = hc_json_obj_set_str(o, "id", se->id)
              && hc_json_obj_set_str(o, "title", se->title)
              && hc_json_obj_set_str(o, "model", se->model)
              && hc_json_obj_set_str(o, "created", se->created)
              && hc_json_obj_set_str(o, "updated", se->updated)
              && hc_json_obj_set_int(o, "turns", turns);
    char *text = ok ? hc_json_print(o, true) : NULL;
    hc_json_free(o);
    if (!text) return false;

    char path[1200];
    bool pok = (size_t)snprintf(path, sizeof path, "%s/meta.json", se->dir) < sizeof path
               && hc_fs_atomic_write(path, text, strlen(text)) == 0;
    free(text);
    return pok;
}

hc_session *hc_session_new(hc_store *store, const char *title, const char *model)
{
    if (!store) return NULL;
    hc_session *se = calloc(1, sizeof *se);
    if (!se) return NULL;
    se->store = store;

    /* id avoids ':' so it is a valid directory name on every OS. It includes the PID so that
     * concurrent worker PROCESSES sharing one store root never collide — time() is per-second and the
     * counter is per-store-handle (per process), so without the pid two workers persisting in the same
     * second would generate the same id and clobber each other (the multi-process session store). */
    snprintf(se->id, sizeof se->id, "sess-%ld-%d-%04u", (long)time(NULL), (int)getpid(),
             store->counter++);
    snprintf(se->title, sizeof se->title, "%s", (title && title[0]) ? title : "Untitled");
    snprintf(se->model, sizeof se->model, "%s", model ? model : "");
    hc_fs_now_iso8601(se->created, sizeof se->created);
    snprintf(se->updated, sizeof se->updated, "%s", se->created);

    if (!session_paths(se) || hc_fs_mkdirs(se->dir) != 0 || !write_meta(se)) {
        free(se);
        return NULL;
    }
    return se;
}

bool hc_session_append(hc_session *se, const char *role, const char *content)
{
    if (!se || !role) return false;
    char ts[32];
    hc_fs_now_iso8601(ts, sizeof ts);

    /* Build "{ts,role,content}" — hc_json escapes content, so any embedded newline becomes
     * \n and the record stays a single physical line (JSONL invariant). */
    hc_json *o = hc_json_new_object();
    if (!o) return false;
    bool ok = hc_json_obj_set_str(o, "ts", ts) && hc_json_obj_set_str(o, "role", role)
              && hc_json_obj_set_str(o, "content", content ? content : "");
    char *line = ok ? hc_json_print(o, false) : NULL;
    hc_json_free(o);
    if (!line) return false;

    size_t llen = strlen(line);
    char *withnl = realloc(line, llen + 2);
    if (!withnl) {
        free(line);
        return false;
    }
    line = withnl;
    line[llen] = '\n';
    line[llen + 1] = '\0';

    /* persist (append-only, crash-safe) before mirroring in memory */
    int rc = hc_fs_append(se->transcript, line, llen + 1);
    free(line);
    if (rc != 0) return false;
    if (!msgs_push(se, role, content, ts)) return false;

    hc_fs_now_iso8601(se->updated, sizeof se->updated);
    write_meta(se); /* best-effort metadata refresh */
    return true;
}

hc_session *hc_session_load(hc_store *store, const char *id)
{
    if (!store || !id || !id[0]) return NULL;
    /* The id becomes a path component (root/<id>/...). Reject anything that could escape the store
     * root — a '/' or a ".." — since a caller may pass an id read from a (same-uid-writable) listing.
     * A legitimate id is always a single safe component ("sess-<time>-<pid>-<counter>"). */
    if (strchr(id, '/') || strstr(id, "..")) return NULL;
    hc_session *se = calloc(1, sizeof *se);
    if (!se) return NULL;
    se->store = store;
    snprintf(se->id, sizeof se->id, "%s", id);
    snprintf(se->title, sizeof se->title, "Untitled");
    if (!session_paths(se)) {
        free(se);
        return NULL;
    }

    /* meta (best-effort) */
    char metapath[1200];
    if ((size_t)snprintf(metapath, sizeof metapath, "%s/meta.json", se->dir) < sizeof metapath) {
        size_t mlen = 0;
        char *mtext = hc_fs_read_file(metapath, HC_STORE_META_MAX, &mlen);
        if (mtext) {
            hc_json *m = hc_json_parse(mtext, mlen);
            if (m) {
                snprintf(se->title, sizeof se->title, "%s",
                         hc_json_get_str(m, "title", "Untitled"));
                snprintf(se->model, sizeof se->model, "%s", hc_json_get_str(m, "model", ""));
                snprintf(se->created, sizeof se->created, "%s", hc_json_get_str(m, "created", ""));
                snprintf(se->updated, sizeof se->updated, "%s", hc_json_get_str(m, "updated", ""));
                hc_json_free(m);
            }
            free(mtext);
        }
    }

    /* transcript: one JSON object per physical line */
    size_t tlen = 0;
    char *t = hc_fs_read_file(se->transcript, HC_STORE_TRANSCRIPT_MAX, &tlen);
    if (t) {
        const char *p = t;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
            if (linelen > 0) {
                hc_json *o = hc_json_parse(p, linelen);
                if (o) {
                    msgs_push(se, hc_json_get_str(o, "role", ""),
                              hc_json_get_str(o, "content", ""), hc_json_get_str(o, "ts", ""));
                    hc_json_free(o);
                }
            }
            if (!nl) break;
            p = nl + 1;
        }
        free(t);
    }
    return se;
}

void hc_session_free(hc_session *se)
{
    if (!se) return;
    for (size_t i = 0; i < se->n; i++) {
        free(se->msgs[i].role);
        free(se->msgs[i].content);
    }
    free(se->msgs);
    free(se);
}

const char *hc_session_id(const hc_session *se) { return se ? se->id : NULL; }
const char *hc_session_dir(const hc_session *se) { return se ? se->dir : NULL; }
size_t hc_session_count(const hc_session *se) { return se ? se->n : 0; }

bool hc_session_message(const hc_session *se, size_t i, const char **role_out,
                        const char **content_out)
{
    if (!se || i >= se->n) return false;
    if (role_out) *role_out = se->msgs[i].role;
    if (content_out) *content_out = se->msgs[i].content;
    return true;
}

bool hc_session_save_meta(hc_session *se) { return se ? write_meta(se) : false; }

bool hc_session_set_title(hc_session *se, const char *title)
{
    if (!se) return false;
    snprintf(se->title, sizeof se->title, "%s", (title && title[0]) ? title : "Untitled"); /* bounded like _new */
    return write_meta(se);                                                                  /* atomic meta refresh */
}

/* ---- listing ---- */

int hc_store_list(hc_store *store, hc_session_info **out, size_t *n_out)
{
    *out = NULL;
    *n_out = 0;
    if (!store) return -1;

    char **dirs = NULL;
    size_t ndirs = 0;
    if (hc_fs_list_dirs(store->root, &dirs, &ndirs) != 0) return -1;

    hc_session_info *info = ndirs ? calloc(ndirs, sizeof *info) : NULL;
    if (ndirs && !info) {
        hc_fs_free_list(dirs, ndirs);
        return -1;
    }

    size_t k = 0;
    for (size_t i = 0; i < ndirs; i++) {
        char metapath[1300];
        if ((size_t)snprintf(metapath, sizeof metapath, "%s/%s/meta.json", store->root, dirs[i])
            >= sizeof metapath)
            continue;
        size_t mlen = 0;
        char *mtext = hc_fs_read_file(metapath, HC_STORE_META_MAX, &mlen);
        if (!mtext) continue;
        hc_json *m = hc_json_parse(mtext, mlen);
        free(mtext);
        if (!m) continue;

        hc_session_info *e = &info[k++];
        /* The operative id is the DIRECTORY NAME — that is what hc_session_load / rename / delete resolve against
         * (root/<id>/). The in-file "id" is advisory: trusting it would let a same-uid attacker who plants a
         * meta.json decouple the displayed row from the dir actually acted on (a confused-deputy on delete). */
        snprintf(e->id, sizeof e->id, "%s", dirs[i]);
        snprintf(e->title, sizeof e->title, "%s", hc_json_get_str(m, "title", "Untitled"));
        snprintf(e->model, sizeof e->model, "%s", hc_json_get_str(m, "model", ""));
        snprintf(e->created, sizeof e->created, "%s", hc_json_get_str(m, "created", ""));
        snprintf(e->updated, sizeof e->updated, "%s", hc_json_get_str(m, "updated", ""));
        e->turns = (int)hc_json_get_int(m, "turns", 0);
        hc_json_free(m);
    }
    hc_fs_free_list(dirs, ndirs);
    *out = info;
    *n_out = k;
    return 0;
}

void hc_store_list_free(hc_session_info *info, size_t n)
{
    (void)n;
    free(info);
}
