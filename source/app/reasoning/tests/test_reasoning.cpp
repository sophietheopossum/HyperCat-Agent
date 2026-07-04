/* Offline test for hc_reasoning: a loopback "fake OpenAI" server streams five canned stage
 * completions (Decompose..Reflect), one per connection. Asserts the 5-stage chain runs in order,
 * assembles the result, and — the doc-11 invariant — that an early stage's graded claim is THREADED
 * into later stages (grade-carry): stage 1's marker appears in the stage-2 and stage-5 requests.
 * No API key, no network beyond loopback. Exit non-zero on any failure. */

#include "hc_reasoning.hpp"

#include "hc_http.h"
#include "hc_llm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(c, m)                                                                            \
    do {                                                                                       \
        if (!(c)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                           \
            g_fails++;                                                                         \
        }                                                                                      \
    } while (0)

/* one SSE completion carrying `content` as a single delta */
static std::string sse(const std::string &content)
{
    return "data: {\"choices\":[{\"delta\":{\"content\":\"" + content + "\"}}]}\n\n"
           "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
           "data: [DONE]\n\n";
}

struct Server {
    int         port = 0;
    int         ready = 0; /* 0 pending, 1 listening, -1 failed; read after join (no race) */
    std::string responses[5];
    std::string requests[5]; /* captured request bytes, per connection */
};

/* Read an HTTP request: headers through \r\n\r\n, honor Expect: 100-continue, then Content-Length
 * body bytes. Robust enough that the captured body is complete for the threading assertion. */
static std::string read_request(int cs)
{
    std::string buf;
    char        tmp[2048];
    size_t      hdr_end = std::string::npos;
    while (hdr_end == std::string::npos) {
        ssize_t r = recv(cs, tmp, sizeof tmp, 0);
        if (r <= 0) return buf;
        buf.append(tmp, (size_t)r);
        hdr_end = buf.find("\r\n\r\n");
        if (buf.size() > 1u << 20) break; /* guard */
    }
    if (hdr_end == std::string::npos) return buf;
    if (buf.find("Expect: 100-continue") != std::string::npos) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        ssize_t     w = send(cs, cont, std::strlen(cont), 0);
        (void)w;
    }
    long   clen = 0;
    size_t cl = buf.find("Content-Length:");
    if (cl != std::string::npos) clen = std::atol(buf.c_str() + cl + 15);
    size_t body_have = buf.size() - (hdr_end + 4);
    while ((long)body_have < clen) {
        ssize_t r = recv(cs, tmp, sizeof tmp, 0);
        if (r <= 0) break;
        buf.append(tmp, (size_t)r);
        body_have += (size_t)r;
    }
    return buf;
}

static void *fake_openai(void *arg)
{
    Server *s = (Server *)arg;
    int     ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        s->ready = -1;
        return nullptr;
    }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 5) != 0) {
        close(ls);
        s->ready = -1;
        return nullptr;
    }
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    s->port = ntohs(a.sin_port);
    s->ready = 1;

    for (int i = 0; i < 5; i++) {
        int cs = accept(ls, nullptr, nullptr);
        if (cs < 0) break;
        s->requests[i] = read_request(cs);
        char head[160];
        int  hn = std::snprintf(head, sizeof head,
                                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                 "Connection: close\r\n\r\n");
        ssize_t w = send(cs, head, (size_t)hn, 0);
        w = send(cs, s->responses[i].data(), s->responses[i].size(), 0);
        (void)w;
        close(cs);
    }
    close(ls);
    return nullptr;
}

int main()
{
    if (!hc_http_global_init()) {
        std::fprintf(stderr, "reasoning: http init failed\n");
        return 1;
    }

    Server srv;
    srv.responses[0] = sse("decompose [Fact] CARRYME alpha"); /* stage-1 graded claim to carry */
    srv.responses[1] = sse("analyze beta");
    srv.responses[2] = sse("critique gamma");
    srv.responses[3] = sse("ANSWER: forty-two");
    srv.responses[4] = sse("reflect: solid; confidence high");

    pthread_t th;
    if (pthread_create(&th, nullptr, fake_openai, &srv) != 0) {
        std::fprintf(stderr, "reasoning: spawn server failed\n");
        hc_http_global_shutdown();
        return 1;
    }
    while (srv.ready == 0) usleep(1000);
    if (srv.ready < 0) {
        pthread_join(th, nullptr);
        hc_http_global_shutdown();
        CHECK(0, "fake server bound");
        return 1;
    }

    char base[64];
    std::snprintf(base, sizeof base, "http://127.0.0.1:%d", srv.port);
    hc_http         *http = hc_http_new();
    hc_llm_provider  cfg = {};
    cfg.name = "fake";
    cfg.base_url = base;
    cfg.model = "test-model";
    hc_llm *llm = hc_llm_new(&cfg, http);
    CHECK(http && llm, "llm/http construct");

    hc::Reasoner *r = hc::Reasoner::create(llm);
    CHECK(r != nullptr, "reasoner construct");

    hc::ReasonResult res;
    if (r) res = r->reason("Is the answer forty-two?");
    pthread_join(th, nullptr); /* all five connections served; requests[] safe to read */

    CHECK(res.complete, "chain completed all five stages");
    CHECK(res.decompose.find("decompose") != std::string::npos, "stage 1 captured");
    CHECK(res.analyze.find("analyze") != std::string::npos, "stage 2 captured");
    CHECK(res.critique.find("critique") != std::string::npos, "stage 3 captured");
    CHECK(res.synthesize.find("ANSWER: forty-two") != std::string::npos, "stage 4 captured");
    CHECK(res.reflect.find("reflect") != std::string::npos, "stage 5 captured");
    CHECK(res.answer == res.synthesize, "answer == synthesize output");
    CHECK(res.confidence == res.reflect, "confidence == reflect output");

    /* grade-carry threading: stage 1's [Fact] claim must reach later stages' prompts */
    CHECK(srv.requests[0].find("CARRYME") == std::string::npos,
          "stage-1 request has no prior context (it is the first stage)");
    CHECK(srv.requests[1].find("CARRYME") != std::string::npos,
          "stage-2 request carries stage-1's graded claim");
    CHECK(srv.requests[4].find("CARRYME") != std::string::npos,
          "stage-5 (reflect) request still carries stage-1's graded claim (grade-carry)");
    CHECK(srv.requests[3].find("CRITIQUE") != std::string::npos,
          "stage-4 (synthesize) request carries the prior CRITIQUE stage section");

    if (r) delete r;
    hc_llm_free(llm);
    hc_http_free(http);
    hc_http_global_shutdown();

    if (g_fails) {
        std::fprintf(stderr, "reasoning: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("reasoning: all checks passed\n");
    return 0;
}
