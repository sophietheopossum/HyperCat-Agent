/* Tests for hc_transport — framing over socketpair (round-trips, empty, ordered, 1 MiB, send- and
 * recv-side frame caps, peer-credential auth, closed detection) plus a real UDS echo round-trip
 * via a listener thread. Offline. Exit non-zero on any failure (CTest reads the exit code). */

#define _DEFAULT_SOURCE 1

#include "hc_transport.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL: %s\n", (msg));                                           \
            g_fails++;                                                                      \
        }                                                                                   \
    } while (0)

static int send_str(hc_transport *t, const char *s)
{
    return hc_transport_send(t, s, strlen(s)) == HC_TRANSPORT_OK;
}

static int recv_eq(hc_transport *t, const char *expect)
{
    void *buf = NULL;
    size_t n = 0;
    if (hc_transport_recv(t, &buf, &n) != HC_TRANSPORT_OK) return 0;
    int ok = (n == strlen(expect)) && (n == 0 || memcmp(buf, expect, n) == 0);
    free(buf);
    return ok;
}

struct uds_ctx {
    char       path[256];
    atomic_int ready; /* 0 pending, 1 listening, -1 failed; atomic so the handshake is race-free */
};

static void *uds_server(void *arg)
{
    struct uds_ctx *c = (struct uds_ctx *)arg;
    hc_transport_listener *l = hc_transport_listen(c->path);
    if (!l) {
        atomic_store(&c->ready, -1);
        return NULL;
    }
    atomic_store(&c->ready, 1);
    hc_transport *ep = hc_transport_accept(l);
    if (ep) {
        void *buf = NULL;
        size_t n = 0;
        while (hc_transport_recv(ep, &buf, &n) == HC_TRANSPORT_OK) {
            hc_transport_send(ep, buf, n); /* echo every frame until the client closes */
            free(buf);
            buf = NULL;
        }
        free(buf);
        hc_transport_close(ep);
    }
    hc_transport_listener_close(l);
    return NULL;
}

