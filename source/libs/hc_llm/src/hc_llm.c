/* hc_llm.c — provider config, request builder, and the streaming chat call. See hc_llm.h.
 *
 * The request builder is a pure hc_json function (offline-testable). chat_stream wires hc_http's
 * streaming POST to an hc_http_sse parser whose events drive the hc_llm_decode decoder. JSON is
 * confined to this module's use of hc_json; consumers see only plain strings.
 */

#include "hc_llm.h"

#include "hc_http_sse.h"
#include "hc_json.h"
#include "hc_llm_decode.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct hc_llm {
    hc_http *http; /* borrowed */
    char base_url[256];
    char api_key[256];
    char model[128];
    char probe_host[128];
    char probe_port[16];
    const char *const *extra_headers; /* borrowed */
    char reasoning_effort[16];        /* "" = omit the field */
    char *extra_body_json;            /* owned copy; NULL = omit (see hc_llm_provider) */
    hc_llm_usage last_usage;          /* the most recent call's token/cost usage (P12)      */
    hc_llm_usage total_usage;         /* running total of reported usage across all calls    */
};

/* Build one message object fully (role + content, plus the optional tool_call_id and adopted
 * tool_calls array). Returns an owned object, or NULL on failure with nothing leaked. */
static hc_json *build_message(const hc_llm_message *msg)
{
    hc_json *m = hc_json_new_object();
    if (!m) return NULL;
    bool ok = hc_json_obj_set_str(m, "role", msg->role ? msg->role : "user")
              && hc_json_obj_set_str(m, "content", msg->content ? msg->content : "");
    if (ok && msg->tool_call_id && msg->tool_call_id[0])
        ok = hc_json_obj_set_str(m, "tool_call_id", msg->tool_call_id);
    if (ok && msg->tool_calls_json && msg->tool_calls_json[0]) {
        hc_json *tcs = hc_json_parse(msg->tool_calls_json, strlen(msg->tool_calls_json));
        if (!tcs)
            ok = false;
        else if (!hc_json_obj_set(m, "tool_calls", tcs)) /* adopts tcs; frees it on failure */
            ok = false;
    }
    if (!ok) {
        hc_json_free(m);
        return NULL;
    }
    return m;
}

/* The accepted ladder, verified against a live OpenRouter endpoint rather than assumed: "low"/"medium"/
 * "high" is the familiar OpenAI trio, but "minimal", "xhigh" and "max" are accepted too
 *
 * An unrecognised value is dropped rather than forwarded: a provider that rejects an unknown enum fails
 * the whole request, and quietly using the default beats every call 400ing on a typo in a settings box.
 * The trade is that a genuinely new level lands here as a silent default until this list is updated. */
static int effort_is_valid(const char *e)
{
    static const char *const kLevels[] = {"none", "minimal", "low", "medium", "high", "xhigh", "max"};
    if (!e) return 0;
    for (size_t i = 0; i < sizeof kLevels / sizeof kLevels[0]; i++)
        if (strcmp(e, kLevels[i]) == 0) return 1;
    return 0;
}

char *hc_llm_build_request_json(const char *model, const hc_llm_message *msgs, size_t n_msgs,
                                const char *tools_json, bool stream)
{
    return hc_llm_build_request_json_ex(model, msgs, n_msgs, tools_json, stream, NULL);
}

/* As hc_llm_build_request_json_ex, plus `extra_body_json`: a JSON object whose keys are merged onto
 * the request root. Kept as a separate entry point so the existing two-arity API is untouched. */
