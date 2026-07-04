/* Offline end-to-end test for hc_agent: a loopback "fake OpenAI" server streams turn 1 (a tool
 * call) then turn 2 (the final answer). The agent must stream turn 1, dispatch the registered
 * tool, feed the result back, and finish on turn 2 — the whole step-2 pipeline (agent -> hc_llm
 * -> hc_http -> SSE -> decode -> dispatch -> loop) with no API key. Exit non-zero on failure. */

#define _DEFAULT_SOURCE 1

#include "hc_agent.h"
#include "hc_http.h"
#include "hc_llm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL: %s\n", (msg));                                           \
            g_fails++;                                                                      \
        }                                                                                   \
    } while (0)

/* turn 1: a tool call to get_time; turn 2: the final assistant text */
static const char *TURN1 =
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_t\","
    "\"function\":{\"name\":\"get_time\",\"arguments\":\"{}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";
static const char *TURN2 =
    "data: {\"choices\":[{\"delta\":{\"content\":\"The time is 12:00\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

struct server {
    int port;
    volatile int ready;
};

static void serve_one(int ls, const char *sse_body)
{
    int cs = accept(ls, NULL, NULL);
    if (cs < 0) return;
    char buf[4096];
    ssize_t r = recv(cs, buf, sizeof buf, 0); /* read (and discard) the request */
    (void)r;
    char head[256];
    int hn = snprintf(head, sizeof head,
                      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
    ssize_t w = send(cs, head, (size_t)hn, 0);
    w = send(cs, sse_body, strlen(sse_body), 0);
    (void)w;
    close(cs);
}

static void *fake_openai(void *arg)
{
    struct server *s = arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        s->ready = -1;
        return NULL;
    }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 2) != 0) {
        close(ls);
        s->ready = -1;
        return NULL;
    }
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    s->port = ntohs(a.sin_port);
    s->ready = 1;

    serve_one(ls, TURN1); /* connection 1: the tool call   */
    serve_one(ls, TURN2); /* connection 2: the final answer */
    close(ls);
    return NULL;
}

/* the registered tool */
static int g_tool_calls = 0;
static char *get_time_tool(const char *args, void *user)
{
    (void)args;
    (void)user;
    g_tool_calls++;
    char *r = malloc(8);
    if (r) memcpy(r, "12:00", 6);
    return r;
}

/* observer */
static char g_final_text[256];
static char g_seen_tool[64];
static void obs_text(const char *delta, size_t n, void *u)
{
    (void)u;
    size_t have = strlen(g_final_text);
    if (have + n < sizeof g_final_text) {
        memcpy(g_final_text + have, delta, n);
        g_final_text[have + n] = '\0';
    }
}
static void obs_tool(const char *name, const char *args, const char *result, void *u)
{
    (void)args;
    (void)result;
    (void)u;
    snprintf(g_seen_tool, sizeof g_seen_tool, "%s", name);
}

/* the per-turn observer (P2 manifest hook): record each completed turn's shape */
static int  g_turns = 0;
static int  g_input_ok = 1;
static char g_turn_finish[3][32];
static char g_turn_assistant[3][128];
static int  g_turn_has_tcs[3];
static int  g_turn_has_results[3];
static void obs_turn(int turn, const char *input, const char *assistant, const char *tool_calls,
                     const char *tool_results, const char *finish, void *u)
{
    (void)u;
    if (turn < 0 || turn >= 3) return;
    if (turn + 1 > g_turns) g_turns = turn + 1;
    if (!input || !input[0]) g_input_ok = 0; /* every turn must carry its input (for the manifest hash) */
    snprintf(g_turn_finish[turn], sizeof g_turn_finish[turn], "%s", finish ? finish : "");
    snprintf(g_turn_assistant[turn], sizeof g_turn_assistant[turn], "%s", assistant ? assistant : "");
    g_turn_has_tcs[turn] = tool_calls && tool_calls[0] && strcmp(tool_calls, "[]") != 0;
    g_turn_has_results[turn] = tool_results && tool_results[0] && strcmp(tool_results, "[]") != 0;
}

