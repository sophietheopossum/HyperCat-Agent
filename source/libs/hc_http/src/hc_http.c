/* hc_http.c — libcurl wrapper. See hc_http.h for the contract.
 *
 * libcurl is confined to this translation unit (consumers never include curl.h, mirroring how
 * hc_json confines cJSON). One reusable easy handle per hc_http; curl_easy_reset before each
 * request restores option defaults while KEEPING the live-connection and DNS caches, so the
 * connection is reused without stale per-request options leaking across calls. The connection
 * guard is wired through BOTH CURLOPT_OPENSOCKETFUNCTION (refuse a denied peer BEFORE connect —
 * no SYN reaches a forbidden host on a NEW connection) and CURLOPT_PREREQFUNCTION (re-assert
 * before every transfer, including one that REUSES a pooled connection and so never re-opens a
 * socket) — together they make the guard a per-request control, the mechanism the step-3 policy
 * module drives.
 *
 * The one module-global, g_curl_refs, is a process-wide refcount for curl_global_init/cleanup;
 * it is not thread-safe and requires single-thread init before any worker spawns (see
 * hc_http_global_init) — the singleton exception the engineering policy permits.
 */

#include "hc_http.h"

#include <curl/curl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* curl_global_init is process-wide and not thread-safe; refcount it and require main-thread
 * init before workers spawn (see the header banner). */
static unsigned g_curl_refs = 0;

bool hc_http_global_init(void)
{
    if (g_curl_refs == 0 && curl_global_init(CURL_GLOBAL_DEFAULT) != 0) return false;
    g_curl_refs++;
    return true;
}

void hc_http_global_shutdown(void)
{
    if (g_curl_refs == 0) return;
    if (--g_curl_refs == 0) curl_global_cleanup();
}

struct hc_http {
    CURL  *easy;
    char   user_agent[128];
    long   timeout_ms;
    long   connect_ms;
    size_t max_bytes;
    bool   follow;
    long   max_redirs;
    bool   verify_peer;
    bool   verify_host;
    hc_http_guard_fn guard;
    void  *guard_user;
    char   cur_host[256]; /* host of the in-flight request, surfaced to the guard */
    bool   denied;        /* set by the opensocket guard when it refuses a peer    */
    struct curl_slist *resolve; /* P07.2: pinned host:port->ip (CURLOPT_RESOLVE), owned + freed in hc_http_free */
};

/* ---- growable byte buffer for the buffered body/header sinks ---- */
typedef struct {
    char  *buf;
    size_t len, cap, max;
    bool   truncated;
} buf_sink;

static bool sink_ensure(buf_sink *b, size_t extra)
{
    if (extra > SIZE_MAX - 1 - b->len) return false; /* length math would overflow */
    size_t need = b->len + extra + 1;
    if (need > b->cap) {
        size_t ncap = b->cap ? b->cap : 4096;
        while (ncap < need) {
            if (ncap > SIZE_MAX / 2) {
                ncap = need;
                break;
            }
            ncap *= 2;
        }
        char *nb = realloc(b->buf, ncap);
        if (!nb) return false;
        b->buf = nb;
        b->cap = ncap;
    }
    return true;
}

static size_t write_body(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    buf_sink *b = userdata;
    size_t n = size * nmemb;
    if (b->len + n > b->max) {              /* cap: keep what fits, mark truncated, abort */
        size_t room = b->max > b->len ? b->max - b->len : 0;
        if (room && sink_ensure(b, room)) {
            memcpy(b->buf + b->len, ptr, room);
            b->len += room;
        }
        b->truncated = true;
        return 0;
    }
    if (!sink_ensure(b, n)) return 0;
    memcpy(b->buf + b->len, ptr, n);
    b->len += n;
    return n;
}

static size_t write_header(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    buf_sink *b = userdata;
    size_t n = size * nmemb;
    if (b->len + n > b->max || !sink_ensure(b, n)) return n; /* over cap/OOM: skip, do not abort */
    memcpy(b->buf + b->len, ptr, n);
    b->len += n;
    return n;
}

typedef struct {
    hc_http_data_fn on_data;
    void  *user;
    size_t total;     /* bytes streamed so far                                    */
    size_t max;       /* cap; exceeding it aborts the stream (unbounded-stream DoS guard) */
    bool   cancelled; /* on_data refused, or the cap was exceeded                  */
} stream_sink;