char *hc_llm_build_request_json_ex2(const char *model, const hc_llm_message *msgs, size_t n_msgs,
                                    const char *tools_json, bool stream, const char *reasoning_effort,
                                    const char *extra_body_json)
{
    char *base = hc_llm_build_request_json_ex(model, msgs, n_msgs, tools_json, stream, reasoning_effort);
    if (!base || !extra_body_json || !extra_body_json[0]) return base;
    /* Validate, then splice the RE-EMITTED text -- never the caller's original bytes.
     *
     * Parsing alone proves nothing about the rest of the string: cJSON stops at the root value and
     * ACCEPTS trailing content, so `{"a":1} JUNK` passes an is_object gate. Splicing from the original
     * buffer then copied to its NUL -- `strlen` past the object's closing brace -- and pasted that tail
     * into the request body. Printing the parsed TREE is what bounds the text to the object we actually
     * validated; it also normalises whitespace, so the brace-hunting the old form needed goes away.
     * Malformed extra JSON must never lose the request -- fall back to the un-merged body. */
    hc_json *add = hc_json_parse(extra_body_json, strlen(extra_body_json));
    if (!add || !hc_json_is_object(add)) {
        if (add) hc_json_free(add);
        return base;
    }

    /* The extra's keys are appended AFTER ours, so any key we also set would WIN under last-key-wins.
     * Refuse the whole block rather than emit a body whose model/messages/stream are not the ones the
     * caller asked for -- `"model"` silently redirects the turn to another model, `"stream":false`
     * starves the SSE decoder, `"messages"` substitutes the prompt. Rejecting wholesale (rather than
     * dropping the offending key) keeps the rule one line to state and impossible to half-apply.
     *
     * By NAME, not by iteration: hc_json exposes no key enumerator, and this is the complete set
     * hc_llm_build_request_json_ex can set -- checked against it, not against the base text, so a
     * conditionally-absent key (tools, reasoning_effort) is still reserved. Keep the two in step. */
    static const char *const kReserved[] = {"model",  "messages", "stream",
                                            "tools",  "stream_options", "reasoning_effort"};
    for (size_t i = 0; i < sizeof kReserved / sizeof *kReserved; i++) {
        if (hc_json_get(add, kReserved[i])) {
            hc_json_free(add);
            return base;
        }
    }

    char *canon = hc_json_print_canonical(add);
    hc_json_free(add);
    if (!canon) return base;

    /* Textual splice rather than a tree merge: both sides are now objects in canonical text, so
     * `{...base...}` + `,` + `...extra...}` is well-formed by construction. hc_json has no merge
     * primitive, and adding one to a second shared library for this single caller is not worth it. */
    size_t bl = strlen(base);
    while (bl > 0 && (base[bl - 1] == ' ' || base[bl - 1] == '\n' || base[bl - 1] == '\t')) bl--;
    const size_t cl = strlen(canon); /* canonical: exactly "{...}", no padding, no tail */
    if (bl < 2 || base[bl - 1] != '}' || cl < 2 || canon[0] != '{' || canon[cl - 1] != '}' ||
        cl == 2 /* "{}" would splice a trailing comma */) {
        free(canon);
        return base;
    }

    const size_t el = cl - 1; /* past the opening brace, up to and including the closing one */
    char        *out = malloc(bl - 1 + 1 + el + 1);
    if (!out) {
        free(canon);
        return base;
    }
    memcpy(out, base, bl - 1); /* base without its closing brace */
    out[bl - 1] = ',';
    memcpy(out + bl, canon + 1, el); /* extra without its opening brace (keeps its closing one) */
    out[bl + el] = '\0';
    free(canon);
    free(base);
    return out;
}

char *hc_llm_build_request_json_ex(const char *model, const hc_llm_message *msgs, size_t n_msgs,
                                   const char *tools_json, bool stream, const char *reasoning_effort)
{
    hc_json *root = hc_json_new_object();
    if (!root) return NULL;
    if (!hc_json_obj_set_str(root, "model", model ? model : "")
        || !hc_json_obj_set_bool(root, "stream", stream)) {
        hc_json_free(root);
        return NULL;
    }
    if (effort_is_valid(reasoning_effort)
        && !hc_json_obj_set_str(root, "reasoning_effort", reasoning_effort)) {
        hc_json_free(root);
        return NULL;
    }

    /* ask the provider to include the token-usage block in the final stream chunk (P12). Providers that
     * ignore stream_options simply omit usage; the decoder then leaves the counts -1 (no fabrication). */
    if (stream) {
        hc_json *so = hc_json_new_object();
        if (!so) {
            hc_json_free(root);
            return NULL;
        }
        if (!hc_json_obj_set_bool(so, "include_usage", true)) {
            hc_json_free(so);
            hc_json_free(root);
            return NULL;
        }
        if (!hc_json_obj_set(root, "stream_options", so)) { /* adopts so; frees it on failure */
            hc_json_free(root);
            return NULL;
        }
    }

    hc_json *arr = hc_json_new_array();
    if (!arr) {
        hc_json_free(root);
        return NULL;
    }
    for (size_t i = 0; i < n_msgs; i++) {
        hc_json *m = build_message(&msgs[i]);
        if (!m || !hc_json_arr_append(arr, m)) { /* arr_append adopts m / frees it on failure */
            hc_json_free(arr);
            hc_json_free(root);
            return NULL;
        }
    }
    if (!hc_json_obj_set(root, "messages", arr)) { /* adopts arr; frees it on failure */
        hc_json_free(root);
        return NULL;
    }

    if (tools_json && tools_json[0]) {
        hc_json *tools = hc_json_parse(tools_json, strlen(tools_json));
        if (!tools) { /* invalid tools_json */
            hc_json_free(root);
            return NULL;
        }
        if (!hc_json_obj_set(root, "tools", tools)) { /* adopts tools; frees it on failure */
            hc_json_free(root);                       /* do NOT free tools again here */
            return NULL;
        }
    }

    char *out = hc_json_print(root, false);
    hc_json_free(root);
    return out;
}

