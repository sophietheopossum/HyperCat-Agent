/* Tests for hc_llm — fully offline. Verifies the pure request builder (round-tripped through
 * hc_json) and the streaming decoder (canned OpenAI SSE driven through hc_http_sse into the
 * hc_llm_decode decoder), including tool-call assembly across deltas. The live chat_stream path
 * needs a real provider + key and is exercised at the integration stage, not here. Exit non-zero
 * on any failure. */

#include "hc_llm.h"
#include "hc_llm_decode.h"

#include "hc_http_sse.h"
#include "hc_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL: %s\n", (msg));                                           \
            g_fails++;                                                                      \
        }                                                                                   \
    } while (0)

static void test_request_builder(void)
{
    hc_llm_message msgs[2] = {
        {"system", "be terse", NULL, NULL},
        {"user", "hi", NULL, NULL},
    };
    char *body = hc_llm_build_request_json("gpt-x", msgs, 2, NULL, true);
    CHECK(body != NULL, "request builder returns a body");
    if (!body) return;

    hc_json *root = hc_json_parse(body, strlen(body));
    CHECK(root != NULL, "request body parses as JSON");
    if (root) {
        CHECK(strcmp(hc_json_get_str(root, "model", ""), "gpt-x") == 0, "model field");
        CHECK(hc_json_get_bool(root, "stream", false) == true, "stream field");
        const hc_json *so = hc_json_get(root, "stream_options"); /* P12: ask for usage */
        CHECK(so && hc_json_get_bool(so, "include_usage", false) == true,
              "stream request asks for include_usage");
        const hc_json *m = hc_json_get(root, "messages");
        CHECK(m && hc_json_is_array(m) && hc_json_arr_len(m) == 2, "messages array length");
        const hc_json *m1 = m ? hc_json_arr_at(m, 1) : NULL;
        CHECK(m1 && strcmp(hc_json_get_str(m1, "role", ""), "user") == 0
                  && strcmp(hc_json_get_str(m1, "content", ""), "hi") == 0,
              "second message contents");
        hc_json_free(root);
    }
    free(body);

    /* tools_json is embedded; invalid tools_json is rejected */
    char *with_tools =
        hc_llm_build_request_json("m", msgs, 1, "[{\"type\":\"function\"}]", false);
    CHECK(with_tools != NULL, "request builder embeds tools array");
    if (with_tools) {
        hc_json *r = hc_json_parse(with_tools, strlen(with_tools));
        CHECK(r && hc_json_is_array(hc_json_get(r, "tools")), "tools is an array");
        hc_json_free(r);
        free(with_tools);
    }
    CHECK(hc_llm_build_request_json("m", msgs, 1, "{not json", false) == NULL,
          "invalid tools_json rejected");
}

/* recording handlers */
static char g_text[256];
static char g_tc_id[80];
static char g_tc_name[128];
static char g_tc_args[256];
static int  g_tc_count;

static void rec_text(const char *delta, size_t n, void *u)
{
    (void)u;
    size_t have = strlen(g_text);
    if (have + n < sizeof g_text) {
        memcpy(g_text + have, delta, n);
        g_text[have + n] = '\0';
    }
}
static void rec_tool(const char *id, const char *name, const char *args, void *u)
{
    (void)u;
    g_tc_count++;
    snprintf(g_tc_id, sizeof g_tc_id, "%s", id);
    snprintf(g_tc_name, sizeof g_tc_name, "%s", name);
    snprintf(g_tc_args, sizeof g_tc_args, "%s", args);
}