static size_t write_stream(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    stream_sink *s = userdata;
    size_t n = size * nmemb;
    s->total += n;
    if (s->max && s->total > s->max) { /* bound a fast, never-ending stream */
        s->cancelled = true;
        return 0;
    }
    if (s->on_data && !s->on_data(ptr, n, s->user)) {
        s->cancelled = true;
        return 0;
    }
    return n;
}

/* CURLOPT_OPENSOCKETFUNCTION: consult the guard with the resolved peer, then create the socket
 * (or refuse). Runs before connect, and again for each redirect's re-resolution. */
static curl_socket_t open_socket_cb(void *clientp, curlsocktype purpose,
                                    struct curl_sockaddr *address)
{
    hc_http *h = clientp;
    (void)purpose;
    if (h->guard) {
        char ip[64] = {0};
        unsigned short port = 0;
        hc_http_addr_family fam = HC_HTTP_AF_INET;
        if (address->family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)&address->addr;
            inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof ip);
            port = ntohs(s4->sin_port);
            fam = HC_HTTP_AF_INET;
        } else if (address->family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&address->addr;
            inet_ntop(AF_INET6, &s6->sin6_addr, ip, sizeof ip);
            port = ntohs(s6->sin6_port);
            fam = HC_HTTP_AF_INET6;
        } else {
            h->denied = true; /* unknown address family: fail closed */
            return CURL_SOCKET_BAD;
        }
        hc_http_peer peer = {fam, ip, port, h->cur_host[0] ? h->cur_host : NULL};
        if (!h->guard(&peer, h->guard_user)) {
            h->denied = true;
            return CURL_SOCKET_BAD;
        }
    }
    return socket(address->family, address->socktype, address->protocol);
}

#if LIBCURL_VERSION_NUM >= 0x075000 /* 7.80.0: CURLOPT_PREREQFUNCTION */
/* Fires before each request is sent — INCLUDING a request that reuses a pooled connection (which
 * never re-opens a socket, so open_socket_cb would not run). This makes the guard a per-REQUEST
 * control, not merely per-connection, closing the connection-reuse bypass. */
static int prereq_cb(void *clientp, char *primary_ip, char *local_ip, int primary_port,
                     int local_port)
{
    hc_http *h = clientp;
    (void)local_ip;
    (void)local_port;
    if (h->guard) {
        hc_http_addr_family fam =
            (primary_ip && strchr(primary_ip, ':')) ? HC_HTTP_AF_INET6 : HC_HTTP_AF_INET;
        hc_http_peer peer = {fam, primary_ip ? primary_ip : "", (unsigned short)primary_port,
                             h->cur_host[0] ? h->cur_host : NULL};
        if (!h->guard(&peer, h->guard_user)) {
            h->denied = true;
            return CURL_PREREQFUNC_ABORT;
        }
    }
    return CURL_PREREQFUNC_OK;
}
#endif

hc_http *hc_http_new(void)
{
    hc_http *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->easy = curl_easy_init();
    if (!h->easy) {
        free(h);
        return NULL;
    }
    h->timeout_ms = 30000;
    h->connect_ms = 5000;
    h->max_bytes = 16u * 1024 * 1024;
    h->follow = true;
    h->max_redirs = 5;
    h->verify_peer = true;
    h->verify_host = true;
    snprintf(h->user_agent, sizeof h->user_agent, "hypercat/0.1");
    return h;
}

void hc_http_free(hc_http *h)
{
    if (!h) return;
    if (h->easy) curl_easy_cleanup(h->easy);
    if (h->resolve) curl_slist_free_all(h->resolve);
    free(h);
}

