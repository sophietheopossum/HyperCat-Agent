/* test_wal_recovery — THE P14 crash-recovery gate (integration, offline). Two deterministic proofs:
 *
 *   PART A — a real agenda runs to completion over real worker PROCESSES with the WAL on, and once it
 *            SETTLES its WAL is removed (the driver journaled the lifecycle and cleaned up — nothing stale
 *            is left for a recovery to re-run).
 *
 *   PART B — THE crux. We reproduce a host crash mid-agenda by writing exactly the records the driver
 *            fsyncs before a crash (an `open` snapshot + one task `done`), using the SAME serializers the
 *            driver uses (no schema drift). Then recovery folds that WAL back into a resumable agenda and
 *            re-runs it over real workers, and we assert: the crashed-before task's Done RESULT survives
 *            verbatim (it is NOT re-run — re-running would change it to the echo worker's output), the
 *            in-flight tasks re-dispatch + complete, the agenda settles, and the WAL is then removed.
 *
 * A real SIGKILL-mid-agenda harness (fork the host, kill it, restart) is deferred: a graceful stop leaves
 * the identical on-disk WAL state (the settle block skips removal unless the agenda actually settled), so
 * the recovery behaviour proven here is the same one a SIGKILL would exercise. AGENTD_PATH from CMake. */

#include "hc_bus.hpp"
#include "hc_orch_wal.hpp" /* internal: the record serializers (PRIVATE src include) — no schema drift */
#include "hc_orchestrator.hpp"
#include "hc_supervisor.hpp"
#include "hc_wal.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include <unistd.h>

using hc::Broker;
using hc::Orchestrator;
using hc::Supervisor;
using hc::orch::Agenda;
using hc::orch::agenda_find;
using hc::orch::Task;
using hc::orch::TaskState;

static int g_fails = 0;
#define CHECK(c, m)                                                                                    \
    do {                                                                                               \
        if (!(c)) {                                                                                    \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                   \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}
template <typename F> static bool wait_until(F pred, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        if (pred()) return true;
        sleep_ms(20);
    }
    return pred();
}

static Task mk(const char *id, const char *cap, const char *desc, std::vector<std::string> deps = {})
{
    Task t;
    t.id = id;
    t.title = id;
    t.description = desc;
    t.capability = cap;
    t.deps = std::move(deps);
    return t;
}

static int count_streams(const std::string &wal_dir)
{
    int n = 0;
    hc_wal_list(
        wal_dir.c_str(), [](const char *, void *u) { (*(int *)u)++; return 0; }, &n);
    return n;
}

