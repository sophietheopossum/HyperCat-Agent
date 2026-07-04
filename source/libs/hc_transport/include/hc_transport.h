#ifndef HC_TRANSPORT_H
#define HC_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_transport — framed byte transport over Unix-domain sockets (POSIX: Linux + macOS).
 *
 * Purpose:   move length-prefixed messages between the host and an agent worker on the same
 *            machine. One frame = u32 length (network byte order) + that many payload bytes. The
 *            payload is opaque here — message semantics (addressing, req/reply, pub/sub) live in
 *            the C++ `bus` on top. A socketpair variant gives two connected endpoints with no
 *            filesystem, for host-internal use and tests.
 * Owns:      a connected endpoint owns its fd; a listener owns its fd + bound path (unlinked on
 *            close). A message buffer returned by hc_transport_recv is malloc'd; the caller frees.
 * Threading: an endpoint is single-owner, not thread-safe; give each thread/direction its own, or
 *            serialize. send and recv on the SAME endpoint from two threads is a data race.
 * Lifetime:  the recv buffer is malloc'd — the caller frees it with free(). An accepted endpoint
 *            outlives its listener (its fd is independent after accept). hc_transport_close frees
 *            an endpoint; hc_transport_listener_close unlinks the bound path and frees the listener.
 * Security:  same-host only. This module sets fds close-on-exec and suppresses SIGPIPE on writes.
 *            The CALLER that serves UNTRUSTED peers (the bus/supervisor) MUST, per accepted
 *            endpoint: (1) call hc_transport_peer_cred and drop a uid mismatch BEFORE reading any
 *            frame (impersonation defense); (2) read with a timeout — recv blocks unboundedly on a
 *            partial frame (slow-loris); (3) keep the listener path in a host-private directory
 *            (mode 0700, host uid) to avoid local UDS races. No cross-machine TLS in v1 (locked).
 */

typedef enum {
    HC_TRANSPORT_OK = 0,
    HC_TRANSPORT_ERR_INVALID,
    HC_TRANSPORT_ERR_IO,
    HC_TRANSPORT_ERR_CLOSED,    /* peer closed the connection (EOF / EPIPE)        */
    HC_TRANSPORT_ERR_TOO_LARGE, /* frame exceeds the configured maximum            */
    HC_TRANSPORT_ERR_NOMEM,
    HC_TRANSPORT_ERR_TIMEOUT    /* recv deadline elapsed (SO_RCVTIMEO); see set_recv_timeout */
} hc_transport_status;

const char *hc_transport_status_str(hc_transport_status);

typedef struct hc_transport          hc_transport;          /* a connected endpoint */
typedef struct hc_transport_listener hc_transport_listener; /* a UDS listener       */

/* Bind+listen on a Unix-domain socket `path` (a stale path is unlinked first). NULL on failure. */
hc_transport_listener *hc_transport_listen(const char *path);
hc_transport          *hc_transport_accept(hc_transport_listener *); /* blocks; NULL on failure */
void                   hc_transport_listener_close(hc_transport_listener *);

/* Unblock a thread blocked in hc_transport_accept on this listener (shutdown the socket) WITHOUT
 * freeing it — closing the fd alone does NOT wake a blocked accept(). Sequence: shutdown, join the
 * accept thread, then hc_transport_listener_close. */
void hc_transport_listener_shutdown(hc_transport_listener *);

hc_transport *hc_transport_connect(const char *path); /* connect to a UDS path; NULL on failure */

/* Two connected in-process endpoints (socketpair) — no filesystem. false on failure. */
bool hc_transport_pair(hc_transport **a, hc_transport **b);

void hc_transport_close(hc_transport *);

/* Unblock a thread blocked in recv/send on this endpoint by shutting down both directions,
 * WITHOUT freeing it — for clean teardown: shutdown, join the I/O thread, then hc_transport_close. */
void hc_transport_shutdown(hc_transport *);

/* Cap on a single frame (default 16 MiB; bounds a hostile peer). 0 keeps the current value. */
void hc_transport_set_max_frame(hc_transport *, size_t max_bytes);

/* Deadline for a blocking recv (SO_RCVTIMEO). timeout_ms == 0 restores indefinite blocking. When
 * the deadline elapses recv returns HC_TRANSPORT_ERR_TIMEOUT. NOTE: a timeout that lands MID-frame
 * (length read, payload pending) leaves the stream desynced — treat TIMEOUT as fatal to the
 * connection UNLESS you know recv was waiting at a frame boundary (a bounded reply-wait where the
 * peer either answers atomically or is silent). Intended for a request/reply deadline, not as a
 * slow-loris guard on an idle long-lived reader (that needs a first-byte-then-deadline policy).
 * Failure of the underlying setsockopt (e.g. on an already-shut-down socket) is silent — a
 * subsequent recv simply returns CLOSED/IO instead, which the caller already handles. */
void hc_transport_set_recv_timeout(hc_transport *, int timeout_ms);

/* NON-CONSUMING liveness probe (1-byte MSG_PEEK): true if the connection is still up (data pending or empty),
 * false if the peer has closed / errored. Lets a patient bounded reply-wait distinguish "still waiting" from "the
 * peer went away" without busy-spinning on a dead socket. Does not remove data from the stream. */
bool hc_transport_alive(hc_transport *);

/* Send one framed message (length prefix + payload). */
hc_transport_status hc_transport_send(hc_transport *, const void *data, size_t len);

/* Receive one framed message. On OK, *out is a malloc'd buffer of *out_len bytes (caller frees);
 * *out_len may be 0 (then *out is a 1-byte allocation). Blocks until a full frame or close —
 * INCLUDING unboundedly on a partial frame from a stalled peer, so a caller facing untrusted
 * peers must impose a read timeout (non-blocking + poll, or SO_RCVTIMEO). */
hc_transport_status hc_transport_recv(hc_transport *, void **out, size_t *out_len);

/* OS peer credentials of the connected peer. *pid_out is set to -1 where the OS does not provide
 * it (macOS). Returns false if credentials are unavailable. */
bool hc_transport_peer_cred(hc_transport *, long *uid_out, long *pid_out);

#ifdef __cplusplus
}
#endif
#endif /* HC_TRANSPORT_H */
