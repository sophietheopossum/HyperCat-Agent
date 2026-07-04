/* test_verify — the P04 gate (offline, deterministic): the PURE tally (uphold-quorum, single-refute-
 * decisive, budget→Unproven) + the ENGINE wiring (a successful result goes Verifying, a sibling — never
 * the author — verifies it, an uphold accepts / a refute rejects, no idle sibling accepts unverified,
 * a lost verifier can't hang). No bus, no LLM — the engine is driven by feeding events. Conductor P1: the
 * engine is multi-agenda, so events thread the agenda id ("a"). */

#include "hc_verify.hpp" /* PRIVATE src include (like test_orch_codec) */

#include "hc_orch_engine.hpp"
#include "hc_orch_model.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace hc::orch;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                               \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                                                 \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

/* find the first intent of a kind in a batch; returns nullptr if absent */
static const Intent *find_intent(const std::vector<Intent> &v, Intent::Kind k)
{
    for (const auto &i : v)
        if (i.kind == k) return &i;
    return nullptr;
}

/* admit a single-task agenda "a" into `e` (the engine is move-only, so we add into a caller-owned one) */
static void add_one_task(Engine &e)
{
    Agenda ag;
    ag.id = "a";
    ag.goal = "g";
    Task t;
    t.id = "t1";
    t.title = "T1";
    t.capability = "dev";
    ag.tasks.push_back(t);
    e.add_agenda(std::move(ag));
}

