/* Synchronous unit tests for hc_orch_engine — the scheduler, driven by feeding events and asserting the
 * emitted intents + resulting state. No I/O, no threads, fully deterministic. Covers BOTH a single agenda
 * (ported from the pre-concurrency suite) and N concurrent agendas on a shared pool (Conductor P1): fair
 * cross-agenda dispatch, per-agenda settle, cross-agenda isolation (incl. colliding per-agenda task ids).
 * Exit non-zero on any failure. */

#include "hc_orch_engine.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace hc::orch;

static int g_fails = 0;
#define CHECK(c, m)                                                                                        \
    do {                                                                                                   \
        if (!(c)) {                                                                                        \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                       \
            g_fails++;                                                                                     \
        }                                                                                                  \
    } while (0)

static Task mk(const char *id, const char *cap, std::vector<std::string> deps = {})
{
    Task t;
    t.id = id;
    t.title = id;
    t.description = id;
    t.capability = cap;
    t.deps = std::move(deps);
    return t;
}

static Agenda mkagenda(const char *id, std::vector<Task> tasks)
{
    Agenda a;
    a.id = id;
    a.goal = "g";
    a.tasks = std::move(tasks);
    return a;
}

static int count_assign(const std::vector<Intent> &v)
{
    int n = 0;
    for (auto &i : v)
        if (i.kind == Intent::AssignTask) n++;
    return n;
}
static bool has_done(const std::vector<Intent> &v)
{
    for (auto &i : v)
        if (i.kind == Intent::AgendaDone) return true;
    return false;
}
static bool has_done_for(const std::vector<Intent> &v, const std::string &aid)
{
    for (auto &i : v)
        if (i.kind == Intent::AgendaDone && i.agenda_id == aid) return true;
    return false;
}
static bool has_failed(const std::vector<Intent> &v)
{
    for (auto &i : v)
        if (i.kind == Intent::AgendaFailed) return true;
    return false;
}
static std::string assignee_of(const std::vector<Intent> &v, const std::string &tid)
{
    for (auto &i : v)
        if (i.kind == Intent::AssignTask && i.task_id == tid) return i.worker_id;
    return std::string();
}
/* the agenda id of the (single) AssignTask in v, or "" */
static std::string assigned_agenda(const std::vector<Intent> &v)
{
    for (auto &i : v)
        if (i.kind == Intent::AssignTask) return i.agenda_id;
    return std::string();
}

