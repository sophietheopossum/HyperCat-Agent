/* Step-5 (orchestrator) gate — integration. An overseer drives a multi-agent agenda end to end:
 * the orchestrator decomposes a goal into tasks (including a dependency-ordered one), assigns them
 * across real agentd worker PROCESSES over the bus, and brings the agenda to completion even though
 * one worker deterministically crashes mid-task — its work is reassigned to a survivor. Fully
 * offline: the worker's task action is a deterministic echo (no LLM, no network).
 *
 * AGENTD_PATH is injected by CMake. Exit non-zero on any failure. */

#include "hc_bus.hpp"
#include "hc_orchestrator.hpp"
#include "hc_replan.hpp" /* P05b: the verify->replan loop over the facade */
#include "hc_supervisor.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

using hc::Broker;
using hc::Orchestrator;
using hc::Supervisor;
using hc::orch::Agenda;
using hc::orch::Task;
using hc::orch::TaskState;

static int g_fails = 0;
#define CHECK(c, m)                                                                            \
    do {                                                                                       \
        if (!(c)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                           \
            g_fails++;                                                                         \
        }                                                                                      \
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

/* #8: record settle-observer firings. The observer runs on the orchestrator DRIVER thread, so the log is
 * mutex-guarded; the main thread polls saw() (the observer fires just after wait_until_done unblocks). */
struct SettleLog {
    std::mutex                                                 mu;
    std::vector<std::pair<std::string, Orchestrator::Verdict>> events;
    void add(const Orchestrator::AgendaSettled &e)
    {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back({e.agenda_id, e.verdict});
    }
    bool saw(const std::string &id, Orchestrator::Verdict v)
    {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto &p : events)
            if (p.first == id && p.second == v) return true;
        return false;
    }
};

