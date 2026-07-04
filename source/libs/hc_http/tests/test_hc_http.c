/* Tests for hc_http. Fully offline: the SSE parser is fed byte fixtures directly; the net probe
 * and the live HTTP/guard paths run against a loopback responder thread on 127.0.0.1. Exit
 * non-zero on any failure (CTest reads the exit code). */

#define _DEFAULT_SOURCE 1

#include "hc_http.h"
#include "hc_http_sse.h"

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

/* ---------- SSE parser fixtures ---------- */

struct rec {
    char ev[64];
    char data[256];
    int  is_done;
};
struct recset {
    struct rec items[64];
    int n;
};

static bool rec_emit(const hc_http_sse_event *e, void *u)
{
    struct recset *rs = u;
    if (rs->n < 64) {
        struct rec *r = &rs->items[rs->n++];
        snprintf(r->ev, sizeof r->ev, "%s", e->event ? e->event : "");
        snprintf(r->data, sizeof r->data, "%.*s", (int)e->data_len, e->data ? e->data : "");
        r->is_done = e->is_done ? 1 : 0;
    }
    return true;
}

/* Feed `text` in `chunk` byte slices (chunk<=0 means all at once), then finish. */
static void parse_sse(const char *text, int chunk, struct recset *rs)
{
    rs->n = 0;
    hc_http_sse *p = hc_http_sse_new(rec_emit, rs);
    if (!p) return;
    size_t len = strlen(text);
    if (chunk <= 0) {
        hc_http_sse_feed(p, text, len);
    } else {
        for (size_t i = 0; i < len; i += (size_t)chunk) {
            size_t n = (size_t)chunk;
            if (i + n > len) n = len - i;
            hc_http_sse_feed(p, text + i, n);
        }
    }
    hc_http_sse_finish(p);
    hc_http_sse_free(p);
}

static void test_sse(void)
{
    struct recset rs;

    parse_sse("data: hello\n\n", 0, &rs);
    CHECK(rs.n == 1 && strcmp(rs.items[0].data, "hello") == 0, "sse: single LF event");

    parse_sse("data: hello\r\n\r\n", 0, &rs);
    CHECK(rs.n == 1 && strcmp(rs.items[0].data, "hello") == 0, "sse: CRLF framing");

    parse_sse("data: a\ndata: b\n\n", 0, &rs);
    CHECK(rs.n == 1 && strcmp(rs.items[0].data, "a\nb") == 0, "sse: multi-data joined with LF");

    parse_sse(": keep-alive\n\ndata: x\n\n", 0, &rs);
    CHECK(rs.n == 1 && strcmp(rs.items[0].data, "x") == 0, "sse: comment line ignored");

    parse_sse("data:no-space\n\n", 0, &rs);
    CHECK(rs.n == 1 && strcmp(rs.items[0].data, "no-space") == 0, "sse: no leading space");

    parse_sse("data: [DONE]\n\n", 0, &rs);
    CHECK(rs.n == 1 && rs.items[0].is_done, "sse: [DONE] sentinel flagged");

    parse_sse("event: tool\nid: 7\ndata: {}\n\n", 0, &rs);
    CHECK(rs.n == 1 && strcmp(rs.items[0].ev, "tool") == 0 && strcmp(rs.items[0].data, "{}") == 0,
          "sse: event/id/data fields");

    /* partial frame split across feeds, plus a realistic [DONE]-terminated stream byte-by-byte */
    parse_sse("data: hel", 0, &rs); /* unterminated alone -> finish flushes "hel" */
    CHECK(rs.n == 1 && strcmp(rs.items[0].data, "hel") == 0, "sse: finish flushes dangling event");

    parse_sse("data: {\"a\":1}\n\ndata: {\"a\":2}\n\ndata: [DONE]\n\n", 1, &rs);
    CHECK(rs.n == 3 && strcmp(rs.items[0].data, "{\"a\":1}") == 0 && rs.items[2].is_done,
          "sse: chunked stream parsed byte-by-byte");
}

/* ---------- loopback HTTP responder ---------- */

struct responder {
    int port;
    volatile int ready;
};

