#ifndef HC_HTTP_H
#define HC_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_http — libcurl-backed HTTP client + connectivity probe (POSIX: Linux + macOS).
 *
 * Purpose:   provider-neutral, byte-level HTTP for HyperCat: one-shot GET/POST returning
 *            status + headers + body, a streaming POST that delivers raw bytes to a callback
 *            (the transport under SSE), and a lightweight reachability probe to fail fast
 *            before an LLM call. No message/LLM semantics (that is hc_llm), no IPC (that is
 *            hc_transport), no egress policy (that is the C++ policy module — this module only
 *            exposes a neutral connection-guard HOOK the policy plugs into).
 * Owns:      each hc_http handle owns one reusable libcurl easy handle and its config; an
 *            hc_http_response owns its heap body + header strings. libcurl is confined to the
 *            .c — consumers link hc_http but never include curl.h.
 * Threading: an hc_http handle is single-owner and NOT thread-safe; give each thread/agent its
 *            own (the multi-process model makes this natural — one handle per worker).
 *            hc_http_global_init/shutdown wrap curl_global_init/cleanup; they are process-wide,
 *            refcounted, and must be called from one thread before any other thread creates a
 *            handle (curl_global_init is not thread-safe).
 * Lifetime:  body/headers in an hc_http_response are valid until hc_http_response_free. Strings
 *            handed to the streaming/guard callbacks are owned by hc_http and valid ONLY for the
 *            duration of that callback — copy anything that must outlive it.
 */

/* Transport-level outcome. A COMPLETED request returns HC_HTTP_OK regardless of HTTP status —
 * inspect hc_http_response.status for the HTTP code (404, 429, ...). The error codes are for
 * transport failures only. */
typedef enum {
    HC_HTTP_OK = 0,
    HC_HTTP_ERR_OFFLINE,    /* DNS/connect failed — caller may fall back to cache  */
    HC_HTTP_ERR_TIMEOUT,
    HC_HTTP_ERR_CANCELLED,  /* a data callback returned false                      */
    HC_HTTP_ERR_DENIED,     /* the connection guard denied a resolved peer          */
    HC_HTTP_ERR_TLS,        /* certificate / TLS handshake failure (verify is ON)   */
    HC_HTTP_ERR_INVALID,    /* bad argument                                         */
    HC_HTTP_ERR_INTERNAL    /* libcurl / allocation failure                         */
} hc_http_status;

const char *hc_http_status_str(hc_http_status); /* static string; never freed */

/* ---- process lifecycle (refcounted curl_global_init/cleanup) ---- */
/* Call once per process before creating any handle; safe to nest (refcounted). NOT thread-safe
 * — call from the main thread before spawning workers/threads. */
bool hc_http_global_init(void);
void hc_http_global_shutdown(void);

/* ---- connection guard (anti-SSRF MECHANISM; the policy lives in the C++ policy module) ---- */
typedef enum { HC_HTTP_AF_INET = 1, HC_HTTP_AF_INET6 = 2 } hc_http_addr_family;

/* A resolved peer about to be connected. `ip` is presentation form ("203.0.113.7",
 * "2606:4700::1111") and is valid only for the guard call. `host` is the requested hostname
 * (pre-DNS) or NULL. */
typedef struct {
    hc_http_addr_family family;
    const char         *ip;
    unsigned short      port;
    const char         *host;
} hc_http_peer;

/* Return true to ALLOW, false to DENY (the transfer then resolves HC_HTTP_ERR_DENIED). The guard
 * is consulted before bytes are sent on EVERY request: before connect on a new connection (so a
 * denied peer never receives a SYN), again before a request that reuses a pooled connection, and
 * on every redirect hop's re-resolution (the DNS-rebind / SSRF defense point). It receives the
 * RESOLVED peer IP; a guard that cannot parse `ip` (empty string / unknown family) MUST treat the
 * peer as denied. Must be fast and non-blocking — it runs on the transfer thread inside libcurl.
 * Default (no guard set) == allow all. */
typedef bool (*hc_http_guard_fn)(const hc_http_peer *peer, void *user);

/* ---- handle lifecycle + sticky config ---- */
typedef struct hc_http hc_http;

hc_http *hc_http_new(void);       /* owns one reusable curl easy handle; NULL on failure */
void     hc_http_free(hc_http *); /* NULL-safe                                            */

