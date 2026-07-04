#ifndef HC_ORCH_ENGINE_HPP
#define HC_ORCH_ENGINE_HPP

/* hc_orch_engine — the agenda scheduler. Decisions only; NO I/O (C++ host).
 *
 * Purpose:   own a SET of active agendas' task graphs + ONE shared pool of schedulable workers, and
 *            decide WHAT to do next — which Pending task (in any active agenda) whose deps are met to
 *            dispatch to which idle role-matching worker; what to do when a result, an ack, an
 *            undeliverable assign, or a worker loss arrives (complete / reassign / fail). It emits
 *            INTENTS ("assign task T of agenda G to worker W", "agenda G done/failed"); it never
 *            performs them. Keeping it pure means the whole scheduler is unit-tested synchronously by
 *            feeding events and asserting the intents + the resulting state — no sockets, no sleeps,
 *            no threads.
 * Concurrency model (Conductor P1): ONE engine holds N concurrent agendas over the SHARED worker pool.
 *            Tasks from different agendas compete for the same role-matched workers; dispatch() is fair
 *            (round-robin across agendas) so no agenda starves. Each agenda settles INDEPENDENTLY (its
 *            own AgendaDone/AgendaFailed). Per-agenda state (deps, deadlines, reassignment, verify) stays
 *            isolated; only the worker pool is shared.
 * Owns:      the active agendas (moved in via add_agenda) + the shared worker pool + busy-set. By value.
 * Threading: single-owner, NOT thread-safe by design — only the orchestrator's ONE driver thread ever
 *            touches it (the same single-caller discipline BusClient uses). No locks needed.
 * Scope:     INTERNAL to hc_orchestrator — not part of the module's public contract. It lives in
 *            include/ (rather than src/) only so the module's own unit test can link the Engine
 *            directly; no downstream target should include it.
 */

#include "hc_orch_model.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hc::orch {

/* What the engine wants the driver to DO. The engine never does I/O itself. Every intent names its
 * `agenda_id` so the driver routes the resulting bus traffic + journaling to the right agenda. */
struct Intent {
    enum Kind { AssignTask, VerifyTask, AgendaDone, AgendaFailed } kind;
    std::string agenda_id; /* which active agenda this intent belongs to (all kinds)             */
    std::string worker_id; /* Assign/VerifyTask: the target worker                               */
    std::string task_id;   /* Assign/VerifyTask: which task (VerifyTask = the ORIGINAL task id)  */
    std::string lens;      /* VerifyTask: the review lens the skeptic should apply               */
};

class Engine {
public:
    /* now_ms: an injectable monotonic clock (milliseconds). Empty (the default) = a real
     * CLOCK_MONOTONIC reader; tests pass a controllable clock to drive the per-task deadline
     * deterministically, without sleeping. The engine starts EMPTY — agendas are admitted with
     * add_agenda. */
    using ClockFn = std::function<uint64_t()>;
    explicit Engine(ClockFn now_ms = ClockFn());
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /* Admit a new agenda (moved in) onto the shared pool. A duplicate `agenda.id` (one already active) is
     * rejected (returns no intents, leaves state unchanged). Returns the intents to act on now — the new
     * agenda may immediately schedule onto idle workers. */
    std::vector<Intent> add_agenda(Agenda agenda);

    /* Operator cancel of an active agenda: fail its non-terminal tasks, free the workers it held, and emit
     * its terminal AgendaFailed exactly once. A no-op (no intents) for an unknown id. */
    std::vector<Intent> cancel_agenda(const std::string &agenda_id);

    /* Prune a SETTLED agenda from the active set (the driver calls this after it has copied the agenda's
     * final state out + journaled it, in response to AgendaDone/AgendaFailed) so settled agendas don't
     * accumulate in a long-lived engine. A no-op for an unknown id; harmless on a still-active id (but the
     * driver only prunes settled ones). */
    void remove_agenda(const std::string &agenda_id);

