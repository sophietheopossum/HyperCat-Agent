#ifndef HC_LLM_H
#define HC_LLM_H

#include "hc_http.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_llm — provider-agnostic LLM chat client (OpenAI-compatible HTTP), POSIX.
 *
 * Purpose:   build a chat-completion request, stream the response over hc_http, and decode the
 *            provider's SSE into text deltas and assembled tool calls — normalizing per-provider
 *            quirks. One client per provider config. Knows nothing of agents, sandboxing, or the
 *            bus; it is the thin model-access layer.
 * Owns:      a copy of the provider config strings; it BORROWS the hc_http handle (the caller
 *            owns and frees it). The request body it builds is owned by the caller (free()).
 * Threading: an hc_llm is single-owner and not thread-safe (it drives one hc_http handle).
 * Lifetime:  strings passed to the streaming handlers are valid only for that call — copy to
 *            keep. provider.extra_headers is borrowed and must outlive the hc_llm.
 * Secrets:   provider.api_key is held in memory for the client's life. v1 takes it from the
 *            caller (env/config); the OS-keychain hc_secrets (roadmap step 3) supplies it later.
 * ABI:       hc_http.h is INTENTIONALLY part of this public surface — hc_llm_probe returns
 *            hc_http_net_state, so a consumer of hc_llm transitively depends on the hc_http enum.
 *            hc_llm_usage is INTENTIONALLY a plain-data out-param struct (not an opaque handle): it
 *            carries a snapshot the caller reads, with no lifecycle to hide. Consequently any change
 *            to its layout (adding/reordering a field, widening a type) is an ABI break that requires
 *            a version bump — callers fill a stack struct and pass &it. The project is pre-1.0, so the
 *            layout is not yet frozen; this note marks the obligation once it is.
 */

typedef enum {
    HC_LLM_OK = 0,
    HC_LLM_ERR_INVALID,    /* bad argument                                   */
    HC_LLM_ERR_HTTP,       /* transport failure (offline/timeout/denied/TLS) */
    HC_LLM_ERR_STATUS,     /* provider returned a non-2xx HTTP status         */
    HC_LLM_ERR_PARSE,      /* request could not be built / tools_json invalid */
    HC_LLM_ERR_CANCELLED,  /* a handler requested cancellation                */
    HC_LLM_ERR_NOMEM
} hc_llm_status;

const char *hc_llm_status_str(hc_llm_status); /* static string */

typedef struct {
    const char *name;
    const char *base_url;          /* e.g. "https://openrouter.ai/api/v1"               */
    const char *api_key;           /* bearer token, or NULL for a keyless local server  */
    const char *model;
    const char *probe_host;        /* host for hc_llm_probe, or NULL                    */
    const char *probe_port;        /* port for hc_llm_probe, or NULL (defaults "443")   */
    const char *const *extra_headers; /* NULL-terminated; borrowed, must outlive hc_llm */
    const char *reasoning_effort;     /* one of the levels in hc_llm.c; NULL/"" = omit (the default)     */
    /* Extra top-level JSON merged verbatim into every request body. NULL/"" = omit, which is what
     * every hosted caller uses -- this exists for provider-specific routing the OpenAI schema has no
     * field for. The concrete need: OpenRouter pins an endpoint with
     * {"provider":{"only":["DeepInfra"],"allow_fallbacks":false}}, and without it a benchmark
     * comparing quantisations silently measures whichever endpoint the router happened to pick.
     * Must be a JSON OBJECT; its keys are copied onto the request root and override nothing we set.
     * COPIED by hc_llm_new, so the caller's buffer need not outlive the client (unlike extra_headers).
     * Invalid JSON is dropped and the request is built without it -- never failed outright. */
    const char *extra_body_json;
} hc_llm_provider;

typedef struct {
    const char *role;            /* "system" | "user" | "assistant" | "tool"                  */
    const char *content;
    const char *tool_call_id;    /* role "tool": links the result to its call (else NULL)      */
    const char *tool_calls_json; /* role "assistant": raw JSON array of tool_calls (else NULL) */
} hc_llm_message;

typedef struct {
    /* a text delta (NOT NUL-accumulated; copy if you must keep it) */
    void (*on_text)(const char *delta, size_t n, void *user);
    /* a fully assembled tool call: id, function name, and the JSON arguments string */
    void (*on_tool_call)(const char *id, const char *name, const char *args_json, void *user);
    /* return true to cancel the stream */
    bool (*should_cancel)(void *user);
    void *user;
} hc_llm_handlers;

/* Build an OpenAI-compatible chat-completion request body from `msgs`. `tools_json`, if non-NULL,
 * is a raw JSON array of tool specs embedded under "tools". Pure and offline-testable; returns a
 * malloc'd JSON string the caller frees, or NULL on OOM / invalid tools_json. */
char *hc_llm_build_request_json(const char *model, const hc_llm_message *msgs, size_t n_msgs,
                                const char *tools_json, bool stream);

