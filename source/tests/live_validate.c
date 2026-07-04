/* live_validate — the ONE thing the offline build couldn't: a real streamed turn against a hosted
 * provider. Points hc_llm at OpenRouter (OpenAI-compatible), registers an `add` tool, and runs a
 * single hc_agent turn end to end: stream the assistant, assemble + dispatch a tool call, feed the
 * result back, finish. Validates hc_http (TLS) + hc_llm (SSE decode + tool-call assembly) + hc_agent
 * (the loop) against a live model.
 *
 * The API key is read from OPENROUTER_API_KEY at runtime — never on argv, never printed. NOT a
 * CTest (needs network + a key). Usage: OPENROUTER_API_KEY=... live_validate [model]. */

#include "hc_agent.h"
#include "hc_http.h"
#include "hc_json.h"
#include "hc_llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tool_calls = 0;

/* The registered tool: add two integers from the model-supplied args, return {"sum":N}. */
static char *add_tool(const char *args, void *user)
{
    (void)user;
    g_tool_calls++;
    hc_json *o = hc_json_parse(args, strlen(args));
    long     a = o ? (long)hc_json_get_int(o, "a", 0) : 0;
    long     b = o ? (long)hc_json_get_int(o, "b", 0) : 0;
    if (o) hc_json_free(o);
    fprintf(stderr, "  [tool] add(a=%ld, b=%ld)\n", a, b);
    char *r = (char *)malloc(48);
    if (r) snprintf(r, 48, "{\"sum\":%ld}", a + b);
    return r;
}

static void on_text(const char *delta, size_t n, void *user)
{
    (void)user;
    fwrite(delta, 1, n, stdout); /* the live streamed tokens */
    fflush(stdout);
}

static void on_tool(const char *name, const char *args, const char *result, void *user)
{
    (void)user;
    fprintf(stdout, "\n  [observed tool] %s(%s) -> %s\n", name, args, result);
}

int main(int argc, char **argv)
{
    const char *model = argc > 1 ? argv[1] : "google/gemini-flash-latest";
    const char *key = getenv("OPENROUTER_API_KEY");
    if (!key || !*key) {
        fprintf(stderr, "live_validate: OPENROUTER_API_KEY is not set\n");
        return 2;
    }

    if (!hc_http_global_init()) {
        fprintf(stderr, "live_validate: hc_http_global_init failed\n");
        return 1;
    }
    hc_http *http = hc_http_new();

    const char     *hdrs[] = {"HTTP-Referer: https://hypercat.local", "X-Title: HyperCat", NULL};
    hc_llm_provider cfg = {0};
    cfg.name = "openrouter";
    cfg.base_url = "https://openrouter.ai/api/v1";
    cfg.api_key = key; /* held in hc_llm, zeroized on free; never logged */
    cfg.model = model;
    cfg.probe_host = "openrouter.ai";
    cfg.probe_port = "443";
    cfg.extra_headers = hdrs;
    hc_llm *llm = hc_llm_new(&cfg, http);
    if (!http || !llm) {
        fprintf(stderr, "live_validate: llm/http construct failed\n");
        return 1;
    }

    fprintf(stderr, "live_validate: probing openrouter.ai:443 ...\n");
    hc_http_net_state ns = hc_llm_probe(llm, 5000);
    fprintf(stderr, "live_validate: net_state = %d (0=ONLINE)\n", (int)ns);

    hc_agent *ag = hc_agent_new(
        llm, "You are precise. For arithmetic, call the add tool, then state the result plainly.");
    hc_agent_tool tool = {0};
    tool.name = "add";
    tool.spec_json =
        "{\"type\":\"function\",\"function\":{\"name\":\"add\",\"description\":\"Add two "
        "integers\",\"parameters\":{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},"
        "\"b\":{\"type\":\"integer\"}},\"required\":[\"a\",\"b\"]}}}";
    tool.invoke = add_tool;
    if (!hc_agent_add_tool(ag, &tool)) fprintf(stderr, "live_validate: add_tool register failed\n");

    fprintf(stderr, "live_validate: running a turn against '%s' ...\n\n--- streamed ---\n", model);
    hc_agent_observer obs = {.on_text = on_text, .on_tool = on_tool, .on_turn = NULL, .user = NULL};
    hc_agent_status   st =
        hc_agent_run(ag, "What is 21 + 21? Use the add tool, then tell me the answer.", &obs, NULL);

    const char *last = hc_agent_last_text(ag);
    printf("\n\n--- result ---\n");
    printf("status       : %s\n", hc_agent_status_str(st));
    printf("tool calls   : %d\n", g_tool_calls);
    printf("final answer : %s\n", last ? last : "(none)");
    printf("history msgs : %zu\n", hc_agent_message_count(ag));

    hc_agent_free(ag);
    hc_llm_free(llm);
    hc_http_free(http);
    hc_http_global_shutdown();
    return st == HC_AGENT_OK ? 0 : 1;
}
