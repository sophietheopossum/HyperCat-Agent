/* hc_transport.c — framed UDS transport. See hc_transport.h for the contract.
 *
 * Framing is a u32 network-order length + payload. Reads/writes loop over partial I/O and retry
 * EINTR; a peer close surfaces as HC_TRANSPORT_ERR_CLOSED (EOF on read, EPIPE on write). The
 * receive side rejects a frame larger than max_frame before allocating, to bound a hostile peer.
 */

#define _GNU_SOURCE 1

#include "hc_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

struct hc_transport {
    int    fd;
    size_t max_frame;
};

struct hc_transport_listener {
    int  fd;
    char path[108]; /* sun_path is ~108 bytes; "" if none (abstract/unknown) */
};

/* macOS has no MSG_NOSIGNAL on send(); it suppresses SIGPIPE via the SO_NOSIGPIPE socket option
 * (set below) instead. Either way, a write to a peer-closed socket returns EPIPE rather than
 * killing the process with SIGPIPE — making the EPIPE->CLOSED contract real (a rogue worker
 * closing its end must not crash the host writer). */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void set_endpoint_opts(int fd)
{
    fcntl(fd, F_SETFD, FD_CLOEXEC); /* don't leak fds across fork+exec into spawned agent workers */
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
}

static hc_transport *make_endpoint(int fd)
{
    hc_transport *t = (hc_transport *)calloc(1, sizeof *t);
    if (!t) {
        close(fd);
        return NULL;
    }
    set_endpoint_opts(fd);
    t->fd = fd;
    t->max_frame = 16u * 1024 * 1024;
    return t;
}

/* write exactly len bytes; 0 ok, -1 IO, -2 peer closed (EPIPE) */
static int write_all(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE || errno == ECONNRESET) return -2;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* read exactly len bytes; 0 ok, -1 IO, -2 peer closed (EOF before full), -3 recv timeout */
static int read_all(int fd, void *data, size_t len)
{
    char *p = (char *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, p + off, len - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -3; /* SO_RCVTIMEO elapsed */
            if (errno == ECONNRESET) return -2;
            return -1;
        }
        if (r == 0) return -2; /* EOF */
        off += (size_t)r;
    }
    return 0;
}

static hc_transport_status map_rw(int rc)
{
    if (rc == 0) return HC_TRANSPORT_OK;
    if (rc == -2) return HC_TRANSPORT_ERR_CLOSED;
    if (rc == -3) return HC_TRANSPORT_ERR_TIMEOUT;
    return HC_TRANSPORT_ERR_IO;
}

hc_transport_status hc_transport_send(hc_transport *t, const void *data, size_t len)
{
    if (!t || (!data && len)) return HC_TRANSPORT_ERR_INVALID;
    if (len > t->max_frame || len > 0xffffffffu) return HC_TRANSPORT_ERR_TOO_LARGE;
    uint32_t nlen = htonl((uint32_t)len);
    int rc = write_all(t->fd, &nlen, sizeof nlen);
    if (rc != 0) return map_rw(rc);
    if (len) {
        rc = write_all(t->fd, data, len);
        if (rc != 0) return map_rw(rc);
    }
    return HC_TRANSPORT_OK;
}

hc_transport_status hc_transport_recv(hc_transport *t, void **out, size_t *out_len)
{
    if (!t || !out || !out_len) return HC_TRANSPORT_ERR_INVALID;
    *out = NULL;
    *out_len = 0;

    uint32_t nlen = 0;
    int rc = read_all(t->fd, &nlen, sizeof nlen);
    if (rc != 0) return map_rw(rc);
    uint32_t len = ntohl(nlen);
    if (len > t->max_frame) return HC_TRANSPORT_ERR_TOO_LARGE;

    void *buf = malloc(len ? len : 1);
    if (!buf) return HC_TRANSPORT_ERR_NOMEM;
    if (len) {
        rc = read_all(t->fd, buf, len);
        if (rc != 0) {
            free(buf);
            return map_rw(rc);
        }
    }
    *out = buf;
    *out_len = len;
    return HC_TRANSPORT_OK;
}

bool hc_transport_pair(hc_transport **a, hc_transport **b)
{
    if (!a || !b) return false;
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
    *a = make_endpoint(fds[0]);
    if (!*a) {
        close(fds[1]);
        return false;
    }
    *b = make_endpoint(fds[1]);
    if (!*b) {
        hc_transport_close(*a);
        *a = NULL;
        return false;
    }
    return true;
}