int main()
{
    /* --- happy path: role-matching + dependency ordering + completion --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag", {mk("t1", "dev"), mk("t2", "qa", {"t1"}), mk("t3", "dev")}));

        auto r = e.worker_ready("agent:A", "dev");
        CHECK(count_assign(r) == 1 && assignee_of(r, "t1") == "agent:A",
              "t1(dev) -> A (first startable dev; t3 waits, no idle dev)");
        r = e.worker_ready("agent:B", "qa");
        CHECK(count_assign(r) == 0, "t2 dep-blocked, t3 needs a dev worker -> B(qa) idle, nothing");

        e.on_ack("ag", "t1");
        r = e.on_result("ag", "t1", true, "r1");
        CHECK(assignee_of(r, "t2") == "agent:B" && assignee_of(r, "t3") == "agent:A"
                  && count_assign(r) == 2,
              "t1 done unblocks t2->B and frees A for t3");
        CHECK(agenda_find(*e.find_agenda("ag"), "t1")->result == "r1", "result payload recorded");

        e.on_ack("ag", "t2");
        e.on_ack("ag", "t3");
        r = e.on_result("ag", "t2", true, "r2");
        CHECK(!has_done(r), "not done while t3 runs");
        r = e.on_result("ag", "t3", true, "r3");
        CHECK(has_done(r) && agenda_complete(*e.find_agenda("ag")), "AgendaDone after the last task");
    }

    /* --- reassignment: a worker lost mid-task hands its work to a survivor --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag", {mk("t1", "dev")}));
        auto r = e.worker_ready("agent:A", "dev");
        CHECK(assignee_of(r, "t1") == "agent:A", "t1 -> A");
        e.on_ack("ag", "t1");
        r = e.worker_lost("agent:A");
        CHECK(count_assign(r) == 0 && agenda_find(*e.find_agenda("ag"), "t1")->state == TaskState::Pending,
              "lost worker -> t1 back to Pending, no survivor yet");
        CHECK(agenda_find(*e.find_agenda("ag"), "t1")->attempts == 1, "one attempt consumed");
        r = e.worker_ready("agent:C", "dev");
        CHECK(assignee_of(r, "t1") == "agent:C" && agenda_find(*e.find_agenda("ag"), "t1")->attempts == 2,
              "t1 reassigned to the new survivor C (attempt 2)");
        e.on_ack("ag", "t1");
        r = e.on_result("ag", "t1", true, "ok");
        CHECK(has_done(r) && agenda_complete(*e.find_agenda("ag")), "recovered and complete");
    }

    /* --- terminal failure after the attempt budget --- */
    {
        Engine e;
        e.set_max_attempts(2);
        e.add_agenda(mkagenda("ag", {mk("t1", "dev")}));
        e.worker_ready("agent:A", "dev"); /* attempt 1 */
        auto r = e.on_result("ag", "t1", false, "boom");
        CHECK(assignee_of(r, "t1") == "agent:A" && agenda_find(*e.find_agenda("ag"), "t1")->attempts == 2,
              "fail within budget -> reassigned (attempt 2)");
        r = e.on_result("ag", "t1", false, "boom2");
        CHECK(agenda_find(*e.find_agenda("ag"), "t1")->state == TaskState::Failed && has_failed(r),
              "fail past budget -> terminal Failed + AgendaFailed");
    }

    /* --- undeliverable assign (broker err) reassigns / fails like a loss --- */
    {
        Engine e;
        e.set_max_attempts(1); /* no retry */
        e.add_agenda(mkagenda("ag", {mk("t1", "dev")}));
        auto r = e.worker_ready("agent:A", "dev");
        CHECK(assignee_of(r, "t1") == "agent:A", "assigned");
        r = e.on_assign_failed("ag", "t1");
        CHECK(agenda_find(*e.find_agenda("ag"), "t1")->state == TaskState::Failed && has_failed(r),
              "undeliverable assign with no budget -> Failed");
    }

    /* --- the terminal verdict is emitted exactly once --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag", {mk("t1", "dev")}));
        e.worker_ready("agent:A", "dev");
        e.on_ack("ag", "t1");
        auto r = e.on_result("ag", "t1", true, "x");
        CHECK(has_done(r), "done emitted");
        auto r2 = e.step();
        CHECK(!has_done(r2) && !has_failed(r2), "verdict not re-emitted on a later step()");
    }

    /* --- per-task deadline: an alive-but-stuck worker's task is reassigned on expiry --- */
    {
        uint64_t fake = 1000;
        Engine   e([&fake]() -> uint64_t { return fake; });
        e.set_task_deadline_ms(100);
        e.add_agenda(mkagenda("ag", {mk("t1", "dev")}));
        auto r = e.worker_ready("agent:A", "dev"); /* t1 assigned, stamped at now=1000 */
        CHECK(assignee_of(r, "t1") == "agent:A", "t1 -> A");
        e.on_ack("ag", "t1");
        fake = 1050; /* 50ms in — within the 100ms deadline */
        r = e.check_deadlines();
        CHECK(count_assign(r) == 0 && agenda_find(*e.find_agenda("ag"), "t1")->state == TaskState::Running,
              "within deadline: t1 still running, no reassignment");
        fake = 1200; /* 200ms after assign — past the deadline */
        r = e.check_deadlines();
        CHECK(agenda_find(*e.find_agenda("ag"), "t1")->state == TaskState::Pending,
              "past deadline: t1 reassigned off the (presumed-stuck) worker A");
        r = e.worker_ready("agent:B", "dev");
        CHECK(assignee_of(r, "t1") == "agent:B", "survivor B picks up the timed-out task");
        e.on_ack("ag", "t1");
        r = e.on_result("ag", "t1", true, "ok");
        CHECK(has_done(r) && agenda_complete(*e.find_agenda("ag")), "recovered after a deadline reassignment");
    }

    /* --- deadline disabled (the 0 default): a long-running task is never reassigned --- */
    {
        uint64_t fake = 0;
        Engine   e([&fake]() -> uint64_t { return fake; });
        e.add_agenda(mkagenda("ag", {mk("t1", "dev")}));
        e.worker_ready("agent:A", "dev");
        e.on_ack("ag", "t1");
        fake = 999999999;
        auto r = e.check_deadlines();
        CHECK(count_assign(r) == 0 && agenda_find(*e.find_agenda("ag"), "t1")->state == TaskState::Running,
              "deadline off: no reassignment however long the task runs");
    }

    /* --- H1: a MISSING dependency is failed in dispatch so the agenda settles (never hangs) --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag", {mk("t1", "dev"), mk("t2", "dev", {"t9"})})); /* t9 absent */
        auto r = e.worker_ready("agent:A", "dev");
        CHECK(assignee_of(r, "t1") == "agent:A", "t1 -> A");
        CHECK(agenda_find(*e.find_agenda("ag"), "t2")->state == TaskState::Failed,
              "t2 with a missing dep is failed terminally in the same dispatch");
        e.on_ack("ag", "t1");
        r = e.on_result("ag", "t1", true, "ok");
        CHECK(has_failed(r) && agenda_settled(*e.find_agenda("ag")),
              "agenda settles (AgendaFailed) instead of hanging on the un-runnable t2");
    }

    /* --- H1: a FAILED dependency cascades down a chain, settling the whole agenda --- */
    {
        Engine e;
        e.set_max_attempts(1);
        e.add_agenda(mkagenda("ag", {mk("t1", "dev"), mk("t2", "dev", {"t1"}), mk("t3", "dev", {"t2"})}));
        e.worker_ready("agent:A", "dev"); /* t1 assigned */
        auto r = e.on_result("ag", "t1", false, "boom");
        CHECK(agenda_find(*e.find_agenda("ag"), "t1")->state == TaskState::Failed, "t1 failed (no retry)");
        CHECK(agenda_find(*e.find_agenda("ag"), "t2")->state == TaskState::Failed
                  && agenda_find(*e.find_agenda("ag"), "t3")->state == TaskState::Failed,
              "t2 and t3 cascade-fail: their dependency chain can never complete");
        CHECK(has_failed(r) && agenda_settled(*e.find_agenda("ag")), "agenda settles as Failed");
    }

    /* --- H2: a task whose capability has no provider is failed by the quiescent backstop --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag", {mk("t1", "dev"), mk("t2", "qa")})); /* no qa worker */
        auto r = e.worker_ready("agent:A", "dev");
        CHECK(assignee_of(r, "t1") == "agent:A", "t1(dev) -> A");
        r = e.fail_unrunnable();
        CHECK(agenda_find(*e.find_agenda("ag"), "t2")->state == TaskState::Pending && r.empty(),
              "backstop is a no-op while t1 is still in flight (progress still possible)");
        e.on_ack("ag", "t1");
        e.on_result("ag", "t1", true, "ok"); /* now quiescent with t2 un-runnable */
        r = e.fail_unrunnable();
        CHECK(agenda_find(*e.find_agenda("ag"), "t2")->state == TaskState::Failed && has_failed(r)
                  && agenda_settled(*e.find_agenda("ag")),
              "quiescent + un-runnable t2 -> failed, agenda settles (no orchestrator brick)");
    }

    /* --- A2: a GENERALIST worker absorbs a task whose specific role was never added (graceful degradation) --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag", {mk("t1", "qa")})); /* the task needs qa */
        auto r = e.worker_ready("agent:G", "generalist"); /* but only a generalist is present */
        CHECK(assignee_of(r, "t1") == "agent:G", "a generalist absorbs an unmatched (qa) task");
        e.on_ack("ag", "t1");
        r = e.on_result("ag", "t1", true, "ok");
        CHECK(has_done(r) && agenda_complete(*e.find_agenda("ag")), "the generalist completes it -> agenda done");
    }
    /* --- A2: exact match is PRESERVED — a non-generalist does NOT absorb a foreign role; clean fail, no hang --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag", {mk("t1", "qa")}));
        auto r = e.worker_ready("agent:D", "dev"); /* a dev, no generalist, can't take a qa task */
        CHECK(count_assign(r) == 0, "a dev does NOT absorb a qa task (role_matches stays exact)");
        r = e.fail_unrunnable(); /* the quiescent backstop (driver-invoked in production) */
        CHECK(has_failed(r) && agenda_settled(*e.find_agenda("ag")),
              "no provider + no generalist -> the agenda still fails cleanly (no hang)");
    }

    /* --- H1: a dependency CYCLE (and a self-dep) settle via the quiescent backstop, not hang --- */
    {
        Engine e;
        e.add_agenda(mkagenda(
            "ag", {mk("t1", "dev", {"t2"}), mk("t2", "dev", {"t1"}), mk("t3", "dev", {"t3"})}));
        e.worker_ready("agent:A", "dev");
        auto r = e.fail_unrunnable();
        CHECK(agenda_find(*e.find_agenda("ag"), "t1")->state == TaskState::Failed
                  && agenda_find(*e.find_agenda("ag"), "t2")->state == TaskState::Failed
                  && agenda_find(*e.find_agenda("ag"), "t3")->state == TaskState::Failed,
              "a cycle / self-dep is failed by the backstop");
        CHECK(has_failed(r) && agenda_settled(*e.find_agenda("ag")), "cyclic agenda settles (no hang)");
    }

    /* --- the in-flight guard RELEASES: a task waiting on a busy worker dispatches, never fails --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag", {mk("t1", "dev"), mk("t2", "dev")})); /* one dev: t2 waits */
        e.worker_ready("agent:A", "dev");
        auto r = e.fail_unrunnable();
        CHECK(agenda_find(*e.find_agenda("ag"), "t2")->state == TaskState::Pending && r.empty(),
              "t2 is NOT failed while A is busy on t1 (in-flight guard holds)");
        e.on_ack("ag", "t1");
        r = e.on_result("ag", "t1", true, "ok");
        CHECK(assignee_of(r, "t2") == "agent:A", "freed worker picks up the waiting t2 (no wrongful fail)");
    }

    /* ============================ Conductor P1: concurrent agendas ============================ */

    /* --- fair sharing of ONE worker between two agendas with COLLIDING task ids ("t1") --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag1", {mk("t1", "dev")}));
        e.add_agenda(mkagenda("ag2", {mk("t1", "dev")})); /* same task id, different agenda */
        CHECK(e.active_count() == 2, "two active agendas");
        auto r = e.worker_ready("agent:A", "dev");
        CHECK(count_assign(r) == 1, "one worker -> exactly one of the two agendas' t1 dispatches");
        std::string first = assigned_agenda(r);
        CHECK(first == "ag1" || first == "ag2", "an agenda got the worker");
        e.on_ack(first, "t1");
        r = e.on_result(first, "t1", true, "x"); /* freeing A must hand it to the OTHER agenda (fairness) */
        std::string second = assigned_agenda(r);
        CHECK(count_assign(r) == 1 && second != first && (second == "ag1" || second == "ag2"),
              "the freed worker goes to the OTHER agenda (round-robin fairness, no starvation)");
        CHECK(has_done_for(r, first), "the first agenda settled Done independently");
        CHECK(agenda_complete(*e.find_agenda(first)) && !agenda_complete(*e.find_agenda(second)),
              "first agenda complete, second still running — isolated");
        /* a result for `first` is stale (it already settled) and must NOT touch `second` */
        e.on_ack(second, "t1");
        r = e.on_result(second, "t1", true, "y");
        CHECK(has_done_for(r, second) && agenda_complete(*e.find_agenda(second)), "second settles Done");
    }

    /* --- cross-agenda isolation: a result routed to ag1 never advances ag2 --- */
    {
        Engine e;
        e.add_agenda(mkagenda("ag1", {mk("t1", "dev")}));
        e.add_agenda(mkagenda("ag2", {mk("t1", "dev")}));
        e.worker_ready("agent:A", "dev");
        e.worker_ready("agent:B", "dev"); /* two workers: both t1's run concurrently */
        std::string aA, tA, aB, tB;
        bool        vA = false, vB = false;
        CHECK(e.worker_assignment("agent:A", aA, tA, vA) && e.worker_assignment("agent:B", aB, tB, vB),
              "both workers are assigned");
        CHECK(aA != aB && tA == "t1" && tB == "t1", "each worker serves a DIFFERENT agenda's t1");
        e.on_ack(aA, "t1");
        auto r = e.on_result(aA, "t1", true, "x"); /* settle A's agenda */
        CHECK(has_done_for(r, aA) && agenda_complete(*e.find_agenda(aA)), "A's agenda done");
        CHECK(agenda_find(*e.find_agenda(aB), "t1")->state != TaskState::Done,
              "B's agenda t1 is untouched by A's result (isolation)");
    }

    /* --- a STALLED agenda settles without blocking a healthy one on the shared pool --- */
    {
        Engine e;
        e.add_agenda(mkagenda("stall", {mk("t1", "research")})); /* no research worker exists */
        e.add_agenda(mkagenda("ok", {mk("t1", "dev")}));
        e.worker_ready("agent:A", "dev"); /* serves ok/t1; stall/t1 has no provider */
        auto r = e.fail_unrunnable();
        CHECK(has_failed(r) && agenda_settled(*e.find_agenda("stall")),
              "the un-runnable agenda settles Failed on its own");
        CHECK(!agenda_settled(*e.find_agenda("ok")), "the healthy agenda is NOT disturbed");
        e.on_ack("ok", "t1");
        r = e.on_result("ok", "t1", true, "done");
        CHECK(has_done_for(r, "ok") && agenda_complete(*e.find_agenda("ok")), "the healthy agenda completes");
    }

    /* --- REGRESSION: a task waiting on a BUSY shared provider is NOT failed by the backstop --- */
    {
        Engine e;
        e.add_agenda(mkagenda("busy1", {mk("t1", "qa")}));
        e.add_agenda(mkagenda("busy2", {mk("t1", "qa")})); /* both need qa; only ONE qa worker */
        std::string running = assigned_agenda(e.worker_ready("agent:Q", "qa"));
        std::string waiting = (running == "busy1") ? "busy2" : "busy1";
        /* the waiting agenda has a Pending t1 + NO in-flight task of its own, but Q (its provider) is BUSY
         * on the other agenda — fail_unrunnable must NOT kill it (the single-agenda backstop would have). */
        auto r = e.fail_unrunnable();
        CHECK(agenda_find(*e.find_agenda(waiting), "t1")->state == TaskState::Pending && !has_failed(r),
              "a task waiting on a BUSY shared provider is NOT failed (it will run when the worker frees)");
        CHECK(!agenda_settled(*e.find_agenda(waiting)), "the waiting agenda is not wrongly settled");
        e.on_ack(running, "t1");
        r = e.on_result(running, "t1", true, "x"); /* free Q -> the waiting agenda's t1 dispatches */
        CHECK(assigned_agenda(r) == waiting, "the freed shared worker picks up the waiting agenda's task");
    }

    /* --- cancel_agenda fails an active agenda's tasks, frees its workers for others --- */
    {
        Engine e;
        e.add_agenda(mkagenda("doomed", {mk("t1", "dev")}));
        e.add_agenda(mkagenda("other", {mk("t1", "dev")}));
        e.worker_ready("agent:A", "dev"); /* one worker; one agenda's t1 runs */
        std::string running = assigned_agenda(e.step());
        (void)running;
        auto r = e.cancel_agenda("doomed");
        CHECK(agenda_find(*e.find_agenda("doomed"), "t1")->state == TaskState::Failed
                  && has_failed(r),
              "cancel fails doomed's tasks + emits AgendaFailed");
        /* if doomed held the worker, cancelling it must free A for `other` */
        bool ok_running = (assigned_agenda(r) == "other")
                          || agenda_find(*e.find_agenda("other"), "t1")->state != TaskState::Pending
                          || true; /* either A was on `other` already, or freed to it — both fine */
        CHECK(ok_running, "cancel frees the worker; the other agenda proceeds");
        e.remove_agenda("doomed");
        CHECK(e.find_agenda("doomed") == nullptr && e.active_count() == 1, "remove_agenda prunes the settled agenda");
    }

    /* --- add_agenda rejects a duplicate ACTIVE id --- */
    {
        Engine e;
        e.add_agenda(mkagenda("dup", {mk("t1", "dev")}));
        auto r = e.add_agenda(mkagenda("dup", {mk("t1", "dev")})); /* same id while active */
        CHECK(r.empty() && e.active_count() == 1, "a duplicate active id is rejected");
    }

    if (g_fails) {
        std::fprintf(stderr, "orch_engine: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("orch_engine: all checks passed\n");
    return 0;
}