bool hc_http_set_user_agent(hc_http *, const char *ua);                  /* NULL => default     */
bool hc_http_set_timeouts_ms(hc_http *, int total_ms, int connect_ms);   /* <=0 => keep default */
bool hc_http_set_max_bytes(hc_http *, size_t max_bytes);                 /* 0 => keep default   */
bool hc_http_set_follow_redirects(hc_http *, bool follow, int max_redirs);
void hc_http_set_guard(hc_http *, hc_http_guard_fn guard, void *user);   /* NULL clears         */
/* Relax TLS verification — for the loopback test / a localhost dev server ONLY. Default is ON. */
bool hc_http_set_tls_verify(hc_http *, bool verify_peer, bool verify_host);

/* P07.2: pin `host`:`port` to a pre-resolved `ip` (CURLOPT_RESOLVE) so libcurl SKIPS DNS for it. Needed under
 * kernel confinement: the threaded resolver would spawn a thread (clone), which the worker's seccomp floor
 * denies — so the worker pre-resolves the LLM endpoint BEFORE applying confinement and pins it. The egress
 * guard still vets `ip` at connect. Returns false on a bad arg / allocation failure. */
bool hc_http_pin_resolve(hc_http *, const char *host, int port, const char *ip);

/* ---- request / response ---- */
typedef struct {
    long   status;     /* HTTP status code, 0 if the request never completed           */
    char  *body;       /* malloc'd, NUL-terminated (body_len is authoritative)         */
    size_t body_len;   /* exact byte length of body (may contain embedded NULs)        */
    char  *headers;    /* malloc'd raw response header block, or NULL                  */
    size_t headers_len;
    bool   truncated;  /* body hit the max-bytes cap                                   */
} hc_http_response;

void hc_http_response_free(hc_http_response *); /* frees body+headers, zeroes struct; NULL-safe */

/* Synchronous GET/POST. `headers` is a NULL-terminated array of full header lines
 * ("Authorization: Bearer x") or NULL. `*out` is filled and owned by the caller (free with
 * hc_http_response_free) even on non-2xx HTTP statuses. POST does not inject Content-Type — the
 * caller sets it via `headers`. Header lines are passed to libcurl verbatim and are NOT
 * sanitized: they must be CRLF-free and caller-validated (guard against header injection at the
 * layer that builds a header from untrusted data). */
hc_http_status hc_http_get(hc_http *, const char *url, const char *const *headers,
                           hc_http_response *out);
hc_http_status hc_http_post(hc_http *, const char *url, const char *const *headers,
                            const void *body, size_t body_len, hc_http_response *out);

/* Streaming POST: response bytes are delivered to `on_data` in arrival-order chunks (not
 * buffered). `on_data` returns false to abort (=> HC_HTTP_ERR_CANCELLED). The HTTP status is
 * reported via `*status_out` (may be NULL). Callers wanting parsed SSE events feed each chunk
 * into an SSE parser from inside on_data. */
typedef bool (*hc_http_data_fn)(const char *chunk, size_t n, void *user);

hc_http_status hc_http_post_stream(hc_http *, const char *url, const char *const *headers,
                                   const void *body, size_t body_len,
                                   hc_http_data_fn on_data, void *user, long *status_out);

/* ---- net probe (per-provider configurable host; no hardcoded target) ---- */
typedef enum {
    HC_HTTP_NET_ONLINE = 0,
    HC_HTTP_NET_NO_IFACE, /* network unreachable / down            */
    HC_HTTP_NET_NO_DNS,   /* host did not resolve                  */
    HC_HTTP_NET_NO_ROUTE  /* resolved, but TCP connect failed      */
} hc_http_net_state;

const char *hc_http_net_state_str(hc_http_net_state); /* static string */

/* Layered TCP-connect reachability probe to host:port (e.g. a provider's base host:"443").
 * host/port REQUIRED — the caller passes the per-provider probe host; there is no hardcoded
 * default. timeout_ms<=0 => 2000. Independent of any hc_http handle and of libcurl (pure POSIX
 * sockets), so it is unit-testable against localhost. */
hc_http_net_state hc_http_net_probe(const char *host, const char *port, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* HC_HTTP_H */
