/* hc_projects — see hc_projects.h. The project index: an in-memory last-wins map replayed from an append-only
 * index.jsonl, cross-checked against the on-disk project dirs (orphan adoption), with an atomic active pointer.
 * Mirrors hc_wal's discipline (opaque handle, id_ok traversal rule, torn-tail-tolerant replay). C, no threads. */

#include "hc_projects.h"

#include "hc_fs.h"
#include "hc_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    HC_PROJECTS_MAX = 4096,         /* bound the index (a hostile/looping file)        */
    HC_PROJECTS_MAX_LINE = 8192,    /* one index record (display is bounded well under) */
    HC_PROJECTS_MAX_INDEX = 32u * 1024u * 1024u /* total index read cap (pre-alloc)     */
};

struct hc_projects {
    char        root[1024];     /* the host data dir given to open                 */
    char        pdir[1100];     /* <root>/projects                                 */
    char        index[1160];    /* <root>/projects/index.jsonl                     */
    char        active[1100];   /* <root>/active_project                           */
    hc_project *ent;            /* the replayed entries (last-wins per id)         */
    int        *dead;           /* parallel tombstone flags (1 == deleted)         */
    size_t      n, cap;
};

/* True iff `path` is a REAL directory we own (lstat, not stat): not a symlink, and owned by the current uid.
 * The isolation boundary is symlink-sensitive — a project subtree (or the projects/ root) that is a symlink a
 * same-uid peer planted would escape the jail — so every dir hc_projects creates/adopts/roots-at is re-checked
 * here as defense-in-depth, NOT trusting hc_fs_list_dirs alone. (Mirrors host_storage::dir_is_host_private,
 * reimplemented in C since that lives in the C++ host layer.) */
