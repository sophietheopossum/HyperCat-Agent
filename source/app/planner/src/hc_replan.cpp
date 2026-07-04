/* hc_replan — the verify->replan policy (P05b). See hc_replan.hpp.
 *
 * The whole module is a synchronous loop over the orchestrator facade: run the agenda, and while it keeps
 * settling Failed, ask the Decomposer for a corrected plan (seeded with the failure reasons) and run that,
 * up to a fixed budget. Two pure helpers — plan_fingerprint (the no-progress backstop) and build_repair_goal
 * (the failure->prompt framing) — carry all the testable logic; run_with_replan is just the bounded driver
 * around them. No bus, no engine, no LLM here: the Decomposer callback owns the (validated, fail-closed)
 * planning, the Orchestrator owns the running. */

#include "hc_replan.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace hc::planner {

using hc::orch::Agenda;
using hc::orch::Task;
using hc::orch::TaskState;

/* Per-reason byte cap fed into the repair prompt: a worker/verifier result can be large and is untrusted,
 * so we quote only a bounded prefix as context (the whole plan is re-validated downstream regardless). */
static const std::size_t kReasonCap = 512;
static const std::size_t kMaxFailuresQuoted = 32; /* bound the repair prompt against a huge failed set */

std::string plan_fingerprint(const std::vector<Task> &tasks)
{
    std::unordered_map<std::string, std::string> title_of;
    title_of.reserve(tasks.size());
    for (const auto &t : tasks) title_of[t.id] = t.title;

    std::vector<std::string> lines;
    lines.reserve(tasks.size());
    for (const auto &t : tasks) {
        std::vector<std::string> dep_titles;
        dep_titles.reserve(t.deps.size());
        for (const auto &d : t.deps) {
            auto it = title_of.find(d);
            dep_titles.push_back(it != title_of.end() ? it->second : d); /* unknown -> the raw id */
        }
        std::sort(dep_titles.begin(), dep_titles.end());
        std::string line = t.capability;
        line += '\t';
        line += t.title;
        line += '\t';
        for (const auto &dt : dep_titles) {
            line += dt;
            line += ',';
        }
        lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end()); /* order-independent: a reordered-but-identical plan matches */
    std::string fp;
    for (auto &l : lines) {
        fp += l;
        fp += '\n';
    }
    return fp;
}

std::string build_repair_goal(const std::string &goal, const Agenda &failed)
{
    std::string s = goal;
    s += "\n\nA previous attempt to achieve this goal did not succeed. The steps below failed. Produce a "
         "corrected plan that achieves the ORIGINAL goal while fixing or avoiding these failures — address "
         "the root cause; do not simply repeat the same steps:\n";
    std::size_t quoted = 0;
    for (const auto &t : failed.tasks) {
        if (t.state != TaskState::Failed) continue;
        if (quoted++ >= kMaxFailuresQuoted) break;
        s += "- \"";
        s += t.title;
        s += "\" (capability '";
        s += t.capability;
        s += "'): ";
        if (t.result.empty())
            s += "failed";
        else if (t.result.size() <= kReasonCap)
            s += t.result;
        else
            s += t.result.substr(0, kReasonCap) + "...";
        s += '\n';
    }
    return s;
}

/* Block until the running agenda reaches a terminal verdict (Done/Failed), polling so a generous live
 * agenda isn't cut off at one timeout slice — the orchestrator's brick-prevention backstops GUARANTEE
 * every agenda settles, so this terminates. Returns Running only if `cancel` asked us to stop. */
static Orchestrator::Verdict settle(Orchestrator &orch, const ReplanOptions &opts,
                                    const ReplanCancel &cancel)
{
    /* Floor the poll slice: wait_until_done(0) returns immediately, which would turn this into a 100%-CPU
     * busy-spin. A positive floor keeps it a bounded blocking wait however the caller set the option. */
    const int slice = opts.settle_poll_ms > 0 ? opts.settle_poll_ms : 1000;
    for (;;) {
        Orchestrator::Verdict v = orch.wait_until_done(slice);
        if (v != Orchestrator::Verdict::Running) return v;
        if (cancel && cancel()) return Orchestrator::Verdict::Running; /* abort signal */
    }
}

