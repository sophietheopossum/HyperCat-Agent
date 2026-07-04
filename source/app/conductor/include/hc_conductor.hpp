#ifndef HC_CONDUCTOR_HPP
#define HC_CONDUCTOR_HPP

/* hc_conductor — the Conductor component (Conductor P3b): HyperCat's conversational front-door agent, on its
 * OWN thread, cut from the host_bridge component mold (own thread + a bounded inbox + a mutex-guarded snapshot
 * the host frame loop copies). It holds ONE hc_agent loop (P3 = conversation only; the fleet-driving tools are
 * P4) and a GoalStore, persists its conversation to an hc_store session, and resumes from a stored session +
 * the goal WAL on restart.
 *
 * Owns:      one hc_agent (built over the supplied backend) + one dedicated thread + a bounded inbox + the
 *            mutex-guarded snapshot + a GoalStore. Manages an hc_store conversation session (it owns the
 *            hc_session it opens/creates; it BORROWS the hc_store).
 * Borrows:   the turn backend's ctx (must outlive the conductor) and the hc_store. The fleet facades
 *            (Orchestrator/MemoryBroker/AuthGate/Decomposer) do NOT enter here directly — they arrive only
 *            through the reserved ControlPlane seam the host fills in P4 (doc 03; no God Object).
 * Threading: the blocking hc_agent_run runs on the conductor's OWN thread. The host's ~60fps frame loop never
 *            calls it — it copies a mutex-guarded snapshot() each frame (one reader). say() is the one writer
 *            of operator input (one-writer/one-reader, the host_bridge discipline). The mutex is NEVER held
 *            across the LLM call; streamed text is merged into the snapshot under brief locks. stop() signals
 *            at a turn boundary and joins (full shutdown); cancel_turn() is the lighter mid-turn stop — it sets
 *            the per-run hc_agent_cancel flag to abort the in-flight hc_agent_run and KEEPS the thread alive.
 * Lifetime:  ConductorView (from snapshot()) is a COPY the caller owns. ~Conductor stops + joins the thread,
 *            frees the agent, and closes its session. Construct via create(); nullptr on failure.
 */

#include "hc_agent.h"  /* hc_agent_backend — the turn source */
#include "hc_goal.hpp" /* Goal (surfaced in the view), GoalStore (owned) */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct hc_store; /* borrowed: the conversation session lives here */

