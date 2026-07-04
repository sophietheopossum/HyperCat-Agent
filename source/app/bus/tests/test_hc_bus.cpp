/* Tests for hc_bus — a broker plus clients in one process (UDS): the codec round-trip, request/
 * reply routing, publish/subscribe, crash-isolation (a client dies; the broker and others
 * survive), and backpressure (a stuck consumer's queue fills -> the sender gets an err). The
 * broker's own threads do the routing; the single test thread orchestrates sequentially. Offline.
 * Exit non-zero on any failure. */

#include "hc_bus.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

using hc::Broker;
using hc::BusClient;
using hc::Message;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                                      \
            g_fails++;                                                                      \
        }                                                                                   \
    } while (0)

int main()
{
    /* --- codec round-trip --- */
    {
        Message m;
        m.type = "req";
        m.from = "a";
        m.to = "b";
        m.corr = 42;
        m.body = "{\"x\":1}";
        std::string enc = m.encode();
        Message d;
        CHECK(Message::decode(enc.data(), enc.size(), d), "decode");
        CHECK(d.type == "req" && d.from == "a" && d.to == "b" && d.corr == 42 && d.body == m.body,
              "codec round-trip");
    }

    char path[128];
    std::snprintf(path, sizeof path, "/tmp/hc_bus_%ld.sock", (long)getpid());

    Broker *broker = Broker::start(path, -1);
    CHECK(broker != nullptr, "broker start");
    if (!broker) return 1;

    BusClient *A = BusClient::connect(path, "agent:A");
    BusClient *B = BusClient::connect(path, "agent:B");
    CHECK(A && B, "clients connect + register");
    if (!A || !B) {
        broker->stop();
        return 1;
    }

    /* --- dup-id rejection: a second client cannot steal an already-registered id. The broker
     * refuses the hello (err, not welcome) so connect() returns null; the real agent:A keeps its
     * registration, which the pub/sub + req/reply tests below implicitly confirm still works. */
    {
        BusClient *imposter = BusClient::connect(path, "agent:A");
        CHECK(imposter == nullptr, "broker refuses a duplicate id (no id hijack)");
        delete imposter; /* delete nullptr is a no-op */
    }

    /* --- publish / subscribe --- */
    CHECK(A->subscribe("topic1"), "A subscribes topic1");
    CHECK(B->publish("topic1", "hi-A"), "B publishes topic1");
    {
        Message m;
        CHECK(A->recv(m) && m.type == "pub" && m.topic == "topic1" && m.body == "hi-A",
              "A receives the published message");
    }

    /* --- request / reply routing --- */
    CHECK(A->send_request("agent:B", 7, "ping"), "A -> B request");
    {
        Message m;
        CHECK(B->recv(m) && m.type == "req" && m.from == "agent:A" && m.corr == 7 && m.body == "ping",
              "B receives the request");
        CHECK(B->send_reply("agent:A", 7, "pong"), "B -> A reply");
    }
    {
        Message m;
        CHECK(A->recv(m) && m.type == "reply" && m.corr == 7 && m.body == "pong",
              "A receives the reply");
    }

    /* --- crash-isolation: B dies; the broker and A keep working --- */
    delete B; /* closes B's connection — the broker tears down only that conn */
    B = nullptr;
    CHECK(A->send_request("agent:ghost", 9, "x"), "A sends to a non-existent endpoint");
    {
        Message m;
        CHECK(A->recv(m) && m.type == "err" && m.corr == 9 && m.body == "no such endpoint",
              "broker still routes for A after B died (crash-isolation)");
    }

    /* --- connection churn: many clean connect/disconnect cycles exercise the reaper (join the
     * finished threads + drop the conn from the table) under ASan/TSan; the broker must keep
     * serving afterwards. Unique ids avoid the async-teardown id-reuse race. --- */
    {
        for (int i = 0; i < 100; i++) {
            char cid[40];
            std::snprintf(cid, sizeof cid, "agent:churn%d", i);
            BusClient *C = BusClient::connect(path, cid);
            CHECK(C != nullptr, "churn client connects");
            delete C; /* clean disconnect -> reaper joins its threads + drops it from conns */
        }
        CHECK(A->send_request("agent:ghost", 11, "x"), "A still routes after churn");
        Message m;
        CHECK(A->recv(m) && m.type == "err" && m.corr == 11 && m.body == "no such endpoint",
              "broker healthy after 100 connect/disconnect cycles");
    }

    /* --- id<->pid authorization (the same-uid id-squat closer). Every in-process client's peer pid
     * is THIS process's pid, so authorizing an id to getpid() accepts and to getpid()+1 refuses. --- */
    {
        long me = (long)getpid();

        /* positive: an id authorized to OUR pid registers (works on every platform — where no peer
         * pid is available the gate is simply skipped). */
        broker->authorize_id("agent:ok", me);
        BusClient *ok = BusClient::connect(path, "agent:ok");
        CHECK(ok != nullptr, "authorized id from the matching pid registers");
        delete ok;
        broker->revoke_id("agent:ok");

#if defined(__linux__) /* the pid GATE needs a peer pid (SO_PEERCRED); macOS keeps the dup-id floor */
        /* negative: an id authorized to a DIFFERENT pid refuses our hello. */
        broker->authorize_id("agent:no", me + 1);
        BusClient *no = BusClient::connect(path, "agent:no");
        CHECK(no == nullptr, "authorized id from a non-matching pid is refused (id-squat closed)");
        delete no;

        broker->revoke_id("agent:no"); /* clearing the binding restores the dup-refusal floor */
        BusClient *no2 = BusClient::connect(path, "agent:no");
        CHECK(no2 != nullptr, "after revoke, the id registers again");
        delete no2;

        /* eviction: a squatter that grabbed an un-authorized id is kicked when authorization lands. */
        BusClient *squat = BusClient::connect(path, "agent:ev");
        CHECK(squat != nullptr, "squatter registers an un-authorized id first");
        broker->authorize_id("agent:ev", me + 1); /* foreign pid -> evict the squatter */
        {
            Message m;
            CHECK(squat && !squat->recv(m), "the evicted squatter's connection is dropped");
        }
        BusClient *reclaim = BusClient::connect(path, "agent:ev");
        CHECK(reclaim == nullptr, "the evicted id (now bound to a foreign pid) refuses re-registration");
        delete reclaim;
        delete squat;
        broker->revoke_id("agent:ev");
#endif
    }

    /* --- token-gated routing: a supervisor-AUTHORIZED but UNCONFIRMED worker id may only complete the
     * check-in handshake with "host"; its task req/reply is withheld until confirm_id. Generic
     * (unauthorized) ids route freely — the gate protects only supervisor-managed ids. (Portable:
     * confirmation is not pid-dependent, so it runs on every platform.) --- */
    {
        long me = (long)getpid();
        broker->authorize_id("host", me);
        broker->confirm_id("host");
        broker->authorize_id("orchestrator", me);
        broker->confirm_id("orchestrator");
        BusClient *H = BusClient::connect(path, "host");
        BusClient *O = BusClient::connect(path, "orchestrator");
        CHECK(H && O, "host + orchestrator connect (confirmed)");

        broker->authorize_id("agent:W", me); /* authorized by the supervisor, but NOT yet confirmed */
        BusClient *W = BusClient::connect(path, "agent:W");
        CHECK(W != nullptr, "authorized-but-unconfirmed worker connects");

        /* the check-in channel to "host" is open even while W is unconfirmed */
        CHECK(W->send_request("host", 1, "{\"cmd\":\"checkin\"}"), "unconfirmed -> host sends");
        {
            Message m;
            CHECK(H && H->recv(m) && m.from == "agent:W" && m.corr == 1,
                  "host receives the unconfirmed worker's check-in (channel open)");
        }

        /* but a req to a non-host endpoint is WITHHELD until W is confirmed */
        CHECK(W->send_request("orchestrator", 2, "{\"cmd\":\"task.result\"}"), "withheld req queues");
        {
            Message m;
            CHECK(W && W->recv(m) && m.type == "err" && m.corr == 2 && m.body == "not confirmed",
                  "an authorized-but-unconfirmed worker's task req is withheld");
        }
        /* sub is also withheld for an unconfirmed managed id (it must not steal a peer's stream) */
        CHECK(!W->subscribe("agent:peer/tokens"), "unconfirmed worker's subscribe is withheld");

        /* confirm W -> it now routes to a confirmed endpoint */
        broker->confirm_id("agent:W");
        CHECK(W->send_request("orchestrator", 3, "{\"cmd\":\"task.result\"}"), "confirmed req sends");
        {
            Message m;
            CHECK(O && O->recv(m) && m.from == "agent:W" && m.corr == 3,
                  "a confirmed worker's task req routes to the orchestrator");
        }

        /* M2: a fresh authorize_id (a respawn rebind) clears confirmation — the resurrected id must
         * re-check-in; it does NOT inherit the dead instance's routing-confirmed status. */
        broker->authorize_id("agent:W", me);
        CHECK(W->send_request("orchestrator", 5, "{\"cmd\":\"task.result\"}"), "post-rebind queues");
        {
            Message m;
            CHECK(W && W->recv(m) && m.type == "err" && m.corr == 5 && m.body == "not confirmed",
                  "re-authorize (respawn) clears confirmation: routing withheld until re-confirm");
        }

        delete W;
        delete O;
        delete H;
        broker->revoke_id("agent:W");
        broker->revoke_id("orchestrator");
        broker->revoke_id("host");
    }

    /* --- backpressure: a stuck consumer's queue fills -> sender gets an err --- */
    BusClient *D = BusClient::connect(path, "agent:D"); /* D registers, then never reads again */
    CHECK(D != nullptr, "stuck consumer D connects");
    if (D) {
        std::string kb(16384, 'x'); /* 16 KiB bodies; a stuck consumer's queue fills fast */
        for (int i = 0; i < 2000; i++) A->send_request("agent:D", (uint64_t)i, kb);
        Message m;
        CHECK(A->recv(m) && m.type == "err" && m.body == "backpressure",
              "sender receives a backpressure err when the consumer is stuck");
        delete D;
    }

    delete A;
    broker->stop();
    delete broker;

    if (g_fails) {
        std::fprintf(stderr, "hc_bus: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("hc_bus: all checks passed\n");
    return 0;
}