bool hc_http_pin_resolve(hc_http *h, const char *host, int port, const char *ip)
{
    if (!h || !host || !host[0] || !ip || !ip[0] || port <= 0) return false;
    /* CURLOPT_RESOLVE pins host:port -> ip so libcurl SKIPS DNS for it. Needed under kernel confinement (P07):
     * libcurl's THREADED resolver would spawn a thread (clone), which the worker's seccomp floor denies — so the
     * worker pre-resolves the LLM endpoint BEFORE applying confinement and pins it here. The egress guard still
     * vets this ip at connect (the pin is the resolved peer the SSRF classifier sees). */
    char entry[400];
    snprintf(entry, sizeof entry, "%s:%d:%s", host, port, ip);
    struct curl_slist *nl = curl_slist_append(NULL, entry);
    if (!nl) return false;
    if (h->resolve) curl_slist_free_all(h->resolve);
    h->resolve = nl;
    curl_easy_setopt(h->easy, CURLOPT_RESOLVE, h->resolve);
    return true;
}

bool hc_http_set_user_agent(hc_http *h, const char *ua)
{
    if (!h) return false;
    snprintf(h->user_agent, sizeof h->user_agent, "%s", ua ? ua : "hypercat/0.1");
    return true;
}

bool hc_http_set_timeouts_ms(hc_http *h, int total_ms, int connect_ms)
{
    if (!h) return false;
    if (total_ms > 0) h->timeout_ms = total_ms;
    if (connect_ms > 0) h->connect_ms = connect_ms;
    return true;
}

bool hc_http_set_max_bytes(hc_http *h, size_t max_bytes)
{
    if (!h) return false;
    if (max_bytes > 0) h->max_bytes = max_bytes;
    return true;
}

bool hc_http_set_follow_redirects(hc_http *h, bool follow, int max_redirs)
{
    if (!h) return false;
    h->follow = follow;
    h->max_redirs = max_redirs > 0 ? max_redirs : 0;
    return true;
}

void hc_http_set_guard(hc_http *h, hc_http_guard_fn guard, void *user)
{
    if (!h) return;
    h->guard = guard;
    h->guard_user = user;
}

bool hc_http_set_tls_verify(hc_http *h, bool verify_peer, bool verify_host)
{
    if (!h) return false;
    h->verify_peer = verify_peer;
    h->verify_host = verify_host;
    return true;
}

void hc_http_response_free(hc_http_response *r)
{
    if (!r) return;
    free(r->body);
    free(r->headers);
    memset(r, 0, sizeof *r);
}

/* Best-effort host extraction for the guard's peer.host (between "://" and the next delimiter). */
static void set_cur_host(hc_http *h, const char *url)
{
    h->cur_host[0] = '\0';
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != ':' && p[i] != '?' && p[i] != '#'
           && i < sizeof h->cur_host - 1) {
        h->cur_host[i] = p[i];
        i++;
    }
    h->cur_host[i] = '\0';
}

static struct curl_slist *build_headers(const char *const *headers)
{
    struct curl_slist *list = NULL;
    if (!headers) return NULL;
    for (size_t i = 0; headers[i]; i++) {
        struct curl_slist *nl = curl_slist_append(list, headers[i]);
        if (!nl) {
            curl_slist_free_all(list);
            return NULL;
        }
        list = nl;
    }
    return list;
}

static void apply_sticky(hc_http *h)
{
    CURL *c = h->easy;
    curl_easy_setopt(c, CURLOPT_USERAGENT, h->user_agent);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, h->connect_ms);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, h->follow ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, h->max_redirs);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, h->verify_peer ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, h->verify_host ? 2L : 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    /* restrict to HTTP(S), including across redirects — no file://, gopher://, etc. The *_STR
     * options replaced the bitmask ones in curl 7.85; use whichever the linked curl provides. */
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, open_socket_cb);
    curl_easy_setopt(c, CURLOPT_OPENSOCKETDATA, h);
#if LIBCURL_VERSION_NUM >= 0x075000
    curl_easy_setopt(c, CURLOPT_PREREQFUNCTION, prereq_cb);
    curl_easy_setopt(c, CURLOPT_PREREQDATA, h);
#endif
}

static hc_http_status map_result(hc_http *h, CURLcode rc, buf_sink *body, stream_sink *stream)
{
    if (rc == CURLE_OK) return HC_HTTP_OK;
    if (h->denied) return HC_HTTP_ERR_DENIED;
    if (stream && stream->cancelled) return HC_HTTP_ERR_CANCELLED;
    if (body && body->truncated) return HC_HTTP_OK; /* capped, but completed */
    switch (rc) {
    case CURLE_OPERATION_TIMEDOUT:
        return HC_HTTP_ERR_TIMEOUT;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
        return HC_HTTP_ERR_OFFLINE;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
        return HC_HTTP_ERR_TLS;
    default:
        return HC_HTTP_ERR_INTERNAL;
    }
}