int main()
{
    char dir[] = "/tmp/hc_walrec_XXXXXX";
    if (!mkdtemp(dir)) {
        std::perror("mkdtemp");
        return 1;
    }
    std::string wal_dir = std::string(dir) + "/wal";
    if (mkdir(wal_dir.c_str(), 0700) != 0) {
        std::perror("mkdir wal");
        return 1;
    }
    std::string sock = std::string(dir) + "/bus.sock";

    Broker *broker = Broker::start(sock.c_str(), -1);
    CHECK(broker != nullptr, "broker start");
    Supervisor *sup = broker ? Supervisor::create(sock, AGENTD_PATH) : nullptr;
    CHECK(sup != nullptr, "supervisor create");
    if (!broker || !sup) return 1;
    CHECK(sup->spawn("agent:A"), "spawn A (dev)");
    CHECK(sup->spawn("agent:B"), "spawn B (qa)");
    CHECK(wait_until([&] { return sup->is_ready("agent:A") && sup->is_ready("agent:B"); }, 6000),
          "A + B check in");
    std::vector<std::pair<std::string, std::string>> pool = {{"agent:A", "dev"}, {"agent:B", "qa"}};

    /* ---- PART A: a real agenda journals + the WAL is removed once it settles ---- */
    {
        Orchestrator *orch = Orchestrator::create(sock, "orchestrator", sup, wal_dir);
        CHECK(orch != nullptr, "orchestrator create (WAL on)");
        Agenda ag;
        ag.id = "agA";
        ag.title = "normal run";
        ag.tasks = {mk("a1", "dev", "build"), mk("a2", "qa", "verify", {"a1"})};
        orch->run_agenda(ag, pool);
        CHECK(orch->wait_until_done(20000) == Orchestrator::Verdict::Done, "agA completes Done");
        CHECK(count_streams(wal_dir) == 0, "a settled agenda's WAL is removed (no stale recovery state)");
        delete orch;
    }

    /* ---- PART B: simulate a crash mid-agenda, then recover + resume ---- */

    /* Write the WAL exactly as the driver would before a crash: the run-start `open` snapshot (all
     * Pending), then `t1` reported Done with its real result — then the host "crashed" (no settled record,
     * t2/t3 still in flight). Same serializers the driver uses. */
    {
        Agenda ag;
        ag.id = "crashed";
        ag.title = "resume me";
        ag.tasks = {mk("t1", "dev", "build A"), mk("t2", "dev", "build B", {"t1"}), mk("t3", "qa", "check")};
        hc_wal *w = hc_wal_open(wal_dir.c_str(), ag.id.c_str());
        CHECK(w != nullptr, "open the crashed WAL");
        std::string open = hc::orch::wal::rec_open(ag);
        std::string done = hc::orch::wal::rec_done("t1", "ORIGINAL-T1-RESULT");
        CHECK(w && hc_wal_append(w, open.c_str(), open.size()) == 0, "journal open");
        CHECK(w && hc_wal_append(w, done.c_str(), done.size()) == 0, "journal t1 done");
        hc_wal_close(w);
    }

    /* Recover: the fold must give t1 Done (with its verbatim result), t2 + t3 Pending. */
    std::vector<Agenda> recovered = Orchestrator::recover_incomplete(wal_dir);
    CHECK(recovered.size() == 1, "exactly one incomplete agenda is recovered");
    if (recovered.size() == 1) {
        const Agenda &r = recovered[0];
        CHECK(r.id == "crashed" && r.tasks.size() == 3, "the recovered agenda graph is intact");
        const Task *r1 = agenda_find(r, "t1");
        const Task *r2 = agenda_find(r, "t2");
        CHECK(r1 && r1->state == TaskState::Done && r1->result == "ORIGINAL-T1-RESULT",
              "fold: t1 recovered Done with its verbatim result");
        CHECK(r2 && r2->state == TaskState::Pending, "fold: the in-flight t2 recovered Pending (re-dispatch)");
    }

    /* Resume it over the real workers (a NEW orchestrator, WAL on — as a restarted host would). */
    if (recovered.size() == 1) {
        Orchestrator *orch = Orchestrator::create(sock, "orchestrator", sup, wal_dir);
        CHECK(orch != nullptr, "orchestrator create (resume)");
        CHECK(orch->run_agenda(recovered[0], pool), "run the recovered agenda");
        CHECK(orch->wait_until_done(20000) == Orchestrator::Verdict::Done, "the resumed agenda completes Done");

        Agenda      snap = orch->snapshot();
        const Task *s1 = agenda_find(snap, "t1");
        const Task *s2 = agenda_find(snap, "t2");
        const Task *s3 = agenda_find(snap, "t3");
        CHECK(s1 && s1->state == TaskState::Done && s1->result == "ORIGINAL-T1-RESULT",
              "t1 was NOT re-run — its ORIGINAL result is preserved (the durability win)");
        CHECK(s2 && s2->state == TaskState::Done && !s2->result.empty(),
              "the in-flight t2 was re-dispatched + completed");
        CHECK(s3 && s3->state == TaskState::Done && !s3->result.empty(),
              "the never-started t3 was dispatched + completed");
        CHECK(count_streams(wal_dir) == 0, "the resumed agenda settled -> its WAL was removed");
        delete orch;
    }

    sup->shutdown();
    delete sup;
    broker->stop();
    delete broker;
    unlink(sock.c_str());
    rmdir(wal_dir.c_str());
    rmdir(dir);

    if (g_fails) {
        std::fprintf(stderr, "wal_recovery: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("wal_recovery: all checks passed\n");
    return 0;
}
