#ifndef HC_ORCHESTRATOR_HPP
#define HC_ORCHESTRATOR_HPP

/* hc_orchestrator — the orchestrator driver / public facade (C++ host, pimpl).
 *
 * Purpose:   the ONLY component that touches the bus and the supervisor. It connects a BusClient,
 *            runs one driver thread that owns the scheduling Engine, turns the engine's intents
 *            into task.assign requests, feeds bus replies / task.result requests / worker deaths
 *            back into the engine, and reports the agenda to completion. An "overseer" decomposes
 *            an agenda's goal into tasks (offline: an injected decomposer; the deep_reason/LLM
 *            decomposer is the deferred path) and the driver assigns them across a worker pool.
 * Owns:      its BusClient and its driver thread (and, on that thread, the Engine). It BORROWS the
 *            Supervisor* (the host owns it) to spawn-check liveness — it never owns process
 *            lifecycle (that is app/supervisor's job; no God Object).
 * Threading: the driver thread is the SOLE mutator of the Engine. Public methods are called from
 *            one owner thread and communicate with the driver via a mutex + a copied-out snapshot;
 *            the owner thread never touches the Engine. The destructor unblocks the driver's recv
 *            (BusClient::shutdown) and joins it — the clean-teardown pattern the supervisor uses.
 * Concurrency (Conductor P1): run_agenda admits MULTIPLE agendas that run CONCURRENTLY on the shared
 *            worker pool (one driver thread still owns the one multi-agenda Engine — no thread-per-agenda).
 *            Each agenda settles independently. Per-agenda snapshot/progress/cancel/wait_until_done + a
 *            list_agendas() expose them by id; the no-id overloads remain, acting on the most-recently-run
 *            agenda (back-compat for single-agenda callers).
 */

#include "hc_orch_model.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hc {

class Supervisor; /* borrowed, fwd-declared (no ownership, no header coupling) */

class Orchestrator {
public:
    /* Connect a BusClient as `id` (e.g. "orchestrator") to the broker at `sock_path`, and borrow
     * `sup` to check worker liveness. Null on failure. Caller owns the returned pointer; delete it
     * (or RAII-wrap) to free — the destructor stops + joins the driver thread and disconnects.
     * P14: a non-empty `wal_dir` (a 0700 host-private dir under the data dir, outside every agent
     * sandbox) turns on durable journaling — each agenda is written ahead so it survives a host crash.
     * Empty (the default) = no journaling (the offline gates + a non-durable host). */
    static Orchestrator *create(const std::string &sock_path, const std::string &id, Supervisor *sup,
                                const std::string &wal_dir = "");
    ~Orchestrator();

    /* P14 recovery: fold the INCOMPLETE agendas left in `wal_dir` by a previous (crashed) run back into
     * resumable Agendas — Done tasks keep their results, in-flight tasks come back Pending. Settled or
     * corrupt streams are cleaned up and omitted. Call BEFORE wiring an orchestrator, then run_agenda each
     * returned Agenda on the worker pool to resume it (the already-Done work is not re-run). Empty wal_dir
     * or no incomplete streams -> an empty vector. */
    static std::vector<hc::orch::Agenda> recover_incomplete(const std::string &wal_dir);

    /* fn(goal) -> task list. Inject to keep the offline gate deterministic without an LLM; the
     * default decomposer (deep_reason over hc_llm) is the deferred path. Set before run_agenda. */
    using Decomposer = std::function<std::vector<hc::orch::Task>(const std::string &)>;

    /* Admit an agenda to run on `pool` (worker bus id -> role) CONCURRENTLY with any already-active
     * agendas on the shared pool. If agenda.tasks is empty the decomposer turns agenda.goal into tasks;
     * otherwise the given tasks are used. Non-blocking — the driver thread runs the schedule. The agenda
     * MUST carry a non-empty id, unique among the currently-active agendas. False on a bad argument (empty
     * id), a duplicate active id, or once the concurrently-live (queued + active) agenda cap is reached
     * (a runaway-caller backstop); otherwise true (admitted). */
    bool run_agenda(const hc::orch::Agenda &agenda,
                    const std::vector<std::pair<std::string, std::string>> &pool);