static int is_real_owned_dir(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    /* The SAME bar as host_storage::dir_is_host_private — not a symlink, a real dir, owned by us, AND 0700 (no
     * group/other bits). Kept aligned so the C primitive's "host-private" and the C++ host's never drift (a
     * loosened same-uid assumption on a shared host would otherwise let a 0755 project dir leak cross-account). */
    return !S_ISLNK(st.st_mode) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
           (st.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

/* --- the traversal-safe id rule, identical to hc_wal.c:25 (reject empty/>128/'/'/'..'/control bytes). It is
 * DUPLICATED rather than hoisted into hc_fs because it is a traversal-validation POLICY (what makes a valid id),
 * not a filesystem primitive — hc_fs's banner deliberately defers traversal-validation to its callers, and the
 * id shapes the two libs validate are different domain objects that could legitimately diverge. --- */
static int id_ok(const char *id)
{
    if (!id || !id[0]) return 0;
    size_t n = strlen(id);
    if (n > 128) return 0;
    if (strchr(id, '/') || strstr(id, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

/* Slugify a display name -> a traversal-safe id (lowercase ASCII alnum; other runs -> one '-'; trim edges). */
int hc_projects_slug(const char *display, char *id_out, size_t cap)
{
    if (!display || !id_out || cap == 0) return -1;
    size_t j = 0, limit = cap - 1 < 128 ? cap - 1 : 128;
    int    prev_dash = 1; /* start "after a dash" so a leading run is suppressed */
    for (size_t i = 0; display[i] && j < limit; i++) {
        unsigned char c = (unsigned char)display[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            id_out[j++] = (char)c;
            prev_dash = 0;
        } else if (c >= 'A' && c <= 'Z') {
            id_out[j++] = (char)(c - 'A' + 'a');
            prev_dash = 0;
        } else if (!prev_dash) {
            id_out[j++] = '-';
            prev_dash = 1;
        }
    }
    while (j > 0 && id_out[j - 1] == '-') j--; /* trim a trailing dash */
    id_out[j] = '\0';
    if (j == 0 || !id_ok(id_out)) return -1;
    return 0;
}

static hc_project *find_entry(hc_projects *p, const char *id, int *dead_out)
{
    for (size_t i = 0; i < p->n; i++)
        if (strcmp(p->ent[i].id, id) == 0) {
            if (dead_out) *dead_out = p->dead[i];
            return &p->ent[i];
        }
    return NULL;
}

/* Insert-or-overwrite an entry (last-wins). Returns 0 / -1 (OOM or the index is full). */
static int upsert(hc_projects *p, const hc_project *e, int dead)
{
    for (size_t i = 0; i < p->n; i++)
        if (strcmp(p->ent[i].id, e->id) == 0) {
            p->ent[i] = *e;
            p->dead[i] = dead;
            return 0;
        }
    if (p->n >= HC_PROJECTS_MAX) return -1;
    if (p->n == p->cap) {
        size_t      nc = p->cap ? p->cap * 2 : 16;
        hc_project *ne = (hc_project *)realloc(p->ent, nc * sizeof *ne);
        int        *nd = (int *)realloc(p->dead, nc * sizeof *nd);
        if (ne) p->ent = ne;
        if (nd) p->dead = nd;
        if (!ne || !nd) return -1;
        p->cap = nc;
    }
    p->ent[p->n] = *e;
    p->dead[p->n] = dead;
    p->n++;
    return 0;
}

/* Serialize one entry to a JSON line and fsync-append it to the index. Returns 0 / -1. */
static int append_line(hc_projects *p, const hc_project *e, int dead)
{
    hc_json *o = hc_json_new_object();
    if (!o) return -1;
    hc_json_obj_set_str(o, "id", e->id);
    hc_json_obj_set_str(o, "display", e->display);
    hc_json_obj_set_int(o, "created", e->created_ms);
    hc_json_obj_set_int(o, "touched", e->touched_ms);
    hc_json_obj_set_bool(o, "deleted", dead ? true : false);
    char *txt = hc_json_print_canonical(o);
    hc_json_free(o);
    if (!txt) return -1;
    size_t len = strlen(txt);
    int    rc = -1;
    if (len + 1 <= HC_PROJECTS_MAX_LINE && !memchr(txt, '\n', len)) {
        char *buf = (char *)malloc(len + 1);
        if (buf) {
            memcpy(buf, txt, len);
            buf[len] = '\n';
            rc = hc_fs_append(p->index, buf, len + 1);
            free(buf);
        }
    }
    free(txt);
    return rc;
}

/* Bounded copy into a fixed buffer (always NUL-terminates). */
static void copy_bounded(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int replay_cb_line(hc_projects *p, const char *line, size_t len)
{
    hc_json *o = hc_json_parse(line, len);
    if (!o) return 0; /* a torn/garbage line is skipped, never fatal */
    const char *id = hc_json_get_str(o, "id", "");
    if (id_ok(id)) {
        hc_project e;
        memset(&e, 0, sizeof e);
        copy_bounded(e.id, sizeof e.id, id);
        copy_bounded(e.display, sizeof e.display, hc_json_get_str(o, "display", id));
        e.created_ms = hc_json_get_int(o, "created", 0);
        e.touched_ms = hc_json_get_int(o, "touched", 0);
        upsert(p, &e, hc_json_get_bool(o, "deleted", false) ? 1 : 0);
    }
    hc_json_free(o);
    return 0;
}

/* Replay index.jsonl into the in-memory map (last-wins; torn final line ignored). */
static void replay_index(hc_projects *p)
{
    size_t len = 0;
    char  *data = hc_fs_read_file(p->index, HC_PROJECTS_MAX_INDEX, &len);
    if (!data) return; /* absent/oversized -> empty index */
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n') continue;
        if (i > start) replay_cb_line(p, data + start, i - start);
        start = i + 1;
    }
    free(data); /* bytes after the last '\n' are a torn tail — deliberately skipped */
}

/* Adopt any <root>/projects/<id>/ dir that the index doesn't already know (crash between mkdir + append).
 * A dir whose id IS in the index (live OR tombstoned) is left to the index — a deleted project is never
 * revived by a lingering dir. */
static void adopt_orphans(hc_projects *p, int64_t now_ms)
{
    char **dirs = NULL;
    size_t nd = 0;
    if (hc_fs_list_dirs(p->pdir, &dirs, &nd) != 0) return;
    for (size_t i = 0; i < nd; i++) {
        if (!id_ok(dirs[i]) || find_entry(p, dirs[i], NULL)) continue;
        char cand[1200]; /* defense-in-depth: re-check it's a real owned dir (hc_fs_list_dirs already skips symlinks) */
        if ((size_t)snprintf(cand, sizeof cand, "%s/%s", p->pdir, dirs[i]) >= sizeof cand ||
            !is_real_owned_dir(cand))
            continue;
        hc_project e;
        memset(&e, 0, sizeof e);
        copy_bounded(e.id, sizeof e.id, dirs[i]);
        copy_bounded(e.display, sizeof e.display, dirs[i]);
        e.created_ms = e.touched_ms = now_ms;
        if (upsert(p, &e, 0) == 0) append_line(p, &e, 0); /* persist the adoption */
    }
    hc_fs_free_list(dirs, nd);
}

hc_projects *hc_projects_open(const char *data_dir)
{
    if (!data_dir || !*data_dir || strlen(data_dir) >= sizeof(((hc_projects *)0)->root)) return NULL;
    hc_projects *p = (hc_projects *)calloc(1, sizeof *p);
    if (!p) return NULL;
    copy_bounded(p->root, sizeof p->root, data_dir);
    if ((size_t)snprintf(p->pdir, sizeof p->pdir, "%s/projects", p->root) >= sizeof p->pdir ||
        (size_t)snprintf(p->index, sizeof p->index, "%s/index.jsonl", p->pdir) >= sizeof p->index ||
        (size_t)snprintf(p->active, sizeof p->active, "%s/active_project", p->root) >= sizeof p->active) {
        free(p);
        return NULL;
    }
    if (hc_fs_mkdirs(p->pdir) != 0 || !is_real_owned_dir(p->pdir)) { /* 0700 -p; reject a planted symlink root */
        free(p);
        return NULL;
    }
    replay_index(p);
    adopt_orphans(p, 0); /* adopted orphans carry created/touched=0 (unknown) — they sort last */
    return p;
}

void hc_projects_close(hc_projects *p)
{
    if (!p) return;
    free(p->ent);
    free(p->dead);
    free(p);
}

int hc_projects_dir(hc_projects *p, const char *id, char *path_out, size_t cap)
{
    if (!p || !id_ok(id) || !path_out) return -1;
    if ((size_t)snprintf(path_out, cap, "%s/%s", p->pdir, id) >= cap) return -1;
    return 0;
}

int hc_projects_create(hc_projects *p, const char *display, int64_t now_ms, hc_project *out)
{
    if (!p) return -1;
    char base[HC_PROJECT_ID_CAP];
    if (hc_projects_slug(display ? display : "", base, sizeof base) != 0)
        copy_bounded(base, sizeof base, "project"); /* a name with no usable chars -> a safe default */

    /* dedup vs ALL entries (incl. tombstoned, so a stale dir is never reused) */
    char id[HC_PROJECT_ID_CAP];
    copy_bounded(id, sizeof id, base);
    for (int k = 2; find_entry(p, id, NULL) != NULL; k++) {
        if (k > 9999 || (size_t)snprintf(id, sizeof id, "%.118s-%d", base, k) >= sizeof id) return -1;
    }
    if (!id_ok(id)) return -1;

    char dir[1200];
    if (hc_projects_dir(p, id, dir, sizeof dir) != 0 || hc_fs_mkdirs(dir) != 0) return -1;
    if (!is_real_owned_dir(dir)) return -1; /* refuse a pre-planted symlink at projects/<id> (mkdirs tolerates EEXIST) */

    hc_project e;
    memset(&e, 0, sizeof e);
    copy_bounded(e.id, sizeof e.id, id);
    copy_bounded(e.display, sizeof e.display, (display && *display) ? display : id);
    e.created_ms = e.touched_ms = now_ms;
    if (append_line(p, &e, 0) != 0) return -1; /* dir stays; a re-open adopts it as an orphan */
    if (upsert(p, &e, 0) != 0) return -1;
    if (out) *out = e;
    return 0;
}

int hc_projects_get(hc_projects *p, const char *id, hc_project *out)
{
    if (!p || !id) return -1;
    int         dead = 0;
    hc_project *e = find_entry(p, id, &dead);
    if (!e || dead) return -1;
    if (out) *out = *e;
    return 0;
}

int hc_projects_count(hc_projects *p)
{
    if (!p) return -1;
    int c = 0;
    for (size_t i = 0; i < p->n; i++)
        if (!p->dead[i]) c++;
    return c;
}

static int cmp_touched_desc(const void *a, const void *b)
{
    int64_t ta = ((const hc_project *)a)->touched_ms, tb = ((const hc_project *)b)->touched_ms;
    if (ta < tb) return 1;
    if (ta > tb) return -1;
    return strcmp(((const hc_project *)a)->id, ((const hc_project *)b)->id); /* stable tiebreak */
}

int hc_projects_list(hc_projects *p, hc_project **out)
{
    if (!p || !out) return -1;
    *out = NULL;
    int c = hc_projects_count(p);
    if (c <= 0) return c; /* 0 -> NULL out; -1 -> error */
    hc_project *arr = (hc_project *)malloc((size_t)c * sizeof *arr);
    if (!arr) return -1;
    size_t j = 0;
    for (size_t i = 0; i < p->n; i++)
        if (!p->dead[i]) arr[j++] = p->ent[i];
    qsort(arr, j, sizeof *arr, cmp_touched_desc);
    *out = arr;
    return (int)j;
}

void hc_projects_free_list(hc_project *list) { free(list); }

int hc_projects_rename(hc_projects *p, const char *id, const char *display)
{
    if (!p || !id || !display) return -1;
    if (strlen(display) >= HC_PROJECT_NAME_CAP) return -1;
    int         dead = 0;
    hc_project *e = find_entry(p, id, &dead);
    if (!e || dead) return -1;
    hc_project up = *e;
    copy_bounded(up.display, sizeof up.display, display);
    if (append_line(p, &up, 0) != 0) return -1;
    return upsert(p, &up, 0);
}

int hc_projects_touch(hc_projects *p, const char *id, int64_t now_ms)
{
    if (!p || !id) return -1;
    int         dead = 0;
    hc_project *e = find_entry(p, id, &dead);
    if (!e || dead) return -1;
    hc_project up = *e;
    up.touched_ms = now_ms;
    if (append_line(p, &up, 0) != 0) return -1;
    return upsert(p, &up, 0);
}

int hc_projects_delete(hc_projects *p, const char *id)
{
    if (!p || !id) return -1;
    int         dead = 0;
    hc_project *e = find_entry(p, id, &dead);
    if (!e || dead) return -1;
    char active[HC_PROJECT_ID_CAP];
    if (hc_projects_get_active(p, active, sizeof active) == 0 && strcmp(active, id) == 0)
        return -1; /* refuse to delete the active project (the host switches away first) */
    hc_project up = *e;
    if (append_line(p, &up, 1) != 0) return -1; /* tombstone */
    return upsert(p, &up, 1);
}

int hc_projects_get_active(hc_projects *p, char *id_out, size_t cap)
{
    if (!p || !id_out || cap == 0) return -1;
    size_t len = 0;
    char  *data = hc_fs_read_file(p->active, HC_PROJECT_ID_CAP + 8, &len);
    if (!data) return -1;
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r' || data[len - 1] == ' ')) data[--len] = '\0';
    int ok = (len > 0 && id_ok(data) && (size_t)len < cap);
    if (ok) memcpy(id_out, data, len + 1);
    free(data);
    if (!ok) return -1;
    int dead = 0; /* a pointer to a tombstoned/unknown project is treated as unset */
    return (find_entry(p, id_out, &dead) && !dead) ? 0 : -1;
}

int hc_projects_set_active(hc_projects *p, const char *id, int64_t now_ms)
{
    if (!p || !id_ok(id)) return -1;
    int         dead = 0;
    hc_project *e = find_entry(p, id, &dead);
    if (!e || dead) return -1; /* only a live project can be made active */
    if (hc_fs_atomic_write(p->active, id, strlen(id)) != 0) return -1;
    return hc_projects_touch(p, id, now_ms);
}