namespace hc::conductor {

/* The ControlPlane seam (doc 03 / doc 12): the conductor's host-facing tool bodies call THROUGH these
 * host-supplied callbacks, so the single place direct facade calls would become bus RPC (if the conductor is
 * ever externalized) is here — earned by factoring, not a God Object. The host fills the members it wants live;
 * an UNSET member makes its tool return a graceful "not available". The current set spans the fleet/agenda ops,
 * memory, reasoning, fleet-shaping (W2), skills (W6), the Music Player (Phase D), and the opt-in third-party
 * tool bridge (D4c) — see the members below for the authoritative, current list. (The set_goal/update_goal tools
 * use the conductor's own GoalStore, and ask_user uses the operator inbox, so those are NOT in this seam.) */
struct ControlPlane {
    /* The host fills these in P5 with thin lambdas that call the in-host facades DIRECTLY (Orchestrator /
     * MemoryBroker / AuthGate / planner / reasoner) — this struct IS the doc-03 "single place where direct
     * facade calls would become bus RPC if the conductor is ever externalized." The conductor invokes them from
     * its OWN thread; each may BLOCK (an LLM round-trip, a dispatch, a gated write). An UNSET member => the
     * matching tool returns a graceful "not available" (never a crash).
     *
     * Threading contract (binding): the host sets these ONCE in Conductor::create() BEFORE the conductor thread
     * starts; they are read-only on the conductor thread thereafter and must be safe to invoke from it (the
     * facades they wrap carry their own synchronization). Plain string/bool signatures keep app/conductor free
     * of every facade type. */
    std::function<std::string(const std::string &goal_text)>                             plan_goal;     /* read-only plan preview ("" on failure)            */
    std::function<std::string(const std::string &goal_text, const std::string &goal_id)> run_agenda;    /* decompose+dispatch -> new agenda id ("" on fail)   */
    std::function<std::string(const std::string &agenda_id)>                              agenda_status; /* read-only human-readable status ("" if unknown)   */
    std::function<std::string(const std::string &agenda_id)>                              agenda_results;/* read-only: an agenda's task results + artifact refs, defanged+bounded ("" if unknown) */
    std::function<std::string(const std::string &artifact_id)>                            read_artifact; /* read-only: one content-addressed artifact's body, defanged+bounded ("" if absent)      */
    std::function<bool(const std::string &agenda_id)>                                     cancel_agenda; /* cancel one agenda (the steer_or_cancel tool)      */
    std::function<std::string(const std::string &query)>                                  recall_memory; /* read-only fenced reference text ("" if none)      */
    std::function<bool(const std::string &scope, const std::string &text)>                write_memory;  /* "self" auto / "shared" host-gated                 */
    std::function<std::string(const std::string &query)>                                  deep_reason;   /* the staged reasoning-chain result                 */
    /* W2 P2.4: fleet management (the operator's "tools for the conductor" ask) — add/remove/list workers. */
    std::function<std::string(const std::string &role)>                                   add_worker;    /* -> the new worker id ("" on fail)                 */
    std::function<bool(const std::string &id)>                                            remove_worker; /* reap + de-authorize a worker                       */
    std::function<std::string()>                                                          list_workers;  /* human-readable roster                             */
    /* W6 P6.3: per-project skills — the conductor can list + author skills (the operator's "author/list" ask). */
    std::function<std::string()>                                                          list_skills;   /* human-readable skill catalog                      */
    std::function<bool(const std::string &name, const std::string &description)>          create_skill;  /* create a skill from a template ("" name => false) */
    /* Music Player (Phase D): the conductor "set the mood" tools — GATED (opt-in via settings.conductor_mood_
     * enabled). When disabled each returns a "not enabled" note, so the conductor KNOWS the capability exists
     * but cannot see the library; when enabled the metadata it sees is DEFANGED + the reads are jailed. */
    std::function<std::string()>                                                  list_audio;    /* defanged catalog (or disabled/empty); re-scans per call (gating limits frequency) */
    std::function<std::string(const std::string &track, const std::string &vibe)> set_mood;      /* play a library track to set a vibe                 */
    std::function<std::string(const std::string &action, int level)>              control_audio; /* pause/resume/stop/volume                           */
    /* D4c (opt-in via settings.thirdparty_tools_conductor): invoke an installed THIRD-PARTY tool by name. The
     * host implementation does the bounded bus round-trip to the ToolHost on a conductor-owned bus client, and
     * for a `sensitive` call routes through the operator AuthGate FIRST (deny-by-default). Returns the tool's
     * result, or a clear "denied"/"error"/"unavailable" string. Unset => the conductor got no third-party tools. */
    std::function<std::string(const std::string &name, const std::string &args, bool sensitive)> invoke_third_party;
};

/* D4c: one third-party tool function the conductor is given (opt-in). The host fills these from the ToolHost's
 * confirmed functions at conductor-build time; the conductor registers one proxy hc_agent_tool per entry whose
 * invoke routes through ControlPlane::invoke_third_party. (Display/registration data only — author-supplied
 * strings; the spec is the model-facing schema.) */
struct ThirdPartyToolDef {
    std::string name;
    std::string spec_json;
    bool        sensitive = false; /* the call needs the per-call operator gate (manifest-declared/envelope) */
};

/* One completed conversation message, for display in the snapshot (the full transcript lives in hc_store). */
struct ConductorTurn {
    std::string   role; /* "user" | "assistant" */
    std::string   text;
    std::uint64_t at_ms = 0; /* wall-clock ms (since epoch) when the turn settled; 0 = unknown (a resumed/seeded
                              * message — hc_store keeps no per-message time). Display-only; never fed to the model. */
};

/* A consistent, mutex-guarded copy of the conductor's state for the host frame loop. */
struct ConductorView {
    std::vector<ConductorTurn> conversation;        /* the recent conversation tail (bounded) */
    std::string                streaming;           /* the in-progress assistant text; "" when idle */
    bool                       busy = false;        /* a turn is currently running */
    std::string                active_tool;         /* the most recent tool the model called this turn (drives the
                                                     * chat tool cue, P5-S2); set on tool dispatch, cleared per turn */
    std::vector<Goal>          goals;               /* the current goals (copied from the GoalStore) */
    int                        turns_used = 0;       /* operator turns processed this session */
    int                        max_turns = 0;        /* the per-session budget; 0 => unlimited */
    bool                       budget_reached = false;
    int                        continuations_used = 0; /* #8: autonomous continuation turns admitted this session */
    int                        max_continuations = 0;  /* #8: the continuation allowance (0 => unlimited) */
    std::string                notice;              /* a transient notice (budget reached, resume summary) */
};

struct ConductorParams {
    hc_agent_backend backend{};        /* the turn source: a stub in tests, hc_agent_hosted_backend(llm) live.
                                          The backend's ctx is BORROWED and must outlive the conductor. */
    std::string      system_prompt;
    std::string      wal_dir;          /* the GoalStore's host-private dir; "" => ephemeral (no goal journaling) */
    hc_store        *store = nullptr;  /* borrowed; the conversation session lives here; nullptr => no persistence */
    std::string      session_id;       /* resume this conversation session, or "" => start a fresh one */
    std::string      new_title;        /* title for a FRESH session (when session_id is empty / load fails); ""=>"conductor" */
    int              max_turns = 0;    /* per-session turn budget (0 => unlimited) — the seam P6 hardens */
    int              max_continuations = 0; /* #8: autonomous follow-up-turn budget (0 => unlimited); an operator
                                             * turn re-arms it. The host sets a finite default via HC_CONDUCTOR_CONTINUATIONS. */
    ControlPlane     control;          /* the host-filled facade seam; unset members return a graceful "not
                                          * available" (never a crash). Set once before create() starts the thread. */
    std::vector<ThirdPartyToolDef> third_party_tools; /* D4c: the opt-in third-party functions to register as
                                                       * conductor proxies (empty => none; the default). */
};

class Conductor {
public:
    static std::unique_ptr<Conductor> create(ConductorParams params); /* nullptr on failure (e.g. agent OOM) */
    ~Conductor();
    Conductor(const Conductor &) = delete;
    Conductor &operator=(const Conductor &) = delete;