ReplanOutcome run_with_replan(Orchestrator &orch, const Agenda &initial,
                              const std::vector<std::pair<std::string, std::string>> &pool,
                              const Decomposer &decompose, const ReplanOptions &opts,
                              const ReplanCancel &cancel)
{
    ReplanOutcome out;

    /* The initial agenda runs as usual (a goal-only agenda is decomposed by the orchestrator's OWN
     * installed decomposer; explicit tasks run as given). Replan only engages on Failure. */
    if (!orch.run_agenda(initial, pool)) {
        out.detail = "could not start the initial agenda (one already running?)";
        return out; /* Aborted */
    }
    Orchestrator::Verdict v = settle(orch, opts, cancel);
    if (v == Orchestrator::Verdict::Done) {
        out.status = ReplanOutcome::Status::Done;
        return out;
    }
    if (v == Orchestrator::Verdict::Running) { /* cancelled mid-settle */
        out.detail = "cancelled before the initial agenda settled";
        return out; /* Aborted */
    }

    /* v == Failed. Re-plan a repair agenda from the failure reasons, up to the budget. `prev_fp` starts
     * as the EXECUTED initial plan's fingerprint so an identical first repair is caught as no-progress. */
    const int   budget = opts.replan_budget < 0 ? 0 : opts.replan_budget;
    std::string prev_fp = plan_fingerprint(orch.snapshot().tasks);

    for (int round = 1; round <= budget; ++round) {
        if (cancel && cancel()) {
            out.detail = "cancelled after " + std::to_string(round - 1) + " repair round(s)";
            return out; /* Aborted; rounds default to the prior count below */
        }
        if (!decompose) { /* defensive: no repair planner configured */
            out.status = ReplanOutcome::Status::Stalled;
            out.detail = "no repair decomposer configured";
            return out;
        }

        const Agenda      failed_snap = orch.snapshot();
        std::vector<Task> repair = decompose(build_repair_goal(initial.goal, failed_snap));
        if (repair.empty()) { /* the planner rejected its own proposal (fail-closed) */
            out.status = ReplanOutcome::Status::Stalled;
            out.detail = "the repair planner returned no valid plan (fail-closed) on repair round "
                         + std::to_string(round);
            return out;
        }
        std::string fp = plan_fingerprint(repair);
        if (fp == prev_fp) { /* the model proposed the same plan again — bail rather than burn the budget */
            out.status = ReplanOutcome::Status::NoProgress;
            out.detail = "the repair plan was identical to the previous attempt — escalating (no progress)";
            return out;
        }
        prev_fp = std::move(fp);

        Agenda repair_ag;
        repair_ag.id = initial.id + ":repair" + std::to_string(round);
        repair_ag.title = initial.title + " (repair " + std::to_string(round) + ")";
        repair_ag.goal = initial.goal; /* keep the original goal for provenance */
        repair_ag.tasks = std::move(repair);

        if (!orch.run_agenda(repair_ag, pool)) {
            out.detail = "could not start repair agenda " + std::to_string(round);
            return out; /* Aborted */
        }
        Orchestrator::Verdict rv = settle(orch, opts, cancel);
        out.rounds = round; /* this repair agenda actually ran */
        if (rv == Orchestrator::Verdict::Done) {
            out.status = ReplanOutcome::Status::Done;
            out.detail = "recovered after " + std::to_string(round) + " repair round(s)";
            return out;
        }
        if (rv == Orchestrator::Verdict::Running) { /* cancelled mid-settle */
            out.detail = "cancelled during repair agenda " + std::to_string(round);
            return out; /* Aborted */
        }
        /* rv == Failed: loop for another repair round (if the budget remains). */
    }

    out.status = ReplanOutcome::Status::Exhausted;
    out.rounds = budget;
    out.detail = "exhausted " + std::to_string(budget) + " repair round(s) without success — escalating";
    return out;
}

} // namespace hc::planner