int main()
{
    char dir[] = "/tmp/hc_orch_XXXXXX";
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

    Orchestrator *orch = Orchestrator::create(sock, "orchestrator", sup);
    CHECK(orch != nullptr, "orchestrator create");
    if (!orch) {
        delete sup;
        broker->stop();
        delete broker;
        return 1;
    }

    /* #8: register a settle observer before any run_agenda — it must fire once per agenda settle. */
    SettleLog settle_log;
    orch->set_settle_observer([&settle_log](const Orchestrator::AgendaSettled &e) { settle_log.add(e); });

    /* The overseer's decomposition (offline fixture in place of deep_reason): four tasks, a role
     * mix, and t2 depending on t1 to prove dependency ordering. */
    orch->set_decomposer([](const std::string &goal) -> std::vector<Task> {
        (void)goal;
        return {
            mk("t1", "dev", "build A"),
            mk("t2", "qa", "verify A", {"t1"}),
            mk("t3", "dev", "build B"),
            mk("t4", "qa", "verify B"),
        };
    });

    /* Pool: two healthy workers (A dev, B qa) and one that crashes on its first task (C dev). */
    CHECK(sup->spawn("agent:A"), "spawn A");
    CHECK(sup->spawn("agent:B"), "spawn B");
    CHECK(sup->spawn("agent:C", {"--crash-on-task"}), "spawn C (crash-on-task)");
    CHECK(wait_until([&] { return sup->is_ready("agent:A") && sup->is_ready("agent:B") &&
                                  sup->is_ready("agent:C"); },
                     6000),
          "A, B, C all check in");

    Agenda ag;
    ag.id = "ag1";
    ag.title = "release";
    ag.goal = "ship the thing"; /* empty tasks -> the decomposer fills them */
    std::vector<std::pair<std::string, std::string>> pool = {
        {"agent:A", "dev"}, {"agent:B", "qa"}, {"agent:C", "dev"}};
    CHECK(orch->run_agenda(ag, pool), "run agenda");

    Orchestrator::Verdict v = orch->wait_until_done(20000);
    CHECK(v == Orchestrator::Verdict::Done,
          "agenda completes Done end-to-end despite a worker crashing mid-task");
    CHECK(orch->progress() == 100, "progress 100%");

    Agenda snap = orch->snapshot();
    int done = 0;
    for (const auto &t : snap.tasks)
        if (t.state == TaskState::Done) done++;
    CHECK(snap.tasks.size() == 4 && done == 4, "all four tasks reached Done");

    CHECK(!sup->is_alive("agent:C"), "the crashing worker C died (crash genuinely happened)");
    CHECK(sup->is_alive("agent:A") && sup->is_alive("agent:B"),
          "survivors A and B stayed up (crash-isolation through the orchestrator)");

    /* --- P04: sibling self-check, end to end over real worker processes (offline verdicts) --- */
    /* v1 then PLEASE_REFUTE (dep on v1) so the two never run concurrently — with a 2-worker pool that
     * guarantees an idle sibling is always free to verify (else a task accepts unverified). */
    orch->set_decomposer([](const std::string &) -> std::vector<Task> {
        return {mk("v1", "dev", "produce v1"),                  /* echo "done: v1" -> UPHELD -> Done       */
                mk("PLEASE_REFUTE", "dev", "produce bad", {"v1"})}; /* echo carries the sentinel -> REFUTED */
    });
    orch->set_verify({hc::orch::VerifyMode::Sibling, 1, 1}); /* one independent sibling, quorum 1 */
    Agenda ag2;
    ag2.id = "ag2";
    ag2.title = "self-check";
    ag2.goal = "verify the work"; /* empty tasks -> the decomposer fills them */
    std::vector<std::pair<std::string, std::string>> pool2 = {{"agent:A", "dev"}, {"agent:B", "qa"}};
    CHECK(orch->run_agenda(ag2, pool2), "run the verify agenda");
    Orchestrator::Verdict v2 = orch->wait_until_done(20000);
    Agenda                snap2 = orch->snapshot();
    const Task           *vt = nullptr, *rt = nullptr;
    for (const auto &t : snap2.tasks) {
        if (t.id == "v1") vt = &t;
        if (t.id == "PLEASE_REFUTE") rt = &t;
    }
    CHECK(vt && vt->state == TaskState::Done, "an UPHELD task is accepted (Done) after sibling verification");
    CHECK(rt && rt->state == TaskState::Failed, "a REFUTED task is rejected (Failed) by the sibling verifier");
    CHECK(rt && rt->result.find("refuted") != std::string::npos,
          "the refuted task carries the verifier's grounds (for P05b replan)");
    CHECK(v2 == Orchestrator::Verdict::Failed, "the agenda settles Failed (a refuted task failed it)");

    /* --- P05b: verify->replan, end to end over real worker processes (offline) --- */
    /* A task with a capability NO worker provides fails the agenda (the engine's fail_unrunnable backstop) —
     * a deterministic offline failure that needs no fault injection. The host then re-plans + re-runs. */
    orch->set_verify({hc::orch::VerifyMode::None, 1, 1}); /* isolate replan from P04 verification */

    /* (1) replan-to-success: the initial plan has an unrunnable task -> Failed; the repair plan is clean. */
    {
        orch->set_decomposer([](const std::string &) -> std::vector<Task> {
            return {mk("ok", "dev", "do A"), mk("bad", "nocap", "needs a missing role")};
        });
        hc::planner::Decomposer repair_ok = [](const std::string &) -> std::vector<Task> {
            return {mk("fix", "dev", "do A the right way")}; /* all-runnable -> Done */
        };
        Agenda ag3;
        ag3.id = "ag3";
        ag3.title = "replan-ok";
        ag3.goal = "achieve the thing";
        hc::planner::ReplanOptions opt;
        opt.replan_budget = 3;
        hc::planner::ReplanOutcome o = hc::planner::run_with_replan(*orch, ag3, pool2, repair_ok, opt);
        CHECK(o.status == hc::planner::ReplanOutcome::Status::Done, "replan recovers a Failed agenda to Done");
        CHECK(o.rounds == 1, "replan succeeded on the first repair round");
    }

    /* (2) budget-exhausted-escalates: every repair still fails (a DISTINCT doomed plan each round so the
     * no-progress guard doesn't trip first) -> the loop spends the budget and escalates. */
    {
        orch->set_decomposer([](const std::string &) -> std::vector<Task> {
            return {mk("bad0", "nocap", "doomed")};
        });
        int                     calls = 0;
        hc::planner::Decomposer repair_bad = [&calls](const std::string &) -> std::vector<Task> {
            calls++;
            return calls == 1 ? std::vector<Task>{mk("bad1", "nocap", "still doomed one")}
                              : std::vector<Task>{mk("bad2", "nocap", "still doomed two")};
        };
        Agenda ag4;
        ag4.id = "ag4";
        ag4.title = "replan-exhaust";
        ag4.goal = "an impossible goal";
        hc::planner::ReplanOptions opt;
        opt.replan_budget = 2;
        hc::planner::ReplanOutcome o = hc::planner::run_with_replan(*orch, ag4, pool2, repair_bad, opt);
        CHECK(o.status == hc::planner::ReplanOutcome::Status::Exhausted,
              "replan escalates (Exhausted) when the budget is spent without success");
        CHECK(o.rounds == 2, "replan ran exactly replan_budget repair rounds");
        CHECK(calls == 2, "the repair decomposer was called once per repair round");
    }

    /* (3) no-progress: the repair planner keeps proposing the SAME plan -> escalate WITHOUT burning the
     * budget (the plan-fingerprint backstop catches a looping model). */
    {
        orch->set_decomposer([](const std::string &) -> std::vector<Task> {
            return {mk("loop", "nocap", "doomed")};
        });
        hc::planner::Decomposer repair_same = [](const std::string &) -> std::vector<Task> {
            return {mk("loop", "nocap", "doomed")}; /* identical to the initial executed plan, every call */
        };
        Agenda ag5;
        ag5.id = "ag5";
        ag5.title = "replan-noprogress";
        ag5.goal = "goal";
        hc::planner::ReplanOptions opt;
        opt.replan_budget = 5;
        hc::planner::ReplanOutcome o = hc::planner::run_with_replan(*orch, ag5, pool2, repair_same, opt);
        CHECK(o.status == hc::planner::ReplanOutcome::Status::NoProgress,
              "replan escalates (NoProgress) when the repair plan never changes");
        CHECK(o.rounds == 0, "no-progress is caught before running any repair agenda");
    }

    /* --- Conductor P1: TWO agendas run CONCURRENTLY on the shared fleet, settle independently --- */
    {
        orch->set_verify({hc::orch::VerifyMode::None, 1, 1}); /* plain execution */
        orch->set_decomposer(nullptr);                        /* use the explicit tasks below */
        CHECK(sup->spawn("agent:D"), "spawn D (dev) for the concurrent test");
        CHECK(wait_until([&] { return sup->is_ready("agent:D"); }, 6000), "D checks in");
        std::vector<std::pair<std::string, std::string>> cpool = {
            {"agent:A", "dev"}, {"agent:B", "qa"}, {"agent:D", "dev"}};

        Agenda cx;
        cx.id = "cx";
        cx.title = "concurrent X";
        cx.tasks = {mk("x1", "dev", "do x1"), mk("x2", "qa", "do x2", {"x1"})};
        Agenda cy;
        cy.id = "cy";
        cy.title = "concurrent Y";
        cy.tasks = {mk("y1", "dev", "do y1"), mk("y2", "qa", "do y2", {"y1"})};

        /* submit BOTH without waiting -> they run AT THE SAME TIME on the shared pool */
        CHECK(orch->run_agenda(cx, cpool), "admit concurrent agenda cx");
        CHECK(orch->run_agenda(cy, cpool), "admit concurrent agenda cy (a second one runs at the same time)");
        CHECK(!orch->run_agenda(cx, cpool), "a duplicate ACTIVE agenda id is rejected");

        Orchestrator::Verdict vx = orch->wait_until_done("cx", 20000);
        Orchestrator::Verdict vy = orch->wait_until_done("cy", 20000);
        CHECK(vx == Orchestrator::Verdict::Done, "concurrent agenda cx settled Done");
        CHECK(vy == Orchestrator::Verdict::Done, "concurrent agenda cy settled Done");
        CHECK(orch->progress("cx") == 100 && orch->progress("cy") == 100, "both concurrent agendas at 100%");

        Agenda sx = orch->snapshot("cx"), sy = orch->snapshot("cy");
        int    dx = 0, dy = 0;
        for (const auto &t : sx.tasks)
            if (t.state == TaskState::Done) dx++;
        for (const auto &t : sy.tasks)
            if (t.state == TaskState::Done) dy++;
        CHECK(sx.tasks.size() == 2 && dx == 2, "cx: both tasks Done (results routed to cx, not cy)");
        CHECK(sy.tasks.size() == 2 && dy == 2, "cy: both tasks Done (results routed to cy, not cx)");

        std::vector<std::string> ids = orch->list_agendas();
        bool                     has_cx = false, has_cy = false;
        for (const auto &id : ids) {
            if (id == "cx") has_cx = true;
            if (id == "cy") has_cy = true;
        }
        CHECK(has_cx && has_cy, "list_agendas reports both concurrent agendas");
    }

    /* #8: the settle observer fired with the correct verdict for each agenda that settled with a stable id.
     * (It fires on the driver thread at the bottom of the settling iteration — just after wait_until_done
     * unblocks — so poll briefly for the most-recent cx/cy.) */
    bool settles_seen = wait_until(
        [&] {
            return settle_log.saw("ag1", Orchestrator::Verdict::Done) &&
                   settle_log.saw("ag2", Orchestrator::Verdict::Failed) &&
                   settle_log.saw("cx", Orchestrator::Verdict::Done) &&
                   settle_log.saw("cy", Orchestrator::Verdict::Done);
        },
        5000);
    CHECK(settles_seen,
          "#8: the settle observer fired with the right verdict — ag1(Done), ag2(Failed), cx(Done), cy(Done)");

    /* --- P2 conductor-conversations REGRESSION (CRIT-1): the settle-observer UNBIND must be a BARRIER. The
     * conductor binds a settle observer capturing a raw Conductor*; a conversation switch / teardown UNBINDS then
     * FREES it. If set_settle_observer(nullptr) doesn't wait for an in-flight fire, the driver dereferences the
     * freed object. Here a SLEEPING observer captures a heap Target; while it is mid-fire, the main thread unbinds
     * + frees. The unbind MUST block until the fire finishes (a broken barrier => ASan/TSan flags the free-vs-touch
     * race + the in_fire assertion fails). */
    {
        CHECK(sup->spawn("agent:Z"), "spawn Z (barrier test)");
        CHECK(wait_until([&] { return sup->is_ready("agent:Z"); }, 6000), "Z checks in");
        struct Target {
            std::atomic<bool> in_fire{false};
            std::atomic<int>  touched{0};
        };
        Target *target = new Target();
        orch->set_decomposer([](const std::string &) -> std::vector<Task> { return {mk("z1", "dev", "do z")}; });
        orch->set_verify({hc::orch::VerifyMode::None, 0, 0}); /* no sibling verify -> z1 settles immediately */
        orch->set_settle_observer([target](const Orchestrator::AgendaSettled &e) {
            (void)e;
            target->in_fire.store(true);
            sleep_ms(120);                /* widen the fire window so the unbind genuinely races it */
            target->touched.fetch_add(1); /* a TOUCH ASan flags if target was freed during the fire */
            target->in_fire.store(false);
        });
        Agenda az;
        az.id = "azbar";
        az.goal = "barrier";
        std::vector<std::pair<std::string, std::string>> zpool = {{"agent:Z", "dev"}};
        CHECK(orch->run_agenda(az, zpool), "run barrier agenda");
        CHECK(wait_until([&] { return target->in_fire.load(); }, 10000), "the settle observer started firing");
        orch->set_settle_observer(nullptr); /* the BARRIER: blocks until the in-flight fire completes */
        CHECK(!target->in_fire.load(), "set_settle_observer(nullptr) BARRIERED the in-flight fire (not mid-fire)");
        CHECK(target->touched.load() == 1, "the fire completed its touch before the unbind returned");
        delete target; /* safe: the observer is unbound + done — a broken barrier would have ASan-flagged a UAF */
    }

    delete orch; /* stops + joins the driver thread, disconnects */
    sup->shutdown();
    delete sup;
    broker->stop();
    delete broker;
    unlink(sock.c_str());
    rmdir(dir);

    if (g_fails) {
        std::fprintf(stderr, "orchestrator_gate: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("orchestrator_gate: all checks passed\n");
    return 0;
}