hc_llm *hc_llm_new(const hc_llm_provider *cfg, hc_http *http)
{
    if (!cfg || !http || !cfg->base_url) return NULL;
    hc_llm *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->http = http;
    snprintf(l->base_url, sizeof l->base_url, "%s", cfg->base_url);
    snprintf(l->api_key, sizeof l->api_key, "%s", cfg->api_key ? cfg->api_key : "");
    snprintf(l->model, sizeof l->model, "%s", cfg->model ? cfg->model : "");
    snprintf(l->probe_host, sizeof l->probe_host, "%s", cfg->probe_host ? cfg->probe_host : "");
    snprintf(l->probe_port, sizeof l->probe_port, "%s", cfg->probe_port ? cfg->probe_port : "443");
    snprintf(l->reasoning_effort, sizeof l->reasoning_effort, "%s",
             cfg->reasoning_effort ? cfg->reasoning_effort : "");
    l->extra_body_json = (cfg->extra_body_json && cfg->extra_body_json[0])
                             ? strdup(cfg->extra_body_json)
                             : NULL;
    l->extra_headers = cfg->extra_headers;
    l->last_usage.input_tokens = l->last_usage.output_tokens = l->last_usage.total_tokens = -1;
    return l;
}

/* Non-elidable zero — the portable equivalent of explicit_bzero/memset_s, so the compiler cannot
 * drop the secret scrub as a dead store (Docs/Plan_HyperCat/08-secrets-and-security.md). */
