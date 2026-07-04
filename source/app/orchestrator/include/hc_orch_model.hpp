#ifndef HC_ORCH_MODEL_HPP
#define HC_ORCH_MODEL_HPP

/* hc_orch_model — the orchestrator's workspace domain model + task state machine (C++ host).
 *
 * Purpose:   the core workspace domain model — Agent (hierarchy),
 *            Overseer, Agenda (a goal + its task graph), Task (a unit of work) — plus the PURE
 *            state-machine free functions that advance a task and report an agenda's progress.
 *            No bus, no LLM, no threads, no I/O: this is the layer that is trivially deterministic
 *            and unit-tested in isolation. The scheduling decisions live in hc_orch_engine; the
 *            bus/supervisor wiring lives in hc_orchestrator.
 * Owns:      plain values. An Agenda owns its Tasks by value; the caller owns every instance.
 * Threading: pure values + total functions, re-entrant. No globals, no shared state.
 * Deliberately dropped (over-engineering, not the kernel): clearance levels, KPIs,
 *            DB persistence, model preferences, prompt composition, StatusReport/metrics/cost.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace hc::orch {

/* Task lifecycle. Legal edges (enforced by task_advance):
 *   Pending   -> Assigned                (dispatched to a worker)
 *   Pending   -> Failed                  (can NEVER start: a missing/failed dependency, or no agent
 *                                         provides its capability — the scheduler fails it terminally
 *                                         so the agenda settles instead of hanging on it forever)
 *   Assigned  -> Running                 (worker acked)
 *   Assigned  -> Pending | Failed        (worker lost before ack / gave up)
 *   Running   -> Verifying               (result accepted but awaiting sibling self-check, P04)
 *   Running   -> Done | Failed | Pending (result / error / reassign on worker loss)
 *   Verifying -> Done | Failed           (verdict: upheld/unproven -> Done; refuted -> Failed)
 *   Done, Failed are terminal. Pending is the reassignment re-entry point. Verifying is NON-terminal
 *   (the agenda stays un-settled until the verdict lands). */
enum class TaskState { Pending, Assigned, Running, Verifying, Done, Failed };

/* P04 verification policy for a task. mode None = no verification (the default). Sibling = after the
 * task's result is accepted, an INDEPENDENT sibling worker (never the author) re-checks it; a single
 * credible refutation fails the task, a quorum of upholds accepts it. `verifiers` = how many siblings
 * to ask (the budget); `quorum` = upholds needed to accept. */
enum class VerifyMode { None, Sibling };
struct VerifyPolicy {
    VerifyMode mode = VerifyMode::None;
    int        verifiers = 1; /* siblings to ask (budget) */
    int        quorum = 1;    /* upholds needed to accept  */
};

/* One verifier's judgment of a result (P04). Uncertain never accepts or refutes on its own. */
enum class Verdict { Uphold, Refute, Uncertain };

struct Task {
    std::string              id;            /* stable within an agenda, e.g. "t1"             */
    std::string              title;         /* human-readable                                  */
    std::string              description;   /* the unit of work handed to the worker           */
    std::string              artifact_path; /* W1.3: the file the worker must produce ("" = no file deliverable);
                                             * the host verifies it exists before the task settles Done       */
    std::string              capability;    /* required role ("" = any) — matched to Agent.role */
    std::vector<std::string> deps;          /* task ids that must be Done before this can start */
    TaskState                state = TaskState::Pending;
    std::string              assignee;      /* worker bus id while Assigned/Running ("" else)   */
    std::string              result;        /* result payload on Done, or error text on Failed  */
    int                      attempts = 0;  /* dispatch count — bounds reassignment storms      */
    uint64_t                 assigned_at_ms = 0; /* monotonic ms stamped at -> Assigned (deadline)  */
    VerifyPolicy             verify;        /* P04: per-task self-check policy (None by default)    */
};

struct Agenda {
    std::string       id;
    std::string       title;
    std::string       goal;   /* what an overseer decomposes into `tasks` */
    std::vector<Task> tasks;  /* value-owned by the Agenda                */
};

struct Agent {                              /* a hierarchy node (composition, not inheritance) */
    std::string              id;            /* bus id, e.g. "agent:A"                          */
    std::string              role;          /* matched against Task::capability                */
    std::string              parent;        /* "" for a root agent (the hierarchy link)        */
    std::vector<std::string> children;
};

struct Overseer {                           /* a role the host plays, not a worker process */
    std::string id;                         /* e.g. "overseer:1" */
    std::string name;
};

/* ---- pure state-machine helpers (the unit-test surface) ---- */

const char *task_state_str(TaskState);

/* A role satisfies a capability when capability is empty (any) or equals the role. */
bool role_matches(const std::string &capability, const std::string &role);

/* True when `task` is Pending and every dependency id in the agenda is Done. */
bool task_can_start(const Agenda &, const Task &task);

/* True when `task` is Pending but a dependency can NEVER be satisfied — a dep id absent from the
 * agenda, or a dep that has terminally Failed. Such a task can never become startable, so the
 * scheduler fails it (rather than letting the agenda hang). Pool-independent: depends only on the
 * task graph, so it is safe to evaluate at any time (no worker-pool timing race). */
bool task_dep_unsatisfiable(const Agenda &, const Task &task);

/* Advance `task` to `to` if the edge is legal (see the enum banner); false leaves it unchanged. */
bool task_advance(Task &task, TaskState to);

/* Find a task by id (mutable / const); nullptr if absent. */
Task       *agenda_find(Agenda &, const std::string &task_id);
const Task *agenda_find(const Agenda &, const std::string &task_id);

int  agenda_progress(const Agenda &); /* 0..100, Done tasks / total (100 for an empty agenda)  */
bool agenda_complete(const Agenda &); /* every task Done (the success condition)                */
bool agenda_failed(const Agenda &);   /* at least one task terminally Failed                    */
bool agenda_settled(const Agenda &);  /* every task terminal (Done or Failed) — nothing to do   */

/* Terminal-state counts for a one-line agenda summary. The conductor's agenda_status tool formats a
 * string from these, so the COUNTING lives here with the other pure agenda helpers (one consistent pass,
 * unit-tested) and the presentation stays in the caller. `running` = not-yet-terminal (the rest). */
struct AgendaTally {
    size_t total = 0;
    size_t done = 0;
    size_t failed = 0;
};
AgendaTally agenda_tally(const Agenda &);

} // namespace hc::orch

#endif /* HC_ORCH_MODEL_HPP */