    /* Enqueue an operator turn (non-blocking; the conductor thread runs it). Returns false if rejected — the
     * inbox is full, the conductor is stopping, the line is empty, or the session budget is reached. */
    bool say(const std::string &line);

    /* #8 continuation: inject a SYSTEM-authored follow-up turn (e.g. "an agenda you started finished") so the
     * conductor reasons about the event and decides the next step WITHOUT the operator typing. Non-blocking;
     * charged against a SEPARATE continuation allowance, so an autonomous chain cannot exhaust the human-
     * conversation budget. Returns false if rejected — stopping, the event queue is full, or the continuation
     * allowance is exhausted (a runaway-loop backstop; an operator say() re-arms it). The CALLER must pre-frame
     * the text as DATA, not an instruction — notify_event adds no framing; it only enqueues + budgets. Over-long
     * text is rejected by a defensive per-event byte cap, so a future caller cannot stream unbounded text in. */
    bool notify_event(const std::string &text);

    /* A consistent snapshot for the host frame loop (copies under the mutex; never blocks on the LLM). */
    ConductorView snapshot() const;

    /* The conductor's own conversation session id ("" if it runs without persistence). */
    std::string session_id() const;

    /* P3a: rename the LIVE conversation's session. Non-blocking — the new title is applied on the conductor thread
     * (the sole owner of the hc_session) within one loop wake, so a host-thread write never races an append. */
    void set_session_title(const std::string &title);

    void stop(); /* idempotent; signals the thread at the next turn boundary and joins it */

    /* Abort the turn currently in flight WITHOUT ending the session — the operator's "stop generating" button.
     * Sets the per-run hc_agent_cancel flag the conductor thread polls inside hc_agent_run; the worker settles
     * whatever streamed so far and loops back for the next message (the thread is NOT joined). Non-blocking,
     * idempotent, and a no-op when idle (each new turn clears the flag before it runs). */
    void cancel_turn();

private:
    Conductor();
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace hc::conductor
#endif /* HC_CONDUCTOR_HPP */