hc_transport_listener *hc_transport_listen(const char *path)
{
    if (!path || !path[0]) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) return NULL;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    unlink(path); /* clear a stale socket file */
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return NULL;
    }
    hc_transport_listener *l = (hc_transport_listener *)calloc(1, sizeof *l);
    if (!l) {
        close(fd);
        unlink(path);
        return NULL;
    }
    l->fd = fd;
    snprintf(l->path, sizeof l->path, "%s", path);
    return l;
}

hc_transport *hc_transport_accept(hc_transport_listener *l)
{
    if (!l) return NULL;
    int fd;
    do {
        fd = accept(l->fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return NULL;
    return make_endpoint(fd);
}

void hc_transport_listener_shutdown(hc_transport_listener *l)
{
    if (l && l->fd >= 0) shutdown(l->fd, SHUT_RDWR); /* wakes a thread blocked in accept() */
}

void hc_transport_listener_close(hc_transport_listener *l)
{
    if (!l) return;
    if (l->fd >= 0) close(l->fd);
    if (l->path[0]) unlink(l->path);
    free(l);
}

hc_transport *hc_transport_connect(const char *path)
{
    if (!path || !path[0]) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) return NULL;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return NULL;
    }
    return make_endpoint(fd);
}

void hc_transport_close(hc_transport *t)
{
    if (!t) return;
    if (t->fd >= 0) close(t->fd);
    free(t);
}

void hc_transport_shutdown(hc_transport *t)
{
    if (t && t->fd >= 0) shutdown(t->fd, SHUT_RDWR);
}

void hc_transport_set_max_frame(hc_transport *t, size_t max_bytes)
{
    if (t && max_bytes) t->max_frame = max_bytes;
}

void hc_transport_set_recv_timeout(hc_transport *t, int timeout_ms)
{
    if (!t) return;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
    setsockopt(t->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); /* 0,0 => block indefinitely */
}

bool hc_transport_alive(hc_transport *t)
{
    if (!t) return false;
    /* NON-CONSUMING liveness probe: a 1-byte MSG_PEEK|MSG_DONTWAIT. recv == 0 means the peer closed (EOF) -> dead;
     * -1 with EAGAIN/EWOULDBLOCK means connected-but-no-data -> alive; -1 with any other errno -> dead; >0 means
     * data is pending -> alive. It never removes a byte from the stream, so a concurrent framed recv is unaffected.
     * Used by a patient reply-wait to tell "the operator hasn't decided yet" (alive) from "the host went away" (dead)
     * without busy-spinning on a closed socket. */
    char    b;
    ssize_t n = recv(t->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0) return true;
    if (n == 0) return false; /* orderly shutdown / EOF */
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool hc_transport_peer_cred(hc_transport *t, long *uid_out, long *pid_out)
{
    if (!t) return false;
#if defined(__linux__)
    struct ucred uc;
    socklen_t l = sizeof uc;
    if (getsockopt(t->fd, SOL_SOCKET, SO_PEERCRED, &uc, &l) != 0) return false;
    if (uid_out) *uid_out = (long)uc.uid;
    if (pid_out) *pid_out = (long)uc.pid;
    return true;
#elif defined(__APPLE__)
    uid_t uid;
    gid_t gid;
    if (getpeereid(t->fd, &uid, &gid) != 0) return false;
    if (uid_out) *uid_out = (long)uid;
    if (pid_out) *pid_out = -1; /* macOS getpeereid does not provide pid */
    return true;
#else
    if (uid_out) *uid_out = -1;
    if (pid_out) *pid_out = -1;
    return false;
#endif
}

const char *hc_transport_status_str(hc_transport_status s)
{
    switch (s) {
    case HC_TRANSPORT_OK:            return "ok";
    case HC_TRANSPORT_ERR_INVALID:   return "invalid argument";
    case HC_TRANSPORT_ERR_IO:        return "i/o error";
    case HC_TRANSPORT_ERR_CLOSED:    return "peer closed";
    case HC_TRANSPORT_ERR_TOO_LARGE: return "frame too large";
    case HC_TRANSPORT_ERR_NOMEM:     return "out of memory";
    case HC_TRANSPORT_ERR_TIMEOUT:   return "recv timeout";
    }
    return "unknown";
}
