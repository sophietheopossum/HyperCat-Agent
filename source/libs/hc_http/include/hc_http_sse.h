#ifndef HC_HTTP_SSE_H
#define HC_HTTP_SSE_H

#include <stdbool.h>
#include <stddef.h>

/* hc_http_sse — incremental Server-Sent-Events parser over a raw byte stream.
 *
 * Purpose:   turn arbitrarily-chunked HTTP response bytes into discrete SSE events with NO
 *            network: push bytes in, get zero-or-more events out via a callback. Tolerates CRLF
 *            and LF, comment / keep-alive lines (':'-prefixed), multiple 'data:' lines per event
 *            (joined with '\n' per the WHATWG SSE spec), partial frames split across feeds, and
 *            the OpenAI '[DONE]' sentinel. A public utility of hc_http so the layer above
 *            (hc_llm) can decode a streamed completion; it is independently testable offline.
 * Owns:      a carry-over buffer (incomplete tail between feeds) and the in-progress event's
 *            accumulators, capped at 8 MiB each to bound a hostile stream.
 * Threading: single-owner, not thread-safe; one parser per stream.
 * Lifetime:  the event (and its strings) handed to the callback is valid ONLY for that call —
 *            the parser reuses its storage afterward. Copy to keep.
 */

typedef struct {
    const char *event;    /* 'event:' value, or NULL (default "message")          */
    const char *data;     /* concatenated 'data:' payload, NUL-terminated          */
    size_t      data_len; /* length of data                                       */
    const char *id;       /* 'id:' value, or NULL                                 */
    bool        is_done;  /* the data payload was exactly "[DONE]"                 */
} hc_http_sse_event;

/* Return false to abort; hc_http_sse_feed then stops and returns false. */
typedef bool (*hc_http_sse_emit_fn)(const hc_http_sse_event *ev, void *user);

typedef struct hc_http_sse hc_http_sse;

hc_http_sse *hc_http_sse_new(hc_http_sse_emit_fn emit, void *user); /* NULL on OOM */
void         hc_http_sse_free(hc_http_sse *);
void         hc_http_sse_reset(hc_http_sse *); /* drop carry-over + in-progress event */

/* Push n bytes; emits each completed event (terminated by a blank line) via the callback.
 * Returns false if the callback aborted or on allocation/ceiling failure (distinguish via _ok). */
bool hc_http_sse_feed(hc_http_sse *, const char *bytes, size_t n);

/* Flush a final event that lacked a trailing blank line (some servers close without one). */
bool hc_http_sse_finish(hc_http_sse *);

/* false iff a feed hit OOM or the 8 MiB ceiling (as opposed to a deliberate callback abort). */
bool hc_http_sse_ok(const hc_http_sse *);

#endif /* HC_HTTP_SSE_H */