static void test_decoder(void)
{
    g_text[0] = '\0';
    g_tc_id[0] = g_tc_name[0] = g_tc_args[0] = '\0';
    g_tc_count = 0;

    /* a canned OpenAI-compatible stream: two content deltas, then a tool call assembled across
     * two argument fragments, then finish_reason, then [DONE]. */
    const char *stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
        "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"city\\\":\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"\\\"NYC\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":7,"
        "\"total_tokens\":19}}\n\n" /* P12: the final usage chunk (stream_options.include_usage) */
        "data: [DONE]\n\n";

    hc_llm_handlers h = {rec_text, rec_tool, NULL, NULL};
    hc_llm_decoder dec;
    hc_llm_decoder_init(&dec, &h);

    hc_http_sse *sse = hc_http_sse_new(hc_llm_decoder_on_sse, &dec);
    CHECK(sse != NULL, "sse parser allocates");
    if (sse) {
        /* feed in awkward 7-byte slices to exercise partial frames */
        size_t len = strlen(stream);
        for (size_t i = 0; i < len; i += 7) {
            size_t n = i + 7 > len ? len - i : 7;
            hc_http_sse_feed(sse, stream + i, n);
        }
        hc_http_sse_finish(sse);
        hc_http_sse_free(sse);
    }
    hc_llm_decoder_finish(&dec);

    CHECK(strcmp(g_text, "Hello world") == 0, "content deltas concatenated");
    CHECK(g_tc_count == 1, "exactly one tool call emitted");
    CHECK(strcmp(g_tc_name, "get_weather") == 0, "tool call name");
    CHECK(strcmp(g_tc_id, "call_1") == 0, "tool call id");
    CHECK(strcmp(g_tc_args, "{\"city\":\"NYC\"}") == 0, "tool call arguments assembled");
    CHECK(strcmp(dec.finish_reason, "tool_calls") == 0, "finish_reason captured");
    CHECK(dec.input_tokens == 12 && dec.output_tokens == 7 && dec.total_tokens == 19,
          "usage block captured from the final chunk (P12)");

    hc_llm_decoder_reset(&dec);
}

/* A hostile provider can put any magnitude in the usage block. Each token field must be sanitized to
 * "not reported" (-1) when negative or absurd (the int64 conversion would otherwise be UB), while a sane
 * value passes through unchanged — no UB, no fabrication. */
static void test_usage_sanitize(void)
{
    const char *stream = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1e308,"
                         "\"completion_tokens\":-5,\"total_tokens\":500}}\n\n"
                         "data: [DONE]\n\n";
    hc_llm_handlers h = {rec_text, rec_tool, NULL, NULL};
    hc_llm_decoder  dec;
    hc_llm_decoder_init(&dec, &h);
    hc_http_sse *sse = hc_http_sse_new(hc_llm_decoder_on_sse, &dec);
    CHECK(sse != NULL, "sse parser allocates (sanitize)");
    if (sse) {
        hc_http_sse_feed(sse, stream, strlen(stream));
        hc_http_sse_finish(sse);
        hc_http_sse_free(sse);
    }
    hc_llm_decoder_finish(&dec);
    CHECK(dec.input_tokens == -1, "absurd prompt_tokens dropped to not-reported (no UB)");
    CHECK(dec.output_tokens == -1, "negative completion_tokens dropped to not-reported");
    CHECK(dec.total_tokens == 500, "sane total_tokens preserved");
    hc_llm_decoder_reset(&dec);
}

/* Embeddings (P01): the pure request builder + the untrusted-provider response parse. The parse is the
 * security-relevant half — a hostile /embeddings response must never yield a partial or poisoned vector. */
static void test_embeddings(void)
{
    const char *inputs[2] = {"alpha", "beta"};
    char       *body = hc_llm_build_embed_request_json("embed-x", inputs, 2);
    CHECK(body != NULL, "embed request builder returns a body");
    if (body) {
        hc_json *root = hc_json_parse(body, strlen(body));
        CHECK(root && strcmp(hc_json_get_str(root, "model", ""), "embed-x") == 0, "embed model field");
        const hc_json *in = root ? hc_json_get(root, "input") : NULL;
        CHECK(in && hc_json_is_array(in) && hc_json_arr_len(in) == 2, "embed input is a 2-element array");
        CHECK(in && strcmp(hc_json_as_str(hc_json_arr_at(in, 0), ""), "alpha") == 0, "embed input[0]");
        hc_json_free(root);
        free(body);
    }

    float *vecs = NULL;
    int    dim = 0;
    /* a valid 2-row, dim-3 response (exactly-representable floats so == is robust) */
    const char *ok = "{\"data\":[{\"embedding\":[0.5,0.25,-0.75]},{\"embedding\":[-1.0,0.0,0.5]}]}";
    CHECK(hc_llm_parse_embeddings(ok, strlen(ok), 2, &vecs, &dim) == 0, "parse valid embeddings");
    CHECK(dim == 3, "parsed dimension");
    if (vecs && dim == 3)
        CHECK(vecs[0] == 0.5f && vecs[2] == -0.75f && vecs[3] == -1.0f && vecs[5] == 0.5f,
              "embedding floats row-major");
    free(vecs);
    vecs = NULL;

    /* every hostile/malformed case must fail AND leave *out_vecs NULL */
    const char *count = "{\"data\":[{\"embedding\":[0.1,0.2,0.3]}]}"; /* 1 row but n=2 */
    CHECK(hc_llm_parse_embeddings(count, strlen(count), 2, &vecs, &dim) == -1 && !vecs,
          "reject row-count mismatch");
    const char *nodata = "{\"object\":\"list\"}";
    CHECK(hc_llm_parse_embeddings(nodata, strlen(nodata), 1, &vecs, &dim) == -1 && !vecs,
          "reject missing data");
    const char *ragged = "{\"data\":[{\"embedding\":[0.1,0.2]},{\"embedding\":[0.1,0.2,0.3]}]}";
    CHECK(hc_llm_parse_embeddings(ragged, strlen(ragged), 2, &vecs, &dim) == -1 && !vecs,
          "reject ragged dimensions");
    const char *nonfinite = "{\"data\":[{\"embedding\":[0.1,1e400,0.3]}]}"; /* 1e400 -> Inf */
    CHECK(hc_llm_parse_embeddings(nonfinite, strlen(nonfinite), 1, &vecs, &dim) == -1 && !vecs,
          "reject non-finite cell (no poisoned vector)");
    const char *empty = "{\"data\":[{\"embedding\":[]}]}";
    CHECK(hc_llm_parse_embeddings(empty, strlen(empty), 1, &vecs, &dim) == -1 && !vecs,
          "reject empty embedding");
    CHECK(hc_llm_parse_embeddings("not json", 8, 1, &vecs, &dim) == -1 && !vecs, "reject non-JSON");
}