static void *http_responder(void *arg)
{
    struct responder *r = arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        r->ready = -1;
        return NULL;
    }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0) {
        close(ls);
        r->ready = -1;
        return NULL;
    }
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    r->port = ntohs(a.sin_port);
    r->ready = 1;

    int cs = accept(ls, NULL, NULL);
    if (cs >= 0) {
        char buf[1024];
        ssize_t rd = recv(cs, buf, sizeof buf, 0);
        (void)rd;
        const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
        ssize_t w = send(cs, resp, strlen(resp), 0);
        (void)w;
        close(cs);
    }
    close(ls);
    return NULL;
}

/* ---------- connection guard ---------- */

static char g_seen_ip[64];
static bool guard_allow(const hc_http_peer *p, void *u)
{
    (void)u;
    snprintf(g_seen_ip, sizeof g_seen_ip, "%s", p->ip ? p->ip : "");
    return true;
}
static bool guard_deny(const hc_http_peer *p, void *u)
{
    (void)p;
    (void)u;
    return false;
}

static void test_http_loopback(void)
{
    struct responder r;
    r.port = 0;
    r.ready = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, http_responder, &r) != 0) {
        CHECK(0, "spawn loopback responder");
        return;
    }
    while (r.ready == 0) usleep(1000);
    if (r.ready < 0) {
        CHECK(0, "loopback responder bound");
        pthread_join(th, NULL);
        return;
    }

    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", r.port);

    hc_http *h = hc_http_new();
    CHECK(h != NULL, "hc_http_new");
    if (!h) {
        pthread_join(th, NULL);
        return;
    }

    /* allow path: the guard sees the resolved loopback IP and the GET round-trips */
    g_seen_ip[0] = '\0';
    hc_http_set_guard(h, guard_allow, NULL);
    hc_http_response resp;
    hc_http_status st = hc_http_get(h, url, NULL, &resp);
    CHECK(st == HC_HTTP_OK, "GET loopback returns OK");
    if (st == HC_HTTP_OK) {
        CHECK(resp.status == 200, "GET status 200");
        CHECK(resp.body && strcmp(resp.body, "hello") == 0, "GET body matches");
        hc_http_response_free(&resp);
    }
    CHECK(strcmp(g_seen_ip, "127.0.0.1") == 0, "guard saw the resolved loopback IP");
    pthread_join(th, NULL);

    /* deny path: the guard refuses before connect -> HC_HTTP_ERR_DENIED (no listener needed) */
    hc_http_set_guard(h, guard_deny, NULL);
    char deadurl[64];
    snprintf(deadurl, sizeof deadurl, "http://127.0.0.1:%d/", r.port);
    hc_http_response resp2;
    hc_http_status st2 = hc_http_get(h, deadurl, NULL, &resp2);
    CHECK(st2 == HC_HTTP_ERR_DENIED, "guard deny yields HC_HTTP_ERR_DENIED");
    if (st2 == HC_HTTP_OK) hc_http_response_free(&resp2);

    hc_http_free(h);
}

static void test_probe(void)
{
    /* ONLINE: a loopback listener we never accept on is still connectable */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(ls, (struct sockaddr *)&a, sizeof a);
    listen(ls, 1);
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    char port[16];
    snprintf(port, sizeof port, "%d", ntohs(a.sin_port));
    CHECK(hc_http_net_probe("127.0.0.1", port, 1000) == HC_HTTP_NET_ONLINE,
          "probe ONLINE to loopback listener");
    close(ls);

    /* NO_ROUTE: connect refused on a now-closed loopback port */
    CHECK(hc_http_net_probe("127.0.0.1", port, 500) == HC_HTTP_NET_NO_ROUTE,
          "probe NO_ROUTE to closed loopback port");

    /* NO_DNS: the reserved .invalid TLD never resolves (RFC 6761) */
    CHECK(hc_http_net_probe("nonexistent.invalid", "80", 500) == HC_HTTP_NET_NO_DNS,
          "probe NO_DNS for an unresolvable host");
}

int main(void)
{
    if (!hc_http_global_init()) {
        fprintf(stderr, "hc_http: global init failed\n");
        return 1;
    }

    test_sse();
    test_probe();
    test_http_loopback();

    hc_http_global_shutdown();

    if (g_fails) {
        fprintf(stderr, "hc_http: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_http: all checks passed\n");
    return 0;
}