int main(void)
{
    if (!hc_http_global_init()) {
        fprintf(stderr, "hc_agent: http init failed\n");
        return 1;
    }

    struct server srv;
    srv.port = 0;
    srv.ready = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, fake_openai, &srv) != 0) {
        CHECK(0, "spawn fake server");
        return 1;
    }
    while (srv.ready == 0) usleep(1000);
    if (srv.ready < 0) {
        CHECK(0, "fake server bound");
        pthread_join(th, NULL);
        return 1;
    }

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", srv.port);

    hc_http *http = hc_http_new();
    hc_llm_provider cfg = {0};
    cfg.name = "fake";
    cfg.base_url = base;
    cfg.model = "test-model";
    hc_llm *llm = hc_llm_new(&cfg, http);
    CHECK(http && llm, "llm/http construct");

    hc_agent *ag = hc_agent_new(llm, "you are a clock");
    CHECK(ag != NULL, "agent construct");

    hc_agent_tool tool = {0};
    tool.name = "get_time";
    tool.spec_json = "{\"type\":\"function\",\"function\":{\"name\":\"get_time\"}}";
    tool.invoke = get_time_tool;
    CHECK(hc_agent_add_tool(ag, &tool), "register tool");

    /* registry growth (Custom Tooling): the registered-tools array grows on demand, so a fresh agent registers
     * well PAST the old fixed cap of 32 (built-ins + any number of third-party proxy tools) with no silent drop. */
    {
        hc_agent  *ga = hc_agent_new(llm, "g");
        static char gnames[40][8];
        int         ok_all = (ga != NULL);
        for (int i = 0; i < 40 && ga; i++) {
            snprintf(gnames[i], sizeof gnames[i], "g%d", i);
            hc_agent_tool t2 = {0};
            t2.name = gnames[i];
            t2.spec_json = "{\"type\":\"function\",\"function\":{\"name\":\"g\"}}";
            t2.invoke = get_time_tool;
            if (!hc_agent_add_tool(ga, &t2)) ok_all = 0;
        }
        CHECK(ok_all, "a fresh agent registers 40 tools past the old 32 cap (growable registry)");
        hc_agent_free(ga);
    }

    hc_agent_observer obs = {.on_text = obs_text, .on_tool = obs_tool, .on_turn = obs_turn, .user = NULL};
    hc_agent_status st = hc_agent_run(ag, "what time is it?", &obs, NULL);
    pthread_join(th, NULL);

    CHECK(st == HC_AGENT_OK, "agent run completes OK");
    CHECK(g_tool_calls == 1, "the tool was invoked exactly once");
    CHECK(strcmp(g_seen_tool, "get_time") == 0, "observer saw the get_time tool");
    CHECK(strcmp(g_final_text, "The time is 12:00") == 0, "final assistant text streamed");
    const char *last = hc_agent_last_text(ag);
    CHECK(last && strcmp(last, "The time is 12:00") == 0, "history records the final answer");
    /* system, user, assistant(tool_call), tool(result), assistant(final) = 5 messages */
    CHECK(hc_agent_message_count(ag) == 5, "history has the expected message count");

    /* the per-turn observer (the P2 manifest hook) saw both turns with the right shape */
    CHECK(g_turns == 2, "on_turn fired once per turn (2)");
    CHECK(g_input_ok, "every turn carried a non-empty input");
    CHECK(strcmp(g_turn_finish[0], "tool_calls") == 0 && g_turn_has_tcs[0] && g_turn_has_results[0]
              && g_turn_assistant[0][0] == '\0',
          "turn 0 = tool-call turn (tool_calls + results, no assistant text, finish=tool_calls)");
    CHECK(strcmp(g_turn_finish[1], "stop") == 0 && !g_turn_has_tcs[1]
              && strcmp(g_turn_assistant[1], "The time is 12:00") == 0,
          "turn 1 = final answer (assistant text, no tools, finish=stop)");

    hc_agent_free(ag);
    hc_llm_free(llm);
    hc_http_free(http);
    hc_http_global_shutdown();

    if (g_fails) {
        fprintf(stderr, "hc_agent: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_agent: all checks passed\n");
    return 0;
}
