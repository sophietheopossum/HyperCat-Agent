#ifndef HC_REPLAN_HPP
#define HC_REPLAN_HPP

/* hc_replan — the verify→replan policy (P05b). When an agenda settles Failed, re-plan a repair agenda
 * from the failure reasons and re-run it — bounded — until it succeeds or a budget / no-progress guard
 * escalates to the operator. This is HOST POLICY sitting ABOVE the orchestrator facade: it decides WHAT
 * to try next; the orchestrator still decides HOW to run each agenda (it is never touched here).
 *
 * Purpose:   run an agenda to a terminal verdict; on Failure, classify the failed tasks (the engine's
 *            brick-prevention reasons + P04's verification grounds, both carried in Task::result), ask a
 *            Decomposer for a CORRECTED plan, and re-run — at most `replan_budget` repair rounds, stopping
 *            early on no-progress (a repair plan identical to the last) or a stall (an empty decomposition).
 * Owns:      nothing. BORROWS the Orchestrator facade (the caller owns it) and a Decomposer callback.
 * Threading: synchronous on the caller's thread; drives run_agenda/wait_until_done serially — one agenda
 *            at a time, honoring the facade's single-agenda guard. Not thread-safe (no shared state).
 * Security:  every repair plan rides the SAME untrusted-input path as the initial — the Decomposer
 *            validates before dispatch (fail-closed), so a garbled/hostile model response can't half-run.
 *            The failure reasons fed back into the repair prompt are untrusted (worker/verifier text);
 *            they are length-bounded and only ever become prompt CONTEXT, never code — and the resulting
 *            plan is re-validated. The loop is BOUNDED (replan_budget) with a no-progress backstop, so a
 *            looping/hostile model can never drive unbounded agendas or token spend.
 * Depends:   hc::orchestrator (the facade + the orch::Task model). Never the bus, never the engine. */

#include "hc_orch_model.hpp"
#include "hc_orchestrator.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hc::planner {

/* fn(goal) -> validated tasks (the Orchestrator::Decomposer shape). For the initial run the orchestrator
 * uses its OWN installed decomposer; replan calls THIS to build each repair agenda (it bakes the failure
 * context into the goal string it passes). In production both are the same hc_planner::decompose lambda. */
using Decomposer = std::function<std::vector<hc::orch::Task>(const std::string &)>;

/* fn() -> true to abort the loop (e.g. host shutdown). Optional (empty = never cancels). Sampled between
 * agendas AND between settle-poll slices, so a long settle can be interrupted; a cancel WHILE an agenda is
 * still settling returns status Aborted with that agenda STILL running on the driver — the caller must not
 * immediately reuse `orch` (let it settle, or tear it down). In the default wiring `cancel` is empty. */
using ReplanCancel = std::function<bool()>;

struct ReplanOptions {
    int replan_budget = 3;         /* max repair agendas after the initial failure (clamped >= 0) */
    int settle_poll_ms = 20000;    /* wait_until_done poll slice; the orchestrator guarantees an    */
                                   /* agenda settles, so this is a poll interval, not a hard cap.   */
};

struct ReplanOutcome {
    enum class Status {
        Done,       /* an agenda (the initial or a repair) settled Done                            */
        Exhausted,  /* every one of replan_budget repair rounds still failed                       */
        NoProgress, /* a repair plan was identical to the previous attempt — the model is looping  */
        Stalled,    /* a repair decomposition came back empty (invalid / over-budget / no fn)      */
        Aborted     /* run_agenda was refused, or the loop was cancelled between agendas           */
    };
    Status      status = Status::Aborted;
    int         rounds = 0; /* repair agendas actually RUN (0 = the initial settled, or we bailed
                             * before running any repair) */
    std::string detail;     /* operator-facing escalation summary (surface on a non-Done status)   */
};

/* Run `initial` to a terminal verdict on `pool`; on Failure, re-plan + re-run a repair agenda up to
 * opts.replan_budget times via `decompose`, stopping on Done / exhaustion / no-progress / a stall.
 * `orch` must be idle (no agenda running). Synchronous — it blocks (polling) on each agenda's settle.
 * Returns the outcome; the caller surfaces .detail to the operator on a non-Done status. */
ReplanOutcome run_with_replan(Orchestrator &orch, const hc::orch::Agenda &initial,
                              const std::vector<std::pair<std::string, std::string>> &pool,
                              const Decomposer &decompose, const ReplanOptions &opts = {},
                              const ReplanCancel &cancel = {});

/* ---- PURE helpers (offline unit-test surface) ---- */

/* A canonical, order-independent fingerprint of a task set: sorted "capability\ttitle\t<sorted dep
 * TITLES>" lines (deps mapped id->title so a pure id-rename of an otherwise-identical plan still
 * fingerprints the same). Two plans with the same fingerprint are treated as "no progress". This is a
 * heuristic backstop for a looping model; the HARD bound on the loop is replan_budget. */
std::string plan_fingerprint(const std::vector<hc::orch::Task> &tasks);

/* Build the repair goal handed to the decomposer: the original goal + each Failed task's title +
 * capability + reason (Task::result, length-bounded), framed as "fix or avoid these". PURE. The reasons
 * are untrusted text — bounded here, validated as part of the resulting plan downstream. */
std::string build_repair_goal(const std::string &goal, const hc::orch::Agenda &failed);

} // namespace hc::planner

#endif /* HC_REPLAN_HPP */
