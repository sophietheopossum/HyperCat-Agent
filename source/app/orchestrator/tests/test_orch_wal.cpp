/* test_orch_wal — the P14 record-schema + pure-fold gate (offline, deterministic). Proves that an agenda's
 * lifecycle records (open/done/failed/settled) fold back into the right Agenda: Done/Failed tasks keep their
 * state AND result (the durability win — no re-run of finished work), an in-flight task (no terminal record)
 * recovers as Pending for re-dispatch, `settled` is detected, a corrupt line is skipped, and a forged
 * delta for a non-existent task id is ignored. No bus, no disk — just serialize -> fold. */

#include "hc_orch_wal.hpp" /* PRIVATE src include (like test_orch_codec / test_verify) */

#include "hc_orch_model.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace hc::orch;

static int g_fails = 0;
#define CHECK(c, m)                                                                                    \
    do {                                                                                               \
        if (!(c)) {                                                                                    \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                   \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

static Task mk(const char *id, const char *cap, const char *desc, std::vector<std::string> deps = {})
{
    Task t;
    t.id = id;
    t.title = std::string(id) + " title";
    t.description = desc;
    t.capability = cap;
    t.deps = std::move(deps);
    return t;
}

int main()
{
    /* a 3-task agenda; t2 depends on t1 */
    Agenda ag;
    ag.id = "ag1";
    ag.title = "release";
    ag.goal = "ship it";
    ag.tasks = {mk("t1", "dev", "build A"), mk("t2", "qa", "verify A", {"t1"}), mk("t3", "ops", "deploy")};

    /* --- serialize a run-in-progress: open (all pending) + t1 done + t3 failed; t2 still in flight --- */
    std::vector<std::string> lines;
    lines.push_back(wal::rec_open(ag));
    lines.push_back(wal::rec_done("t1", "built A ok"));
    lines.push_back(wal::rec_failed("t3", "deploy blew up"));
    /* (no record for t2 — it was Assigned/Running when the host 'crashed') */

    wal::Folded f = wal::fold(lines);
    CHECK(f.valid, "fold: a stream with an open record is valid");
    CHECK(!f.settled, "fold: no settled record -> not settled");
    CHECK(f.agenda.id == "ag1" && f.agenda.goal == "ship it", "fold: agenda id + goal recovered");
    CHECK(f.agenda.tasks.size() == 3, "fold: all 3 tasks recovered from the open snapshot");

    const Task *t1 = agenda_find(f.agenda, "t1");
    const Task *t2 = agenda_find(f.agenda, "t2");
    const Task *t3 = agenda_find(f.agenda, "t3");
    CHECK(t1 && t1->state == TaskState::Done && t1->result == "built A ok",
          "fold: a Done task keeps its state AND result (the durability win)");
    CHECK(t3 && t3->state == TaskState::Failed && t3->result == "deploy blew up",
          "fold: a Failed task keeps its state AND reason");
    CHECK(t2 && t2->state == TaskState::Pending && t2->assignee.empty() && t2->attempts == 0,
          "fold: an in-flight task recovers as Pending (fresh budget) for re-dispatch");
    CHECK(t2 && t2->deps.size() == 1 && t2->deps[0] == "t1", "fold: the task graph (deps) survives");
    CHECK(t2 && t2->capability == "qa", "fold: the task capability survives (re-routes on re-dispatch)");

    /* --- settled detection --- */
    {
        std::vector<std::string> ls = {wal::rec_open(ag), wal::rec_done("t1", "ok"),
                                       wal::rec_settled(false)};
        wal::Folded fs = wal::fold(ls);
        CHECK(fs.settled, "fold: a settled record is detected (recovery skips/cleans a settled stream)");
    }

    /* --- a corrupt line is skipped, not fatal; a forged delta for an unknown id is ignored --- */
    {
        std::vector<std::string> ls = {wal::rec_open(ag), "this is not json at all",
                                       wal::rec_done("nonexistent", "forged"), wal::rec_done("t1", "real")};
        wal::Folded fc = wal::fold(ls);
        CHECK(fc.valid && fc.agenda.tasks.size() == 3, "fold: a corrupt line is skipped, the fold continues");
        const Task *ft1 = agenda_find(fc.agenda, "t1");
        CHECK(ft1 && ft1->state == TaskState::Done && ft1->result == "real",
              "fold: a real delta still applies after a corrupt line");
        /* the forged 'nonexistent' delta created no task */
        CHECK(agenda_find(fc.agenda, "nonexistent") == nullptr,
              "fold: a delta for a non-declared task id is ignored (no task is conjured)");
    }

    /* --- an empty / open-less stream is invalid (recovery discards it) --- */
    {
        wal::Folded fe = wal::fold({});
        CHECK(!fe.valid, "fold: an empty stream is invalid");
        wal::Folded fo = wal::fold({wal::rec_done("t1", "x")}); /* a delta with no open */
        CHECK(!fo.valid, "fold: a stream with no open record is invalid");
    }

    /* --- last-open-wins (a resume re-opens with the recovered snapshot) --- */
    {
        Agenda ag2 = ag;
        ag2.tasks[0].state = TaskState::Done; /* a resume's open carries t1 already Done */
        ag2.tasks[0].result = "carried over";
        std::vector<std::string> ls = {wal::rec_open(ag), wal::rec_done("t2", "first run"),
                                       wal::rec_open(ag2)}; /* the resume supersedes */
        wal::Folded fr = wal::fold(ls);
        const Task *rt1 = agenda_find(fr.agenda, "t1");
        const Task *rt2 = agenda_find(fr.agenda, "t2");
        CHECK(rt1 && rt1->state == TaskState::Done && rt1->result == "carried over",
              "fold: a resume open carries the recovered Done state (a 2nd crash still preserves it)");
        CHECK(rt2 && rt2->state == TaskState::Pending,
              "fold: the later open supersedes earlier deltas (last open wins)");
    }

    if (g_fails == 0) std::printf("test_orch_wal: OK\n");
    return g_fails ? 1 : 0;
}