static hc_http_status run_buffered(hc_http *h, const char *url, const char *const *headers,
                                   const void *body, size_t body_len, bool is_post,
                                   hc_http_response *out)
{
    if (!h || !url || !out) return HC_HTTP_ERR_INVALID;
    memset(out, 0, sizeof *out);
    h->denied = false;
    set_cur_host(h, url);

    CURL *c = h->easy;
    curl_easy_reset(c); /* keeps the live-connection + DNS caches; clears per-request options */
    apply_sticky(h);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, h->timeout_ms);
    if (is_post) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    }
    struct curl_slist *hl = build_headers(headers);
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);

    buf_sink bodysink = {0};
    bodysink.max = h->max_bytes;
    buf_sink hdrsink = {0};
    hdrsink.max = 256u * 1024;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &bodysink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &hdrsink);

    CURLcode rc = curl_easy_perform(c);
    if (hl) curl_slist_free_all(hl);

    hc_http_status st = map_result(h, rc, &bodysink, NULL);
    if (st != HC_HTTP_OK) {
        free(bodysink.buf);
        free(hdrsink.buf);
        return st;
    }
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    out->status = status;
    out->truncated = bodysink.truncated;
    if (bodysink.buf) {
        bodysink.buf[bodysink.len] = '\0'; /* sink_ensure reserved the +1 */
        out->body = bodysink.buf;
        out->body_len = bodysink.len;
    } else {
        out->body = calloc(1, 1); /* empty body -> "" */
    }
    if (hdrsink.buf) {
        hdrsink.buf[hdrsink.len] = '\0';
        out->headers = hdrsink.buf;
        out->headers_len = hdrsink.len;
    }
    return HC_HTTP_OK;
}

hc_http_status hc_http_get(hc_http *h, const char *url, const char *const *headers,
                           hc_http_response *out)
{
    return run_buffered(h, url, headers, NULL, 0, false, out);
}

hc_http_status hc_http_post(hc_http *h, const char *url, const char *const *headers,
                            const void *body, size_t body_len, hc_http_response *out)
{
    return run_buffered(h, url, headers, body, body_len, true, out);
}

hc_http_status hc_http_post_stream(hc_http *h, const char *url, const char *const *headers,
                                   const void *body, size_t body_len, hc_http_data_fn on_data,
                                   void *user, long *status_out)
{
    if (!h || !url || !on_data) return HC_HTTP_ERR_INVALID;
    h->denied = false;
    set_cur_host(h, url);

    CURL *c = h->easy;
    curl_easy_reset(c);
    apply_sticky(h);
    curl_easy_setopt(c, CURLOPT_URL, url);
    /* Streaming is long-lived but NOT unbounded: honor the configured total (default 30s; a worker
     * raises it to a generous per-call cap) so a hostile slow-drip turn that dribbles >=1 byte per
     * low-speed window cannot run forever. The low-speed limit stays the faster stall catch. */
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, h->timeout_ms);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    struct curl_slist *hl = build_headers(headers);
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);

    stream_sink sink = {on_data, user, 0, h->max_bytes, false};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_stream);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);

    CURLcode rc = curl_easy_perform(c);
    if (hl) curl_slist_free_all(hl);
    if (status_out) {
        long s = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &s);
        *status_out = s;
    }
    return map_result(h, rc, NULL, &sink);
}

const char *hc_http_status_str(hc_http_status s)
{
    switch (s) {
    case HC_HTTP_OK:            return "ok";
    case HC_HTTP_ERR_OFFLINE:   return "offline / could not connect";
    case HC_HTTP_ERR_TIMEOUT:   return "timed out";
    case HC_HTTP_ERR_CANCELLED: return "cancelled by callback";
    case HC_HTTP_ERR_DENIED:    return "connection denied by guard";
    case HC_HTTP_ERR_TLS:       return "TLS verification failed";
    case HC_HTTP_ERR_INVALID:   return "invalid argument";
    case HC_HTTP_ERR_INTERNAL:  return "internal error";
    }
    return "unknown";
}
