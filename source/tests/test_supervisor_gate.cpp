/* Step-4 (IPC) gate — integration test. The supervisor spawns real agentd worker PROCESSES that
 * exchange messages over the bus; killing one mid-flight must NOT take down the broker or the
 * other worker (crash-isolation), and the supervisor must reap and (when enabled) respawn it. Phase C
 * proves the DELIBERATE reap() retires a worker WITHOUT respawn (even with autorestart on), frees the id,
 * and the freed id re-authorizes + routes again on a fresh spawn (the bus-id revoke is reversible).
 * Fully offline: no LLM, no network — the workers run the bus harness (ping / pingpeer / shutdown).
 *
 * AGENTD_PATH is injected by CMake ($<TARGET_FILE:agentd>). Exit non-zero on any failure. */

#include "hc_bus.hpp"
#include "hc_json.h"
#include "hc_supervisor.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include <unistd.h>

using hc::Broker;
using hc::BusClient;
using hc::Message;
using hc::Supervisor;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                       \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                                         \
            g_fails++;                                                                         \
        }                                                                                      \
    } while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

/* Poll pred() until true or the budget elapses; returns pred's final value. */
template <typename F> static bool wait_until(F pred, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        if (pred()) return true;
        sleep_ms(20);
    }
    return pred();
}

/* Send a control req and wait for the matching-corr reply; true iff the reply body is {"ok":true}.
 * A broker err (no endpoint) or a disconnect yields false. */
static bool send_cmd(BusClient &t, const std::string &to, uint64_t corr, const std::string &body)
{
    if (!t.send_request(to, corr, body)) return false;
    for (;;) {
        Message r;
        if (!t.recv(r)) return false;
        if (r.corr != corr) continue;
        if (r.type != "reply") return false; /* e.g. "err": no such endpoint */
        hc_json *o = hc_json_parse(r.body.data(), r.body.size());
        bool ok = o && hc_json_get_bool(o, "ok", false);
        if (o) hc_json_free(o);
        return ok;
    }
}

int main()
{
    /* Host-private 0700 dir for the bus socket (the supervisor/host obligation). mkdtemp => 0700. */
    char dir[] = "/tmp/hc_sup_XXXXXX";
    if (!mkdtemp(dir)) {
        std::perror("mkdtemp");
        return 1;
    }
    std::string sock = std::string(dir) + "/bus.sock";

    Broker *broker = Broker::start(sock.c_str(), -1);
    CHECK(broker != nullptr, "broker start");
    if (!broker) return 1;

    Supervisor *sup = Supervisor::create(sock, AGENTD_PATH);
    CHECK(sup != nullptr, "supervisor create");
    if (!sup) {
        broker->stop();
        delete broker;
        return 1;
    }

    BusClient *t = BusClient::connect(sock.c_str(), "tester");
    CHECK(t != nullptr, "tester connects");

    /* ---- Phase A: two workers exchange messages, then one is killed (crash-isolation) ---- */
    CHECK(sup->spawn("agent:A"), "spawn A");
    CHECK(sup->spawn("agent:B"), "spawn B");
    CHECK(wait_until([&] { return sup->is_ready("agent:A") && sup->is_ready("agent:B"); }, 5000),
          "A and B connect + check in");

    CHECK(t && send_cmd(*t, "agent:A", 1, "{\"cmd\":\"pingpeer\",\"peer\":\"agent:B\"}"),
          "A reaches B — two workers exchange messages");

    CHECK(sup->signal("agent:B", SIGKILL), "kill B mid-flight");
    CHECK(wait_until([&] { return !sup->is_alive("agent:B"); }, 5000),
          "supervisor reaps B (autorestart off -> stays dead)");

    /* The broker and A survived B's death: A still answers, and A's probe of the dead B fails
     * cleanly (bounded by the request deadline) instead of hanging or crashing anything. */
    CHECK(t && send_cmd(*t, "agent:A", 2, "{\"cmd\":\"ping\"}"),
          "A still answers after B crashed (crash-isolation)");
    CHECK(t && !send_cmd(*t, "agent:A", 3, "{\"cmd\":\"pingpeer\",\"peer\":\"agent:B\"}"),
          "A->B now fails cleanly; broker stays up");

    /* ---- Phase B: respawn + recovery ---- */
    sup->set_autorestart(true);
    CHECK(sup->signal("agent:A", SIGKILL), "kill A");
    CHECK(wait_until([&] { return sup->is_ready("agent:A"); }, 6000),
          "supervisor auto-respawns A and it re-checks in");
    CHECK(sup->spawn("agent:B"), "respawn B");
    CHECK(wait_until([&] { return sup->is_ready("agent:B"); }, 6000), "B back and ready");
    CHECK(t && send_cmd(*t, "agent:A", 4, "{\"cmd\":\"pingpeer\",\"peer\":\"agent:B\"}"),
          "A reaches B again after respawn — full recovery");

    /* ---- Phase C: deliberate reap() — retires a worker WITHOUT respawn even with autorestart ON ---- */
    CHECK(sup->is_alive("agent:B"), "B is alive before reap");
    CHECK(sup->reap("agent:B"), "reap B (deliberate retire)");
    CHECK(!sup->is_alive("agent:B"), "reap left B not alive");
    /* autorestart is ON (Phase B) — a reap MUST suppress it; give the monitor a moment, then confirm B stays gone */
    sleep_ms(300);
    CHECK(!sup->is_ready("agent:B") && !sup->is_alive("agent:B"),
          "reap suppressed the autorestart — B is GONE, not respawned");
    CHECK(!sup->reap("agent:B"), "reaping an unknown (already-reaped) id returns false");
    /* the id was FREED + its bus routing revoked, then a fresh spawn re-authorizes it: it checks in + routes */
    CHECK(sup->spawn("agent:B"), "the reaped id is free — spawn B again");
    CHECK(wait_until([&] { return sup->is_ready("agent:B"); }, 6000),
          "the re-spawned B checks in (the freed id is re-authorized + token-confirmed)");
    CHECK(t && send_cmd(*t, "agent:A", 5, "{\"cmd\":\"pingpeer\",\"peer\":\"agent:B\"}"),
          "A reaches the re-spawned B — the freed id routes again after reap->revoke->respawn->re-authorize");

    /* ---- teardown ---- */
    delete t;
    sup->shutdown(); /* SIGKILL + reap all workers, join threads */
    delete sup;
    broker->stop();
    delete broker;
    unlink(sock.c_str());
    rmdir(dir);

    if (g_fails) {
        std::fprintf(stderr, "supervisor_gate: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("supervisor_gate: all checks passed\n");
    return 0;
}
