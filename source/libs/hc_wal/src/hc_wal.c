/* hc_wal — per-stream append-only write-ahead log. See hc_wal.h.
 *
 * The whole module is path + bytes: a stream is one <dir>/<id>.wal file; append writes one validated line
 * + '\n' via the shared crash-safe hc_fs_append; replay reads the capped file and splits on '\n', skipping
 * a torn tail; list scans the dir for *.wal; remove unlinks. No JSON, no domain knowledge, no held fd. */

#include "hc_wal.h"

#include "hc_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct hc_wal {
    char path[1024]; /* <dir>/<stream_id>.wal — built + validated at open */
};

/* A stream id becomes a path component, so reject anything that could escape `dir`: a '/' or a ".."
 * substring (the hc_store guard), an empty / over-long id, or a control byte in a filename. A legitimate
 * agenda id is a single safe token (e.g. "ag1", "release", "ag2:repair1"). */
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

/* Build "<dir>/<id>.wal" into `out`; -1 on a bad/unsafe id or a path that would not fit. */
static int wal_path(char *out, size_t cap, const char *dir, const char *id)
{
    if (!dir || !id_ok(id)) return -1;
    int k = snprintf(out, cap, "%s/%s.wal", dir, id);
    return (k > 0 && (size_t)k < cap) ? 0 : -1;
}

hc_wal *hc_wal_open(const char *dir, const char *stream_id)
{
    char path[1024];
    if (wal_path(path, sizeof path, dir, stream_id) != 0) return NULL;
    hc_wal *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    memcpy(w->path, path, strlen(path) + 1);
    return w;
}

int hc_wal_append(hc_wal *w, const char *line, size_t len)
{
    if (!w || !line || len > HC_WAL_MAX_LINE) return -1;
    if (memchr(line, '\n', len)) return -1; /* an embedded newline would split the record on replay */
    /* One append of line + '\n' so the record and its terminator land together: a crash before the fsync
     * leaves at worst a torn tail (a newline-less trailing fragment) that replay skips. */
    char *buf = malloc(len + 1);
    if (!buf) return -1;
    memcpy(buf, line, len);
    buf[len] = '\n';
    int r = hc_fs_append(w->path, buf, len + 1);
    free(buf);
    return r;
}

void hc_wal_close(hc_wal *w) { free(w); }

int hc_wal_replay(const char *dir, const char *stream_id, hc_wal_line_cb cb, void *user)
{
    char path[1024];
    if (wal_path(path, sizeof path, dir, stream_id) != 0) return -1;
    if (access(path, F_OK) != 0) return 0; /* absent stream = nothing to replay */
    size_t len = 0;
    char  *data = hc_fs_read_file(path, HC_WAL_MAX_FILE, &len); /* capped before alloc */
    if (!data) return -1;                                       /* unreadable or over the cap */
    /* Split on '\n'. The trailing bytes AFTER the last '\n' (if any) are a torn tail from a crash
     * mid-append — never handed to `cb`. Records are NUL-terminated in place for the callback. */
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n') continue;
        data[i] = '\0';
        if (cb && cb(data + start, i - start, user) != 0) break; /* caller asked to stop — not an error */
        start = i + 1;
    }
    free(data);
    return 0;
}

int hc_wal_list(const char *dir, hc_wal_id_cb cb, void *user)
{
    if (!dir) return -1;
    DIR *d = opendir(dir);
    if (!d) return (errno == ENOENT) ? 0 : -1; /* no dir yet = no streams */
    struct dirent *e;
    unsigned       count = 0;
    while ((e = readdir(d)) != NULL) {
        if (count >= HC_WAL_MAX_STREAMS) break;
        const char *name = e->d_name;
        size_t      n = strlen(name);
        if (n <= 4 || strcmp(name + (n - 4), ".wal") != 0) continue; /* only *.wal basenames */
        size_t idlen = n - 4;
        char   id[160];
        if (idlen >= sizeof id) continue;
        memcpy(id, name, idlen);
        id[idlen] = '\0';
        if (!id_ok(id)) continue; /* defense in depth — never hand back an unsafe id */
        count++;
        if (cb && cb(id, user) != 0) break;
    }
    closedir(d);
    return 0;
}

int hc_wal_remove(const char *dir, const char *stream_id)
{
    char path[1024];
    if (wal_path(path, sizeof path, dir, stream_id) != 0) return -1;
    if (unlink(path) != 0 && errno != ENOENT) return -1; /* absent = already gone (idempotent) */
    return 0;
}