int main(void)
{
    hc_transport *a = NULL, *b = NULL;

    /* --- socketpair framing --- */
    CHECK(hc_transport_pair(&a, &b), "create pair");
    if (!a || !b) return 1;

    CHECK(send_str(a, "hello"), "send a->b");
    CHECK(recv_eq(b, "hello"), "recv 'hello' on b");
    CHECK(send_str(b, "world") && recv_eq(a, "world"), "b->a round-trip");

    CHECK(hc_transport_send(a, "", 0) == HC_TRANSPORT_OK, "send empty frame");
    {
        void *buf = NULL;
        size_t n = 1;
        CHECK(hc_transport_recv(b, &buf, &n) == HC_TRANSPORT_OK && n == 0, "recv empty frame (len 0)");
        free(buf);
    }

    send_str(a, "m1");
    send_str(a, "m2");
    send_str(a, "m3");
    CHECK(recv_eq(b, "m1") && recv_eq(b, "m2") && recv_eq(b, "m3"), "ordered messages preserved");

    /* NB: a large frame is exercised over the UDS connection below, where the server thread
     * reads concurrently — sending a buffer larger than the socket buffer on the SAME thread that
     * must also receive it would deadlock. */

    hc_transport_set_max_frame(a, 8);
    {
        char ten[10];
        memset(ten, 'y', sizeof ten);
        CHECK(hc_transport_send(a, ten, sizeof ten) == HC_TRANSPORT_ERR_TOO_LARGE,
              "send-side frame cap rejects oversize");
    }

    {
        long uid = -1, pid = -1;
        CHECK(hc_transport_peer_cred(b, &uid, &pid) && uid == (long)getuid(),
              "peer-credential uid matches this process");
    }
    hc_transport_close(a);
    hc_transport_close(b);

    /* --- recv-side frame cap (fresh pair) --- */
    a = b = NULL;
    CHECK(hc_transport_pair(&a, &b), "create pair (cap test)");
    if (a && b) {
        hc_transport_set_max_frame(b, 8);
        char big2[100];
        memset(big2, 'z', sizeof big2);
        CHECK(hc_transport_send(a, big2, sizeof big2) == HC_TRANSPORT_OK, "send 100 (sender cap default)");
        void *buf = NULL;
        size_t n = 0;
        CHECK(hc_transport_recv(b, &buf, &n) == HC_TRANSPORT_ERR_TOO_LARGE,
              "recv-side frame cap rejects oversize");
        free(buf);
        hc_transport_close(a);
        hc_transport_close(b);
    }

    /* --- closed detection --- */
    a = b = NULL;
    CHECK(hc_transport_pair(&a, &b), "create pair (close test)");
    if (a && b) {
        hc_transport_close(a);
        void *buf = NULL;
        size_t n = 0;
        CHECK(hc_transport_recv(b, &buf, &n) == HC_TRANSPORT_ERR_CLOSED,
              "recv after peer close -> CLOSED");
        free(buf);
        hc_transport_close(b);
    }

    /* --- recv timeout (SO_RCVTIMEO): a deadline elapses with no data -> TIMEOUT, and because the
     *     timeout lands at a frame boundary the connection is still usable afterwards --- */
    a = b = NULL;
    CHECK(hc_transport_pair(&a, &b), "create pair (timeout test)");
    if (a && b) {
        hc_transport_set_recv_timeout(b, 100); /* 100 ms */
        void *buf = NULL;
        size_t n = 0;
        CHECK(hc_transport_recv(b, &buf, &n) == HC_TRANSPORT_ERR_TIMEOUT,
              "recv with no data -> TIMEOUT after the deadline");
        free(buf);
        hc_transport_set_recv_timeout(b, 0); /* restore blocking */
        CHECK(send_str(a, "after") && recv_eq(b, "after"),
              "connection still usable after a frame-boundary timeout");
        hc_transport_close(a);
        hc_transport_close(b);
    }

    /* --- send to a closed peer returns CLOSED, never a SIGPIPE process-kill --- */
    a = b = NULL;
    CHECK(hc_transport_pair(&a, &b), "create pair (SIGPIPE test)");
    if (a && b) {
        hc_transport_close(b);
        hc_transport_status st = HC_TRANSPORT_OK;
        for (int i = 0; i < 2000 && st == HC_TRANSPORT_OK; i++)
            st = hc_transport_send(a, "x", 1); /* fill the buffer, then hit the closed peer */
        CHECK(st == HC_TRANSPORT_ERR_CLOSED, "send to a closed peer -> CLOSED (no SIGPIPE)");
        hc_transport_close(a);
    }

    /* --- real UDS echo round-trip --- */
    struct uds_ctx c;
    atomic_init(&c.ready, 0);
    snprintf(c.path, sizeof c.path, "/tmp/hc_transport_%ld.sock", (long)getpid());
    pthread_t th;
    if (pthread_create(&th, NULL, uds_server, &c) == 0) {
        while (atomic_load(&c.ready) == 0) usleep(1000);
        if (atomic_load(&c.ready) > 0) {
            hc_transport *cli = hc_transport_connect(c.path);
            CHECK(cli != NULL, "UDS connect");
            if (cli) {
                CHECK(send_str(cli, "ping") && recv_eq(cli, "ping"), "UDS echo round-trip");
                /* large frame: the server thread reads concurrently, so this cannot deadlock */
                size_t N = 1u << 20; /* 1 MiB */
                char *big = (char *)malloc(N);
                if (big) {
                    memset(big, 'Z', N);
                    int sent = hc_transport_send(cli, big, N) == HC_TRANSPORT_OK;
                    void *buf = NULL;
                    size_t n = 0;
                    int got = hc_transport_recv(cli, &buf, &n) == HC_TRANSPORT_OK && n == N
                              && memcmp(buf, big, N) == 0;
                    CHECK(sent && got, "UDS large 1MiB frame round-trips intact");
                    free(buf);
                    free(big);
                }
                long uid = -1, pid = -1;
                CHECK(hc_transport_peer_cred(cli, &uid, &pid) && uid == (long)getuid(),
                      "UDS peer-credential uid");
                hc_transport_close(cli);
            }
        } else {
            CHECK(0, "UDS listener bound");
        }
        pthread_join(th, NULL);
    }

    if (g_fails) {
        fprintf(stderr, "hc_transport: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_transport: all checks passed\n");
    return 0;
}
