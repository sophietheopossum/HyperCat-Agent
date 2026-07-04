/* Pure unit tests for hc_orch_model — the task state machine + agenda queries. No I/O, no threads,
 * fully deterministic. Exit non-zero on any failure. */

#include "hc_orch_model.hpp"

#include <cstdio>
#include <vector>

using namespace hc::orch;

static int g_fails = 0;
#define CHECK(c, m)                                                                            \
    do {                                                                                       \
        if (!(c)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                           \
            g_fails++;                                                                         \
        }                                                                                      \
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

int main()
{
    /* --- task_advance: legal happy path + illegal edges leave state unchanged --- */
    {
        Task t = mk("t1", "");
        CHECK(t.state == TaskState::Pending, "starts pending");
        CHECK(!task_advance(t, TaskState::Done), "pending->done is illegal");
        CHECK(t.state == TaskState::Pending, "illegal edge does not mutate");
        CHECK(task_advance(t, TaskState::Assigned), "pending->assigned");
        CHECK(task_advance(t, TaskState::Running), "assigned->running");
        CHECK(task_advance(t, TaskState::Done), "running->done");
        CHECK(!task_advance(t, TaskState::Pending), "done is terminal");
        CHECK(!task_advance(t, TaskState::Failed), "done cannot fail");

        Task b = mk("t9", ""); /* pending->failed: a task that can never start fails terminally */
        CHECK(task_advance(b, TaskState::Failed), "pending->failed (un-runnable: bad dep / no agent)");
        CHECK(b.state == TaskState::Failed && !task_advance(b, TaskState::Assigned),
              "failed is terminal (cannot resurrect)");
    }

    /* --- reassignment: running/assigned -> pending clears the assignee --- */
    {
        Task t = mk("t1", "");
        task_advance(t, TaskState::Assigned);
        t.assignee = "agent:A";
        task_advance(t, TaskState::Running);
        CHECK(task_advance(t, TaskState::Pending), "running->pending (reassign)");
        CHECK(t.assignee.empty(), "reassign clears the assignee");
        Task u = mk("t2", "");
        task_advance(u, TaskState::Assigned);
        u.assignee = "agent:B";
        CHECK(task_advance(u, TaskState::Pending), "assigned->pending (lost before ack)");
        CHECK(u.assignee.empty(), "assigned->pending clears the assignee");
        CHECK(task_advance(u, TaskState::Assigned) && task_advance(u, TaskState::Failed),
              "assigned->failed (gave up)");
    }

    /* --- role_matches --- */
    CHECK(role_matches("", "anything"), "empty capability matches any role");
    CHECK(role_matches("dev", "dev"), "exact role matches");
    CHECK(!role_matches("dev", "qa"), "mismatched role rejected");

    /* --- deps / task_can_start --- */
    {
        Agenda a;
        a.id = "ag1";
        a.goal = "g";
        a.tasks.push_back(mk("t1", ""));
        a.tasks.push_back(mk("t2", "", {"t1"}));
        CHECK(task_can_start(a, *agenda_find(a, "t1")), "t1 startable (no deps)");
        CHECK(!task_can_start(a, *agenda_find(a, "t2")), "t2 blocked on t1");
        agenda_find(a, "t1")->state = TaskState::Done;
        CHECK(task_can_start(a, *agenda_find(a, "t2")), "t2 startable once t1 is done");
        a.tasks.push_back(mk("t9", "", {"nope"}));
        CHECK(!task_can_start(a, *agenda_find(a, "t9")), "a missing dep blocks forever");
        CHECK(agenda_find(a, "absent") == nullptr, "find absent -> nullptr");

        /* task_dep_unsatisfiable: distinguishes "can never start" from "merely not yet startable" */
        CHECK(task_dep_unsatisfiable(a, *agenda_find(a, "t9")), "missing dep -> unsatisfiable");
        CHECK(!task_dep_unsatisfiable(a, *agenda_find(a, "t2")),
              "t2's dep (t1) is Done -> satisfiable, not failed");
        Task blk = mk("t3", "", {"t1"});
        agenda_find(a, "t1")->state = TaskState::Failed; /* now t1 is terminally failed */
        CHECK(task_dep_unsatisfiable(a, blk), "a FAILED dep -> unsatisfiable (never becomes Done)");
    }

    /* --- agenda progress / complete / failed / settled --- */
    {
        Agenda a;
        a.tasks.push_back(mk("t1", ""));
        a.tasks.push_back(mk("t2", ""));
        CHECK(agenda_progress(a) == 0, "0% at start");
        CHECK(!agenda_complete(a) && !agenda_failed(a) && !agenda_settled(a), "in-flight");
        agenda_find(a, "t1")->state = TaskState::Done;
        CHECK(agenda_progress(a) == 50, "50% with one done");
        agenda_find(a, "t2")->state = TaskState::Done;
        CHECK(agenda_progress(a) == 100 && agenda_complete(a) && agenda_settled(a), "all done");

        Agenda b;
        b.tasks.push_back(mk("t1", ""));
        b.tasks.push_back(mk("t2", ""));
        agenda_find(b, "t1")->state = TaskState::Done;
        agenda_find(b, "t2")->state = TaskState::Failed;
        CHECK(!agenda_complete(b) && agenda_failed(b) && agenda_settled(b),
              "settled-with-failure: failed + settled, not complete");

        Agenda e;
        CHECK(agenda_progress(e) == 100 && agenda_complete(e) && agenda_settled(e) &&
                  !agenda_failed(e),
              "empty agenda is vacuously complete");

        /* agenda_tally — the done/failed/total counts the conductor's agenda_status formats from. */
        AgendaTally ta = agenda_tally(a); /* a: t1+t2 both Done */
        CHECK(ta.total == 2 && ta.done == 2 && ta.failed == 0, "tally: all-done counts");
        AgendaTally tb = agenda_tally(b); /* b: t1 Done, t2 Failed */
        CHECK(tb.total == 2 && tb.done == 1 && tb.failed == 1, "tally: mixed done/failed counts");
        AgendaTally te = agenda_tally(e); /* empty */
        CHECK(te.total == 0 && te.done == 0 && te.failed == 0, "tally: empty agenda is all-zero");
    }

    if (g_fails) {
        std::fprintf(stderr, "orch_model: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("orch_model: all checks passed\n");
    return 0;
}