int main()
{
    /* --- PURE tally --- */
    CHECK(tally(0, 0, 0, 1, 1) == VerifyOutcome::Pending, "tally: nothing reported -> Pending");
    CHECK(tally(1, 0, 1, 1, 1) == VerifyOutcome::Upheld, "tally: 1 uphold, quorum 1 -> Upheld");
    CHECK(tally(1, 0, 1, 2, 2) == VerifyOutcome::Pending, "tally: 1/2 upholds, budget left -> Pending");
    CHECK(tally(2, 0, 2, 2, 2) == VerifyOutcome::Upheld, "tally: 2 upholds, quorum 2 -> Upheld");
    CHECK(tally(3, 1, 4, 2, 4) == VerifyOutcome::Refuted, "tally: a single refute is DECISIVE");
    CHECK(tally(0, 1, 1, 1, 1) == VerifyOutcome::Refuted, "tally: 1 refute -> Refuted");
    CHECK(tally(0, 0, 1, 2, 1) == VerifyOutcome::Unproven, "tally: budget spent, no quorum/refute -> Unproven");
    CHECK(verdict_from_str("refute") == Verdict::Refute && verdict_from_str("uphold") == Verdict::Uphold
              && verdict_from_str("???") == Verdict::Uncertain,
          "verdict_from_str maps words + defaults Uncertain");

    /* --- ENGINE: a successful result is VERIFIED by a sibling (never the author) --- */
    {
        Engine e;
        add_one_task(e);
        e.set_default_verify({VerifyMode::Sibling, 1, 1});
        e.worker_ready("A", "dev"); /* t1 -> assigned to A (the author) */
        e.worker_ready("B", "dev"); /* B is the idle sibling            */
        e.on_ack("a", "t1");        /* Running */
        auto        out = e.on_result("a", "t1", true, "the answer is 42");
        const Task *t = agenda_find(*e.find_agenda("a"), "t1");
        CHECK(t && t->state == TaskState::Verifying, "a successful Sibling-mode result goes Verifying");
        const Intent *vi = find_intent(out, Intent::VerifyTask);
        CHECK(vi != nullptr, "a VerifyTask intent is emitted");
        CHECK(vi && vi->worker_id == "B", "the verifier is the SIBLING (B), never the author (A)");
        CHECK(vi && vi->task_id == "t1", "the verify intent names the original task");
        CHECK(vi && vi->agenda_id == "a", "the verify intent names the agenda");

        e.on_verdict("a", "t1", "B", Verdict::Uphold, "looks correct"); /* uphold -> Done */
        const Task *t2 = agenda_find(*e.find_agenda("a"), "t1");
        CHECK(t2 && t2->state == TaskState::Done, "an upheld verdict accepts the task (Done)");
        CHECK(t2 && t2->result == "the answer is 42", "the upheld result is preserved");
    }

    /* --- a REFUTE rejects the task (Failed, grounds carried for replan) --- */
    {
        Engine e;
        add_one_task(e);
        e.set_default_verify({VerifyMode::Sibling, 1, 1});
        e.worker_ready("A", "dev");
        e.worker_ready("B", "dev");
        e.on_ack("a", "t1");
        e.on_result("a", "t1", true, "the answer is 41");
        e.on_verdict("a", "t1", "B", Verdict::Refute, "41 is wrong; it should be 42");
        const Task *t = agenda_find(*e.find_agenda("a"), "t1");
        CHECK(t && t->state == TaskState::Failed, "a refuted verdict rejects the task (Failed)");
        CHECK(t && t->result.find("refuted") != std::string::npos
                  && t->result.find("should be 42") != std::string::npos,
              "the refusal grounds are carried in the result (for P05b replan)");
    }

    /* --- NO idle sibling: the result is accepted unverified (never blocks) --- */
    {
        Engine e;
        add_one_task(e);
        e.set_default_verify({VerifyMode::Sibling, 1, 1});
        e.worker_ready("A", "dev"); /* only the author exists */
        e.on_ack("a", "t1");
        e.on_result("a", "t1", true, "solo");
        const Task *t = agenda_find(*e.find_agenda("a"), "t1");
        CHECK(t && t->state == TaskState::Done, "no independent sibling -> accept unverified (Done, no hang)");
    }

    /* --- a LOST verifier can't hang the task (fail-closed -> resolves) --- */
    {
        Engine e;
        add_one_task(e);
        e.set_default_verify({VerifyMode::Sibling, 1, 1});
        e.worker_ready("A", "dev");
        e.worker_ready("B", "dev");
        e.on_ack("a", "t1");
        e.on_result("a", "t1", true, "the answer"); /* -> Verifying, B verifying */
        e.worker_lost("B");                          /* the verifier crashes before reporting */
        const Task *t = agenda_find(*e.find_agenda("a"), "t1");
        CHECK(t && (t->state == TaskState::Done || t->state == TaskState::Failed),
              "a lost verifier resolves the task (no hang)");
    }

    /* --- an ALIVE-but-WEDGED verifier (silent past the verify deadline) is force-resolved, not hung. --- */
    {
        uint64_t fake = 1000;
        Engine   e([&fake]() -> uint64_t { return fake; });
        add_one_task(e);
        e.set_default_verify({VerifyMode::Sibling, 1, 1});
        e.set_task_deadline_ms(100);
        e.worker_ready("A", "dev"); /* author, assigned at now=1000 */
        e.worker_ready("B", "dev"); /* the idle sibling */
        e.on_ack("a", "t1");
        e.on_result("a", "t1", true, "the answer"); /* -> Verifying; verify window re-stamps at now=1000 */
        CHECK(agenda_find(*e.find_agenda("a"), "t1")->state == TaskState::Verifying, "H1 setup: t1 Verifying");
        fake = 1050; /* 50ms in — within the verify deadline */
        e.check_deadlines();
        CHECK(agenda_find(*e.find_agenda("a"), "t1")->state == TaskState::Verifying,
              "within the verify deadline: a silent verifier does not yet resolve the task");
        fake = 1200; /* 200ms after entering Verifying — the window blew its deadline */
        e.check_deadlines();
        const Task *tv = agenda_find(*e.find_agenda("a"), "t1");
        CHECK(tv && tv->state == TaskState::Done,
              "past the verify deadline: a wedged-but-alive verifier is force-resolved (Unproven -> Done)");
        CHECK(tv && tv->result.find("inconclusive") != std::string::npos,
              "the force-resolved task is marked verification-inconclusive (not silently upheld)");
    }

    if (g_fails == 0) std::printf("test_verify: OK\n");
    return g_fails ? 1 : 0;
}