/* As above, plus `reasoning_effort`: the OpenAI-compatible knob for how long a REASONING model thinks
 * before it answers. Accepted are "none", "minimal", "low", "medium", "high", "xhigh" and "max" -- the
 * familiar OpenAI trio plus the levels a live OpenRouter endpoint was observed to take. Anything else,
 * including NULL and "", omits the field entirely, which is what every non-reasoning provider wants and
 * is the default. Keep this list and effort_is_valid() in hc_llm.c in step; the single source of truth
 * is that function.
 *
 * This exists because reasoning time is a real failure mode, not a tuning nicety. A reasoning model on a
 * ~22k-token prompt was observed spending SIXTY SECONDS emitting nothing but reasoning tokens -- every one
 * of its completion tokens, none of them output -- and being cancelled by the client's wall-clock cap
 * before it began to answer. Raising the cap only defers that as context grows; capping the thinking
 * addresses it. Providers that do not support the field ignore it. */
char *hc_llm_build_request_json_ex(const char *model, const hc_llm_message *msgs, size_t n_msgs,
                                   const char *tools_json, bool stream, const char *reasoning_effort);

typedef struct hc_llm hc_llm;

hc_llm *hc_llm_new(const hc_llm_provider *cfg, hc_http *http); /* borrows http; NULL on failure */
void    hc_llm_free(hc_llm *);

/* Stream a chat turn: drives `handlers` with text deltas and assembled tool calls.
 * `finish_reason_out` (optional, cap fr_cap) receives the provider's finish_reason. Requires
 * network + (for a hosted provider) a valid api_key. */
hc_llm_status hc_llm_chat_stream(hc_llm *, const hc_llm_message *msgs, size_t n_msgs,
                                 const char *tools_json, const hc_llm_handlers *handlers,
                                 char *finish_reason_out, size_t fr_cap);

/* What a finished chat turn cost (P12). Filled from the provider's "usage" block (requested via
 * stream_options.include_usage); a token field is -1 when the provider did not report it (no
 * fabrication). Token counts are sanitized at decode (non-finite/negative/absurd are treated as
 * not-reported), so a consumer never sees a hostile magnitude. finish_reason is NOT carried here — it
 * is per-call and already returned via hc_llm_chat_stream's finish_reason_out. */
typedef struct {
    long   input_tokens;  /* prompt_tokens                                       */
    long   output_tokens; /* completion_tokens                                   */
    long   total_tokens;  /* total_tokens (or input+output if only those given)  */
    double latency_ms;    /* wall time of the request (running sum in the total) */
} hc_llm_usage;

/* The usage of the MOST RECENT hc_llm_chat_stream on this client (single-owner, so no race). Valid
 * until the next chat call. `out` is filled; on a client that has made no call, fields are -1 / 0. */
void hc_llm_last_usage(const hc_llm *, hc_llm_usage *out);

/* The RUNNING TOTAL of reported usage across every chat call on this client (tokens summed; a turn that
 * makes several LLM calls is captured by reading this before and after — the delta is that turn's cost).
 * Unreported per-call counts contribute 0 (never -1) so the sum stays meaningful. */
void hc_llm_total_usage(const hc_llm *, hc_llm_usage *out);

/* ---- embeddings (P01 memory) ----
 * A separate, non-streaming OpenAI-compatible /embeddings call on the SAME client (base_url + key);
 * the embeddings `model` is passed per call (config-driven, e.g. HC_EMBED_MODEL), distinct from the
 * chat model. hc_memory stores the returned vectors; the host never embeds inside the C storage lib. */

/* Defensive upper bound on a provider-reported embedding dimension (untrusted JSON). */
#define HC_LLM_EMBED_MAX_DIM 16384

/* Build the request body {"model":model,"input":[inputs...]}. Pure + offline-testable; malloc'd JSON
 * the caller free()s, or NULL on OOM / invalid argument. */
char *hc_llm_build_embed_request_json(const char *model, const char *const *inputs, size_t n);

/* Parse an /embeddings response body into a contiguous n*(*out_dim) float block (row i = the vector for
 * inputs[i], row-major). The provider JSON is UNTRUSTED: the row count must equal n, every row must
 * share one dimension in [1, HC_LLM_EMBED_MAX_DIM], and every cell must be finite — otherwise it fails
 * with NO partial/poisoned vector returned. 0 on success (*out_vecs malloc'd, caller free()s; *out_dim
 * set), -1 on any malformed/hostile input (*out_vecs left NULL). Pure + offline-testable. */
int hc_llm_parse_embeddings(const char *json, size_t len, size_t n, float **out_vecs, int *out_dim);

/* Embed `n` input strings with `model`, via POST {base_url}/embeddings (reusing this client's key +
 * extra_headers). On HC_LLM_OK writes a contiguous n*(*out_dim) float block to *out_vecs (caller
 * free()s) + the dimension to *out_dim — whatever the configured model returns (the store records it).
 * Any error leaves *out_vecs NULL. Requires network + (for a hosted provider) a valid api_key. */
hc_llm_status hc_llm_embed(hc_llm *, const char *model, const char *const *inputs, size_t n,
                           float **out_vecs, int *out_dim);

/* Reachability probe to the provider's configured host:port (hc_http_net_probe under the hood). */
hc_http_net_state hc_llm_probe(hc_llm *, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* HC_LLM_H */