    /* Pool management (the driver tells the engine who can run work — the pool is SHARED across agendas).
     * Each returns the intents to act on now (a new idle worker may unblock pending tasks in any agenda; a
     * lost worker's in-flight task is reassigned within its own agenda to a survivor). */
    std::vector<Intent> worker_ready(const std::string &id, const std::string &role);
    std::vector<Intent> worker_lost(const std::string &id);

    /* Drive the schedule: assign dispatchable tasks across ALL active agendas to idle role-matching
     * workers (round-robin across agendas for fairness), and emit each settled agenda's terminal verdict
     * exactly once. Idempotent between events. */
    std::vector<Intent> step();

    /* Brick-prevention backstop. For each QUIESCENT agenda (none of ITS tasks Assigned/Running/Verifying)
     * with Pending tasks left, fail those tasks terminally so that agenda settles instead of hanging. Per
     * agenda — a stalled agenda settles without disturbing the others. Must be called ONLY at a quiescent
     * wake (after the startup pool is registered + liveness polled), never mid-startup. */
    std::vector<Intent> fail_unrunnable();

    /* Feed bus events back in, routed to `agenda_id` (the driver provides it from its worker->agenda map,
     * which sidesteps per-agenda task-id collisions). Each returns the intents to act on next. */
    std::vector<Intent> on_ack(const std::string &agenda_id, const std::string &task_id);
    std::vector<Intent> on_result(const std::string &agenda_id, const std::string &task_id, bool ok,
                                  const std::string &payload);
    std::vector<Intent> on_assign_failed(const std::string &agenda_id, const std::string &task_id);

    /* P04: enable sibling self-check. A task whose own verify.mode is None inherits this default; with a
     * Sibling policy, a successful result goes Verifying and the engine emits VerifyTask intents to idle
     * siblings (never the author) instead of completing immediately. Applies to agendas added AFTER it is
     * set. Default None (no verification). */
    void set_default_verify(VerifyPolicy);

    /* P04: a verifier's verdict on a Verifying task in `agenda_id`. Tally -> accept (Done), refute
     * (Failed, result = grounds), or wait for more. */
    std::vector<Intent> on_verdict(const std::string &agenda_id, const std::string &task_id,
                                   const std::string &verifier, Verdict, const std::string &grounds);

    /* Read-only access for the driver's snapshots + journaling. */
    std::vector<std::string> agenda_ids() const;                 /* the active agendas, in admit order */
    const Agenda            *find_agenda(const std::string &agenda_id) const; /* null if not active     */
    std::size_t              active_count() const;

    /* Find what `worker` is currently doing — for the driver to route a task.result it can only key by the
     * broker-stamped sender (so a per-agenda task-id collision never misroutes). Sets agenda_id + task_id +
     * is_verify and returns true if the worker holds a task (Assigned/Running) in some agenda OR is an
     * expected verifier of some Verifying task; false if idle/unknown. A worker does at most one thing at a
     * time, so the result is unambiguous. */
    bool worker_assignment(const std::string &worker, std::string &agenda_id, std::string &task_id,
                           bool &is_verify) const;

    void set_max_attempts(int n); /* total dispatches per task before terminal Fail (>=1) */
    int  max_attempts() const;

    /* Per-task deadline. 0 (default) = off. A task Assigned/Running/Verifying longer than `ms` is treated
     * as a stuck-worker loss — check_deadlines() removes the (alive but presumed-hung) worker from
     * scheduling and reassigns its task to a survivor (or fails it past the attempt budget). Applies
     * across all active agendas. The driver calls check_deadlines() on its periodic wake. */
    void                set_task_deadline_ms(uint64_t ms);
    std::vector<Intent> check_deadlines();

private:
    struct Impl;
    Impl *p_;
};

} // namespace hc::orch

#endif /* HC_ORCH_ENGINE_HPP */