/* reasoning_effort: the field must be OMITTED unless the value is one the providers actually take.
 * Both halves matter. Emitting it always would change the request body for every existing install --
 * including local OpenAI-compatible servers that reject unknown parameters outright. Forwarding an
 * unrecognised value would turn one typo in a settings box into a 400 on every call. */
static void test_reasoning_effort(void)
{
    hc_llm_message msgs[1] = {{"user", "hi", NULL, NULL}};

    /* absent by default -- the plain builder must be byte-identical to a NULL effort */
    char *plain = hc_llm_build_request_json("m", msgs, 1, NULL, false);
    char *nul = hc_llm_build_request_json_ex("m", msgs, 1, NULL, false, NULL);
    char *empty = hc_llm_build_request_json_ex("m", msgs, 1, NULL, false, "");
    CHECK(plain && nul && strcmp(plain, nul) == 0, "effort: NULL builds the pre-existing body");
    CHECK(empty && nul && strcmp(empty, nul) == 0, "effort: \"\" builds the pre-existing body");
    CHECK(plain && strstr(plain, "reasoning_effort") == NULL, "effort: absent when unset");
    free(plain);
    free(nul);
    free(empty);

    /* every accepted level round-trips into the body */
    static const char *const ok[] = {"none", "minimal", "low", "medium", "high", "xhigh", "max"};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        char *b = hc_llm_build_request_json_ex("m", msgs, 1, NULL, false, ok[i]);
        CHECK(b != NULL, "effort: builder returns a body");
        if (!b) continue;
        hc_json *root = hc_json_parse(b, strlen(b));
        CHECK(root && strcmp(hc_json_get_str(root, "reasoning_effort", ""), ok[i]) == 0,
              "effort: accepted level reaches the body");
        if (root) hc_json_free(root);
        free(b);
    }

    /* anything else is dropped, NOT forwarded -- a typo costs the provider default, not a 400 */
    static const char *const bad[] = {"MEDIUM", "medium ", "hi", "1", "\"}, \"x\":\"y"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char *b = hc_llm_build_request_json_ex("m", msgs, 1, NULL, false, bad[i]);
        CHECK(b != NULL, "effort: an unrecognised level still builds a body");
        if (!b) continue;
        CHECK(strstr(b, "reasoning_effort") == NULL, "effort: unrecognised level is dropped");
        /* and it must not have escaped into the JSON as anything else */
        hc_json *root = hc_json_parse(b, strlen(b));
        CHECK(root != NULL, "effort: body still parses after a hostile level");
        if (root) hc_json_free(root);
        free(b);
    }
}

int main(void)
{
    test_request_builder();
    test_decoder();
    test_usage_sanitize();
    test_embeddings();
    test_reasoning_effort();

    if (g_fails) {
        fprintf(stderr, "hc_llm: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_llm: all checks passed\n");
    return 0;
}