    enum class Verdict { Running, Done, Failed };

    /* Bounded wait for an agenda to settle. The no-id form waits for the MOST-RECENTLY-RUN agenda
     * (back-compat); the id form waits for that specific agenda. Returns the verdict; on timeout returns
     * Running (never blocks forever — mirrors the bus recv-timeout discipline). An UNKNOWN id returns
     * Running IMMEDIATELY (it does not block for the timeout) — register the agenda with run_agenda before
     * waiting on its id, so a caller never busy-polls a not-yet-known id. */
    Verdict wait_until_done(int timeout_ms);
    Verdict wait_until_done(const std::string &agenda_id, int timeout_ms);

    /* Per-agenda views. The no-id forms act on the most-recently-run agenda (back-compat). */
    int              progress();                              /* 0..100 of the most-recent agenda    */
    int              progress(const std::string &agenda_id);  /* 0..100 of that agenda (0 if unknown) */
    hc::orch::Agenda snapshot();                              /* the most-recent agenda's state      */
    hc::orch::Agenda snapshot(const std::string &agenda_id);  /* that agenda's state (empty if gone) */

    /* Active + recently-settled agenda ids (admit order), for a conductor's agenda_status. */
    std::vector<std::string> list_agendas();

    /* Request cancellation of an active agenda: its non-terminal tasks fail, its workers free, it settles
     * Failed. False if the id is not currently active. */
    bool cancel(const std::string &agenda_id);

    void set_decomposer(Decomposer);

    /* W1.3: the host's deliverable-existence check. fn(agent, artifact_path) -> true iff the worker actually
     * produced (a non-empty file at) that path in its workspace. When set, a Done result for a task with a
     * non-empty artifact_path is verified; a missing/empty deliverable is converted to a failure so the agenda
     * never reports a false Done (the worker is re-prompted on the retry). The orchestrator stays free of
     * filesystem knowledge — the host fills this, like set_decomposer. Set before run_agenda. */
    using DeliverableVerifier = std::function<bool(const std::string &agent, const std::string &artifact_path)>;
    void set_deliverable_verifier(DeliverableVerifier);

    /* P04: enable sibling self-check for subsequent agendas (mode None = off, the default). With a
     * Sibling policy, each successful task result is independently re-checked by a sibling worker (never
     * the author) before the task is accepted; a refutation fails it. Set before run_agenda. */
    void set_verify(hc::orch::VerifyPolicy);

    /* #8 continuation trigger: a one-shot notification fired when an agenda SETTLES (Done/Failed), so a
     * listener (the Conductor) can react and continue a chain of work WITHOUT the operator prompting it.
     * Fired ONCE per settle, on the driver thread, with NO orchestrator lock held — so the callback may take
     * another component's mutex. The callback MUST NOT call back into this Orchestrator (it should only
     * enqueue elsewhere). Set before run_agenda; read-only on the driver thereafter. The driver may fire
     * until the Orchestrator is destroyed (~Orchestrator joins it), so UNBIND (set an empty observer) before
     * freeing anything the callback captures if that happens before this Orchestrator is destroyed. */
    struct AgendaSettled {
        std::string agenda_id;
        Verdict     verdict; /* Done or Failed (never Running) */
        std::string summary; /* concise tally, e.g. "3/3 done" or "2/3 done, 1 failed" */
    };
    using SettleObserver = std::function<void(const AgendaSettled &)>;
    void set_settle_observer(SettleObserver);

private:
    Orchestrator();
    struct Impl;
    Impl *p_;
};

} // namespace hc

#endif /* HC_ORCHESTRATOR_HPP */
