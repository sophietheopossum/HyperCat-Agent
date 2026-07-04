/* hc_http_sse.c — incremental SSE parser. See hc_http_sse.h for the contract.
 *
 * A line-oriented state machine: bytes accumulate in `carry`; complete lines (terminated by
 * '\n', tolerating a trailing '\r') are processed; a blank line dispatches the in-progress
 * event. Keeping the framing line-based (rather than scanning for "\n\n") is what makes CRLF
 * streams correct. No libcurl, no sockets — exercised directly by byte fixtures in the tests.
 */

#include "hc_http_sse.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Hard ceiling on a single carry/data buffer — bounds a hostile stream that never sends a frame
 * terminator, or piles up enormous data: lines (DoS), independent of any transport-level cap. */
#define HC_SSE_MAX_BUF (8u * 1024 * 1024)

struct hc_http_sse {
    hc_http_sse_emit_fn emit;
    void  *user;
    char  *carry;
    size_t carry_len, carry_cap;
    char  *data; /* accumulated 'data:' payload (joined with '\n') */
    size_t data_len, data_cap;
    char   event[256];
    char   id[256];
    bool   have_data;
    bool   oom;
};

static bool buf_append(char **buf, size_t *len, size_t *cap, const char *src, size_t n)
{
    if (n > SIZE_MAX - 1 - *len) return false; /* length math would overflow */
    size_t need = *len + n + 1;
    if (need > HC_SSE_MAX_BUF) return false;   /* refuse to grow past the ceiling */
    if (need > *cap) {
        size_t ncap = *cap ? *cap : 256;
        while (ncap < need) {
            if (ncap > HC_SSE_MAX_BUF) {
                ncap = need;
                break;
            }
            ncap *= 2;
        }
        char *nb = realloc(*buf, ncap);
        if (!nb) return false;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static void reset_event(hc_http_sse *s)
{
    s->data_len = 0;
    if (s->data) s->data[0] = '\0';
    s->event[0] = '\0';
    s->id[0] = '\0';
    s->have_data = false;
}

static bool dispatch(hc_http_sse *s)
{
    size_t dl = s->data_len;
    if (dl > 0 && s->data[dl - 1] == '\n') dl--; /* strip the single trailing join '\n' */
    if (s->data) s->data[dl] = '\0';

    hc_http_sse_event ev;
    ev.event = s->event[0] ? s->event : NULL;
    ev.data = s->data ? s->data : "";
    ev.data_len = dl;
    ev.id = s->id[0] ? s->id : NULL;
    ev.is_done = (dl == 6 && memcmp(ev.data, "[DONE]", 6) == 0);

    bool cont = s->emit ? s->emit(&ev, s->user) : true;
    reset_event(s);
    return cont;
}

static bool process_line(hc_http_sse *s, const char *line, size_t len)
{
    if (len == 0) {                          /* blank line dispatches if we accumulated data */
        if (s->have_data) return dispatch(s);
        reset_event(s);
        return true;
    }
    if (line[0] == ':') return true;         /* comment / keep-alive */

    const char *colon = memchr(line, ':', len);
    const char *val;
    size_t flen, vlen;
    if (colon) {
        flen = (size_t)(colon - line);
        val = colon + 1;
        vlen = len - flen - 1;
        if (vlen > 0 && val[0] == ' ') {     /* strip one optional leading space */
            val++;
            vlen--;
        }
    } else {
        flen = len;                          /* field name with empty value */
        val = line + len;
        vlen = 0;
    }

    if (flen == 4 && memcmp(line, "data", 4) == 0) {
        if (!buf_append(&s->data, &s->data_len, &s->data_cap, val, vlen)
            || !buf_append(&s->data, &s->data_len, &s->data_cap, "\n", 1)) {
            s->oom = true;
            return false;
        }
        s->have_data = true;
    } else if (flen == 5 && memcmp(line, "event", 5) == 0) {
        size_t c = vlen < sizeof s->event - 1 ? vlen : sizeof s->event - 1;
        memcpy(s->event, val, c);
        s->event[c] = '\0';
    } else if (flen == 2 && memcmp(line, "id", 2) == 0) {
        size_t c = vlen < sizeof s->id - 1 ? vlen : sizeof s->id - 1;
        memcpy(s->id, val, c);
        s->id[c] = '\0';
    }
    /* retry: and unknown fields are ignored */
    return true;
}

hc_http_sse *hc_http_sse_new(hc_http_sse_emit_fn emit, void *user)
{
    hc_http_sse *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->emit = emit;
    s->user = user;
    return s;
}

void hc_http_sse_free(hc_http_sse *s)
{
    if (!s) return;
    free(s->carry);
    free(s->data);
    free(s);
}

void hc_http_sse_reset(hc_http_sse *s)
{
    if (!s) return;
    s->carry_len = 0;
    reset_event(s);
    s->oom = false;
}

bool hc_http_sse_feed(hc_http_sse *s, const char *bytes, size_t n)
{
    if (!s) return false;
    if (!buf_append(&s->carry, &s->carry_len, &s->carry_cap, bytes, n)) {
        s->oom = true;
        return false;
    }
    size_t start = 0;
    bool cont = true;
    for (size_t i = 0; i < s->carry_len && cont; i++) {
        if (s->carry[i] == '\n') {
            const char *line = s->carry + start;
            size_t ll = i - start;
            if (ll > 0 && line[ll - 1] == '\r') ll--; /* tolerate CRLF */
            cont = process_line(s, line, ll);
            start = i + 1;
        }
    }
    if (start > 0) { /* drop consumed lines, keep the incomplete tail */
        memmove(s->carry, s->carry + start, s->carry_len - start);
        s->carry_len -= start;
    }
    return cont && !s->oom;
}

bool hc_http_sse_finish(hc_http_sse *s)
{
    if (!s) return false;
    if (s->carry_len > 0) { /* an unterminated final line */
        const char *line = s->carry;
        size_t ll = s->carry_len;
        if (ll > 0 && line[ll - 1] == '\r') ll--;
        process_line(s, line, ll);
        s->carry_len = 0;
    }
    if (s->have_data) return dispatch(s);
    return !s->oom;
}

bool hc_http_sse_ok(const hc_http_sse *s) { return s && !s->oom; }
