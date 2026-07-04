/* hc_llm_decode.c — streaming chat SSE decoder. See hc_llm_decode.h.
 *
 * Each SSE event's data is one JSON chunk: choices[0].delta carries either a text `content`
 * fragment or `tool_calls` fragments; choices[0].finish_reason marks the end. The '[DONE]'
 * sentinel (hc_http_sse_event.is_done) also ends the stream. Strings handed to the handlers
 * point into the per-event JSON tree and are consumed before it is freed.
 */

#include "hc_llm_decode.h"

#include "hc_json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hc_llm_decoder_init(hc_llm_decoder *d, const hc_llm_handlers *h)
{
    memset(d, 0, sizeof *d);
    d->h = h;
    d->input_tokens = d->output_tokens = d->total_tokens = -1; /* "not reported" until the usage chunk */
}

/* Cap one tool call's accumulated arguments. hc_http_sse bounds a single event, but arguments
 * are concatenated ACROSS events, so a hostile provider could otherwise stream them without end. */
#define HC_LLM_MAX_TOOL_ARGS (512u * 1024)

/* Sane domain for a reported token count. Real counts are small (a turn is bounded by the model's
 * context); anything negative or absurdly large is a hostile/garbage provider value, so we treat it as
 * "not reported" (-1) rather than fabricate or let it feed the downstream running-total accumulators.
 * The ceiling (2^40 ≈ 1.1e12) is far above any real turn yet far below where int64 sums could overflow. */
#define HC_LLM_TOKENS_SANE_MAX (1LL << 40)

static long sane_tokens(int64_t v)
{
    return (v < 0 || v > HC_LLM_TOKENS_SANE_MAX) ? -1 : (long)v;
}

static bool args_append(hc_llm_tool_acc *t, const char *s, size_t n)
{
    if (n > SIZE_MAX - 1 - t->args_len) return false;
    size_t need = t->args_len + n + 1;
    if (need > HC_LLM_MAX_TOOL_ARGS) return false; /* refuse to grow past the per-call ceiling */
    if (need > t->args_cap) {
        size_t ncap = t->args_cap ? t->args_cap : 128;
        while (ncap < need) {
            if (ncap > SIZE_MAX / 2) {
                ncap = need;
                break;
            }
            ncap *= 2;
        }
        char *nb = realloc(t->args, ncap);
        if (!nb) return false;
        t->args = nb;
        t->args_cap = ncap;
    }
    memcpy(t->args + t->args_len, s, n);
    t->args_len += n;
    t->args[t->args_len] = '\0';
    return true;
}

void hc_llm_decoder_finish(hc_llm_decoder *d)
{
    if (d->flushed) return;
    d->flushed = true;
    for (size_t i = 0; i < HC_LLM_MAX_TOOLCALLS; i++) {
        hc_llm_tool_acc *t = &d->tc[i];
        if (t->active && d->h && d->h->on_tool_call)
            d->h->on_tool_call(t->id, t->name, t->args ? t->args : "", d->h->user);
    }
}

void hc_llm_decoder_reset(hc_llm_decoder *d)
{
    for (size_t i = 0; i < HC_LLM_MAX_TOOLCALLS; i++) free(d->tc[i].args);
    const hc_llm_handlers *h = d->h;
    memset(d, 0, sizeof *d);
    d->h = h;
}

static void accumulate_tool_calls(hc_llm_decoder *d, const hc_json *tcs)
{
    size_t n = hc_json_arr_len(tcs);
    for (size_t i = 0; i < n; i++) {
        const hc_json *tc = hc_json_arr_at(tcs, i);
        if (!tc) continue;
        /* Range-check index as a double first — casting an out-of-range / NaN JSON number to an
         * integer is UB; this comparison rejects NaN/Inf/out-of-range without ever narrowing. */
        double idxd = hc_json_get_double(tc, "index", -1.0);
        if (!(idxd >= 0.0 && idxd < (double)HC_LLM_MAX_TOOLCALLS)) continue;
        hc_llm_tool_acc *t = &d->tc[(size_t)idxd];
        t->active = true;
        const char *id = hc_json_get_str(tc, "id", NULL);
        if (id && id[0]) snprintf(t->id, sizeof t->id, "%s", id);
        const hc_json *fn = hc_json_get(tc, "function");
        if (fn) {
            const char *name = hc_json_get_str(fn, "name", NULL);
            if (name && name[0]) snprintf(t->name, sizeof t->name, "%s", name);
            const char *args = hc_json_get_str(fn, "arguments", NULL);
            if (args && args[0] && !args_append(t, args, strlen(args)))
                d->parse_error = true; /* OOM or per-call args ceiling exceeded */
        }
    }
}

bool hc_llm_decoder_on_sse(const hc_http_sse_event *ev, void *user)
{
    hc_llm_decoder *d = user;

    if (ev->is_done) {
        hc_llm_decoder_finish(d);
        d->finished = true;
    } else if (ev->data_len > 0) {
        hc_json *root = hc_json_parse(ev->data, ev->data_len);
        if (!root) {
            d->parse_error = true;
        } else {
            const hc_json *choices = hc_json_get(root, "choices");
            const hc_json *c0 = choices ? hc_json_arr_at(choices, 0) : NULL;
            if (c0) {
                const hc_json *delta = hc_json_get(c0, "delta");
                if (delta) {
                    const char *content = hc_json_get_str(delta, "content", NULL);
                    if (content && content[0] && d->h && d->h->on_text)
                        d->h->on_text(content, strlen(content), d->h->user);
                    const hc_json *tcs = hc_json_get(delta, "tool_calls");
                    if (tcs && hc_json_is_array(tcs)) accumulate_tool_calls(d, tcs);
                }
                const char *fr = hc_json_get_str(c0, "finish_reason", NULL);
                if (fr && fr[0]) {
                    snprintf(d->finish_reason, sizeof d->finish_reason, "%s", fr);
                    hc_llm_decoder_finish(d);
                    d->finished = true;
                }
            }
            /* the usage block (P12) is a root-level sibling of choices, in the final chunk when the
             * request asked for stream_options.include_usage; absent providers leave the fields -1. */
            const hc_json *usage = hc_json_get(root, "usage");
            if (usage && hc_json_is_object(usage)) {
                d->input_tokens = sane_tokens(hc_json_get_int(usage, "prompt_tokens", -1));
                d->output_tokens = sane_tokens(hc_json_get_int(usage, "completion_tokens", -1));
                d->total_tokens = sane_tokens(hc_json_get_int(usage, "total_tokens", -1));
            }
            hc_json_free(root);
        }
    }

    if (d->h && d->h->should_cancel && d->h->should_cancel(d->h->user)) return false;
    return true;
}
