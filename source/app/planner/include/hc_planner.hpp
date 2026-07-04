#ifndef HC_PLANNER_HPP
#define HC_PLANNER_HPP

/* hc_planner — turn a GOAL into a validated task DAG (P05a / P05b). The autonomy layer that decides
 * WHAT the work is; the orchestrator decides HOW to run it (it is never touched here).
 *
 * Purpose:   prompt an LLM to decompose a goal into {tasks: [{id,title,description,capability,deps}]},
 *            parse it, and VALIDATE it before a single task is dispatched. The validated tasks install
 *            into the orchestrator via its existing `set_decomposer` seam. P05b adds the replan policy
 *            (re-decompose a repair agenda after a Failed settle, bounded).
 * Owns:      nothing durable. `decompose` BORROWS an hc_llm chat client for one call; the parser +
 *            validator are PURE (no I/O, no LLM) and are the offline unit-test surface.
 * Threading: synchronous on the caller's thread; not thread-safe (no shared state).
 * Security:  the LLM plan is UNTRUSTED structured input — `validate_plan` rejects an oversized / cyclic /
 *            dangling-dep / unknown-capability / plan-bomb proposal so it can NEVER reach the bus; task
 *            titles/descriptions become worker PROMPTS, never code. decompose fails CLOSED (empty plan ->
 *            the orchestrator runs nothing) so a hostile/garbled model response can't half-run.
 * Depends:   hc::orchestrator (the orch::Task model + the facade), hc::llm (the decompose call),
 *            hc::json (parse). Never the bus directly, never the engine internals. */

#include "hc_orch_model.hpp" /* hc::orch::Task */

#include <cstddef>
#include <string>
#include <vector>

struct hc_llm; /* forward — decompose borrows a chat client; the real header stays in the .cpp */

namespace hc::planner {

struct PlanLimits {
    std::size_t max_tasks = 32;       /* a goal decomposes into at most this many tasks       */
    std::size_t max_field_bytes = 8192; /* per id/title/description/capability byte cap        */
    std::size_t max_deps = 16;        /* per-task dependency count cap                          */
};

/* Parse plan JSON `{"tasks":[{"id","title","description","capability","deps":[...]}]}` into Tasks. PURE +
 * offline. false on malformed JSON / a missing required field / a non-array deps — NO partial plan. */
bool parse_plan_json(const std::string &json, std::vector<hc::orch::Task> &out);

/* Validate an LLM-proposed task list. PURE + offline. true iff: within `lim`; ids non-empty + UNIQUE;
 * every `capability` is in `allowed_caps`; every dep references an id present in the list; and the dep
 * graph is ACYCLIC. *why (optional) receives the first rejection reason. */
bool validate_plan(const std::vector<hc::orch::Task> &tasks,
                   const std::vector<std::string> &allowed_caps, const PlanLimits &lim,
                   std::string *why = nullptr);

/* The plan-request user prompt for `goal` + the offered capabilities. PURE — exposed so the prompt is
 * testable and `decompose` is a thin wrapper. */
std::string build_plan_prompt(const std::string &goal, const std::vector<std::string> &allowed_caps);

/* REPAIR a weak model's capabilities IN PLACE so the plan survives instead of being rejected: each task's
 * capability that isn't already a known role is mapped to the nearest one — a small case-insensitive substring
 * table (developer->dev, tester->qa, ...), with anything unrecognised (or empty) landing on the generalist (or, if
 * that role was deleted, the first known role). PURE + offline-testable. Run BETWEEN parse and validate so the
 * strict validator's contract (and the hostile-plan tests that call it directly) stay intact. */
std::string nearest_role(const std::string &capability, const std::vector<std::string> &allowed_caps);
void        normalize_capabilities(std::vector<hc::orch::Task> &tasks,
                                   const std::vector<std::string> &allowed_caps);

/* The Decomposer (fn(goal)->tasks, the `Orchestrator::set_decomposer` shape): prompt `llm` to decompose
 * `goal`, NORMALIZE then validate against `allowed_caps`. Returns the validated tasks, or an EMPTY vector on ANY
 * failure (fail-closed — the orchestrator then runs nothing). *why (optional) receives the failure reason on an
 * empty result (so the conductor can surface "no plan / no worker for X" instead of flailing). Requires network +
 * a valid key on `llm`. */
std::vector<hc::orch::Task> decompose(hc_llm *llm, const std::string &goal,
                                      const std::vector<std::string> &allowed_caps,
                                      const PlanLimits &lim = {}, std::string *why = nullptr);

} // namespace hc::planner

#endif /* HC_PLANNER_HPP */