static void secure_zero(void *p, size_t n)
{
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

void hc_llm_free(hc_llm *l)
{
    if (!l) return;
    secure_zero(l->api_key, sizeof l->api_key); /* scrub the key before releasing the block */
    free(l->extra_body_json);                   /* owned copy taken in hc_llm_new */
    free(l);
}

/* feed streamed bytes into the SSE parser; false aborts the transfer (cancellation) */
static bool stream_to_sse(const char *chunk, size_t n, void *user)
{
    return hc_http_sse_feed((hc_http_sse *)user, chunk, n);
}

/* Saturating add for the running token total. An unreported per-turn count is the sentinel -1 (and a
 * sane reported count is in [0, 2^40] after decode sanitizes it), so b <= 0 must contribute NOTHING — not
 * be added — or unreported turns would drift the total. Reported counts saturate at LONG_MAX rather than
 * overflow (signed overflow is UB), so a hostile-but-in-range stream of turns can never wrap the total. */
static long sat_add_long(long a, long b)
{
    if (b <= 0) return a;
    return (a > LONG_MAX - b) ? LONG_MAX : a + b;
}

hc_llm_status hc_llm_chat_stream(hc_llm *l, const hc_llm_message *msgs, size_t n_msgs,
                                 const char *tools_json, const hc_llm_handlers *handlers,
                                 char *finish_reason_out, size_t fr_cap)
{
    if (!l || !msgs) return HC_LLM_ERR_INVALID;

    char *body = hc_llm_build_request_json_ex2(l->model, msgs, n_msgs, tools_json, true,
                                               l->reasoning_effort[0] ? l->reasoning_effort : NULL,
                                               l->extra_body_json);
    if (!body) return HC_LLM_ERR_PARSE;

    char auth[300];
    const char *hdrs[20];
    size_t hi = 0;
    hdrs[hi++] = "Content-Type: application/json";
    if (l->api_key[0]) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", l->api_key);
        hdrs[hi++] = auth;
    }
    if (l->extra_headers)
        for (size_t i = 0; l->extra_headers[i] && hi < 18; i++) hdrs[hi++] = l->extra_headers[i];
    hdrs[hi] = NULL;

    char url[320];
    snprintf(url, sizeof url, "%s/chat/completions", l->base_url);

    hc_llm_decoder dec;
    hc_llm_decoder_init(&dec, handlers);
    hc_http_sse *sse = hc_http_sse_new(hc_llm_decoder_on_sse, &dec);
    if (!sse) {
        free(body);
        hc_llm_decoder_reset(&dec);
        return HC_LLM_ERR_NOMEM;
    }

    long            status = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    hc_http_status hst = hc_http_post_stream(l->http, url, hdrs, body, strlen(body), stream_to_sse,
                                             sse, &status);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(body);
    hc_http_sse_finish(sse);
    hc_http_sse_free(sse);
    hc_llm_decoder_finish(&dec); /* flush any tool calls if the stream ended without a marker */

    if (finish_reason_out && fr_cap) snprintf(finish_reason_out, fr_cap, "%s", dec.finish_reason);
    /* record this turn's usage (P12) before the decoder is reset; the decoder has already sanitized the
     * token fields (non-finite/negative/absurd -> -1), so they are -1 or a sane non-negative count. */
    l->last_usage.input_tokens = dec.input_tokens;
    l->last_usage.output_tokens = dec.output_tokens;
    l->last_usage.total_tokens = dec.total_tokens;
    l->last_usage.latency_ms =
        (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    /* accumulate into the running total: sat_add_long treats unreported counts (-1) as a no-op and
     * saturates reported ones, so a hostile-but-in-range stream of turns can never overflow it (UB). */
    l->total_usage.input_tokens = sat_add_long(l->total_usage.input_tokens, dec.input_tokens);
    l->total_usage.output_tokens = sat_add_long(l->total_usage.output_tokens, dec.output_tokens);
    l->total_usage.total_tokens = sat_add_long(l->total_usage.total_tokens, dec.total_tokens);
    l->total_usage.latency_ms += l->last_usage.latency_ms;
    bool parse_error = dec.parse_error;
    hc_llm_decoder_reset(&dec);

    if (hst == HC_HTTP_ERR_CANCELLED) return HC_LLM_ERR_CANCELLED;
    if (hst != HC_HTTP_OK) return HC_LLM_ERR_HTTP;
    if (status < 200 || status >= 300) return HC_LLM_ERR_STATUS;
    if (parse_error) return HC_LLM_ERR_PARSE;
    return HC_LLM_OK;
}

void hc_llm_last_usage(const hc_llm *l, hc_llm_usage *out)
{
    if (!l || !out) return;
    *out = l->last_usage;
}

void hc_llm_total_usage(const hc_llm *l, hc_llm_usage *out)
{
    if (!l || !out) return;
    *out = l->total_usage;
}

/* ---- embeddings (P01) ------------------------------------------------------------------------- */

char *hc_llm_build_embed_request_json(const char *model, const char *const *inputs, size_t n)
{
    if (!model || !inputs || n == 0) return NULL;
    hc_json *root = hc_json_new_object();
    if (!root) return NULL;
    hc_json *arr = hc_json_new_array();
    if (!arr) {
        hc_json_free(root);
        return NULL;
    }
    bool ok = hc_json_obj_set_str(root, "model", model);
    for (size_t i = 0; ok && i < n; i++) ok = hc_json_arr_append_str(arr, inputs[i] ? inputs[i] : "");
    if (ok)
        ok = hc_json_obj_set(root, "input", arr); /* adopts arr (frees it on failure) */
    else
        hc_json_free(arr); /* not yet adopted */
    if (!ok) {
        hc_json_free(root);
        return NULL;
    }
    char *out = hc_json_print(root, false);
    hc_json_free(root);
    return out;
}

int hc_llm_parse_embeddings(const char *json, size_t len, size_t n, float **out_vecs, int *out_dim)
{
    if (out_vecs) *out_vecs = NULL;
    if (out_dim) *out_dim = 0;
    if (!json || n == 0 || !out_vecs || !out_dim) return -1;

    hc_json *r = hc_json_parse(json, len);
    if (!r) return -1;
    /* The provider JSON is untrusted: the row count must equal n, all rows must share one dimension in
     * a sane bound, and every cell must be finite — otherwise we return nothing (never a poisoned vector
     * that would corrupt cosine downstream), mirroring the P12 hostile-provider hardening. */
    const hc_json *data = hc_json_get(r, "data");
    if (!hc_json_is_array(data) || hc_json_arr_len(data) != n) {
        hc_json_free(r);
        return -1;
    }
    const hc_json *emb0 = hc_json_get(hc_json_arr_at(data, 0), "embedding");
    if (!hc_json_is_array(emb0)) {
        hc_json_free(r);
        return -1;
    }
    size_t dim = hc_json_arr_len(emb0);
    if (dim == 0 || dim > HC_LLM_EMBED_MAX_DIM || n > SIZE_MAX / (dim * sizeof(float))) {
        hc_json_free(r);
        return -1;
    }
    float *vecs = malloc(n * dim * sizeof *vecs);
    if (!vecs) {
        hc_json_free(r);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        const hc_json *emb = hc_json_get(hc_json_arr_at(data, i), "embedding");
        if (!hc_json_is_array(emb) || hc_json_arr_len(emb) != dim) {
            free(vecs);
            hc_json_free(r);
            return -1;
        }
        for (size_t j = 0; j < dim; j++) {
            double d = hc_json_as_double(hc_json_arr_at(emb, j), NAN);
            if (!isfinite(d)) {
                free(vecs);
                hc_json_free(r);
                return -1;
            }
            vecs[i * dim + j] = (float)d;
        }
    }
    hc_json_free(r);
    *out_vecs = vecs;
    *out_dim = (int)dim;
    return 0;
}

hc_llm_status hc_llm_embed(hc_llm *l, const char *model, const char *const *inputs, size_t n,
                           float **out_vecs, int *out_dim)
{
    if (out_vecs) *out_vecs = NULL;
    if (out_dim) *out_dim = 0;
    if (!l || !model || !inputs || n == 0 || !out_vecs || !out_dim) return HC_LLM_ERR_INVALID;

    char *body = hc_llm_build_embed_request_json(model, inputs, n);
    if (!body) return HC_LLM_ERR_PARSE;

    /* same auth/header construction as chat_stream; the embed model rides this client's endpoint + key */
    char        auth[300];
    const char *hdrs[20];
    size_t      hi = 0;
    hdrs[hi++] = "Content-Type: application/json";
    if (l->api_key[0]) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", l->api_key);
        hdrs[hi++] = auth;
    }
    if (l->extra_headers)
        for (size_t i = 0; l->extra_headers[i] && hi < 18; i++) hdrs[hi++] = l->extra_headers[i];
    hdrs[hi] = NULL;

    char url[320];
    snprintf(url, sizeof url, "%s/embeddings", l->base_url);

    hc_http_response resp = {0};
    hc_http_status   hst = hc_http_post(l->http, url, hdrs, body, strlen(body), &resp);
    free(body);
    if (hst != HC_HTTP_OK) {
        hc_http_response_free(&resp);
        return hst == HC_HTTP_ERR_CANCELLED ? HC_LLM_ERR_CANCELLED : HC_LLM_ERR_HTTP;
    }
    if (resp.status < 200 || resp.status >= 300) {
        hc_http_response_free(&resp);
        return HC_LLM_ERR_STATUS;
    }
    int rc = hc_llm_parse_embeddings(resp.body, resp.body_len, n, out_vecs, out_dim);
    hc_http_response_free(&resp);
    return rc == 0 ? HC_LLM_OK : HC_LLM_ERR_PARSE;
}

hc_http_net_state hc_llm_probe(hc_llm *l, int timeout_ms)
{
    if (!l || !l->probe_host[0]) return HC_HTTP_NET_NO_DNS;
    return hc_http_net_probe(l->probe_host, l->probe_port, timeout_ms);
}

const char *hc_llm_status_str(hc_llm_status s)
{
    switch (s) {
    case HC_LLM_OK:            return "ok";
    case HC_LLM_ERR_INVALID:   return "invalid argument";
    case HC_LLM_ERR_HTTP:      return "transport error";
    case HC_LLM_ERR_STATUS:    return "provider returned an error status";
    case HC_LLM_ERR_PARSE:     return "request/response parse error";
    case HC_LLM_ERR_CANCELLED: return "cancelled";
    case HC_LLM_ERR_NOMEM:     return "out of memory";
    }
    return "unknown";
}
