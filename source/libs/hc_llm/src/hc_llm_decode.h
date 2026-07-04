#ifndef HC_LLM_DECODE_H
#define HC_LLM_DECODE_H

/* hc_llm_decode — INTERNAL: turn OpenAI-compatible streaming chat SSE events into text deltas
 * and assembled tool calls. Driven by hc_http_sse events, so it is unit-testable with canned
 * fixtures (no network). Tool calls arrive fragmented across deltas (id+name in the first, then
 * arguments appended); the decoder accumulates per choice-index and flushes complete calls when
 * finish_reason arrives or the stream ends.
 *
 * Owns:      the per-call argument buffers (heap, grown with a per-call ceiling); freed by
 *            hc_llm_decoder_reset. The handlers struct is borrowed (caller-owned).
 * Threading: single-owner — one decoder is driven by one hc_llm_chat_stream call on one thread; no
 *            sharing, no internal locking.
 * Lifetime:  stack-allocated by hc_llm.c per chat call: init -> fed SSE events -> read -> reset. The
 *            strings handed to the handlers point into the per-event JSON tree and are consumed before
 *            it is freed. All provider input is untrusted: arg growth is bounded and token counts are
 *            sanitized at capture (see sane_tokens in the .c). */

#include "hc_http_sse.h"
#include "hc_llm.h"

#include <stdbool.h>
#include <stddef.h>

#define HC_LLM_MAX_TOOLCALLS 16

typedef struct {
    bool   active;
    char   id[80];
    char   name[128];
    char  *args;
    size_t args_len, args_cap;
} hc_llm_tool_acc;

typedef struct {
    const hc_llm_handlers *h;
    hc_llm_tool_acc tc[HC_LLM_MAX_TOOLCALLS];
    char  finish_reason[32];
    bool  finished;
    bool  flushed;
    bool  parse_error;
    /* the provider's `usage` block (P12), captured from the final stream chunk; -1 = not reported */
    long  input_tokens;
    long  output_tokens;
    long  total_tokens;
} hc_llm_decoder;

void hc_llm_decoder_init(hc_llm_decoder *, const hc_llm_handlers *);
void hc_llm_decoder_finish(hc_llm_decoder *); /* flush assembled tool calls (idempotent) */
void hc_llm_decoder_reset(hc_llm_decoder *);  /* flush if needed, then free arg buffers */

/* hc_http_sse_emit_fn-compatible: feed it SSE events. `user` is an hc_llm_decoder*. */
bool hc_llm_decoder_on_sse(const hc_http_sse_event *ev, void *user);

#endif /* HC_LLM_DECODE_H */
