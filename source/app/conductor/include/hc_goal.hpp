#ifndef HC_GOAL_HPP
#define HC_GOAL_HPP

/* hc_goal — the durable Goal aggregate + GoalStore (Conductor P3a).
 *
 * A Goal is the long-running unit ABOVE an agenda: it survives host restarts, spans multiple agendas over its
 * life, and is steerable. It is created DELIBERATELY (after a discussion settles into work), never per turn —
 * a conversation that never produces a goal persists only as the conductor's transcript. A Goal sits at the
 * altitude of orch::Agenda (an application-domain aggregate), so it is C++ in app/, NOT a libs/ primitive.
 *
 * GoalStore persists goals on the SHIPPED libs/hc_wal (one append-only stream per goal) with the same
 * journal-before-act + pure-fold recovery the orchestrator uses for agendas (app/orchestrator, P14): hc_wal
 * does domain-agnostic durable lines, GoalStore owns the goal record schema + the fold. Nothing is added to
 * libs/. Every field is serialized through hc_json, so untrusted operator/title text can never forge a record
 * boundary. Durability is FAIL-CLOSED: a mutation is applied in memory ONLY after its journal append succeeds,
 * so the in-memory set and the on-disk log never diverge (in ephemeral mode there is nothing to journal).
 *
 * Owns:      the in-memory goal set + one hc_wal handle per live (non-settled) goal.
 * Borrows:   nothing. `wal_dir` is a host-private 0700 directory the caller provisions (outside every agent
 *            sandbox); an EMPTY wal_dir means EPHEMERAL — goals live only in memory, nothing is journaled or
 *            recovered (mirrors the orchestrator's ephemeral mode).
 * Threading: single-owner, NOT thread-safe — the conductor's own thread is the sole owner (same contract as
 *            hc_store / hc_wal's single-writer-per-stream). Goal ids are conductor-generated + traversal-safe
 *            (an id becomes a WAL stream id; hc_wal also re-validates).
 * Lifetime:  ~GoalStore closes every open WAL handle but LEAVES non-settled streams on disk for the next
 *            restart's recover_incomplete(); a settled goal's stream is removed at the transition. Goal values
 *            returned by create/get/list are COPIES the caller owns.
 */

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hc::conductor {

enum class GoalStatus { Discussing, Active, Paused, Done, Failed, Abandoned };

/* Stable wire tokens for the WAL record (lowercase, ascii) — NOT localized display text. */
const char *goal_status_str(GoalStatus s);
GoalStatus  goal_status_from_str(const char *s); /* unknown / null -> Discussing */
bool        goal_status_is_terminal(GoalStatus s); /* Done | Failed | Abandoned */

struct Goal {
    std::string              id;          /* stable, traversal-safe, conductor-generated */
    std::string              title;       /* human-readable */
    std::string              text;        /* the goal statement (operator-stated / confirmed) */
    GoalStatus               status = GoalStatus::Discussing;
    std::vector<std::string> agenda_ids;  /* the agendas spawned to pursue it (0..N over its life) */
    std::string              session_id;  /* the conductor conversation session this goal lives in */
    std::string              source;      /* provenance: "operator" | "conductor" */
};

class GoalStore {
public:
    /* wal_dir: an existing host-private dir for the per-goal WAL streams; "" => ephemeral (no journaling). */
    explicit GoalStore(std::string wal_dir);
    ~GoalStore();
    GoalStore(const GoalStore &) = delete;
    GoalStore &operator=(const GoalStore &) = delete;

    /* Create a goal: generate a stable id, journal an `open` baseline, hold it live (status Active). Returns
     * the new Goal, or nullopt if the live-goal cap is reached or (durable mode) the journal append fails
     * (fail-closed — nothing is held if it could not be made durable). `session_id` links the conversation. */
    std::optional<Goal> create(const std::string &title, const std::string &text,
                               const std::string &source, const std::string &session_id);

    /* Transition a goal's status. A NON-terminal change journals a `status` delta; a TERMINAL status
     * (Done/Failed/Abandoned) journals a `settled` record then removes the goal's stream (recovery will not
     * resurrect it). A goal ALREADY in a terminal status is FINAL — any transition out of it returns false (a
     * settled goal can never be flipped back to live). Fail-closed: applied in memory only if journaled.
     * Returns false on an unknown id or a journal failure. */
    bool set_status(const std::string &goal_id, GoalStatus status);

    /* Attach an agenda id to a goal (journal an `agenda` delta). Idempotent: already-attached => true, no
     * journal. A goal's agenda_ids are bounded (so a corrupted WAL cannot spike recovery); past the bound this
     * returns false. Fail-closed. Returns false on an unknown id or a journal failure. */
    bool attach_agenda(const std::string &goal_id, const std::string &agenda_id);

    std::vector<Goal>   list() const;                    /* all retained goals, in creation order */
    std::optional<Goal> get(const std::string &goal_id) const;
    std::size_t         live_count() const;              /* non-terminal goals (counted against the cap) */

    /* Adopt goals recovered from a prior run (see recover_incomplete): re-open each one's WAL stream, journal
     * a fresh `open` baseline (so a second crash still has a complete picture), and add it to the live set so
     * journaling continues. For host-restart resume. Goals beyond the live cap are dropped (logged by caller). */
    void adopt(std::vector<Goal> recovered);

    /* Replay the incomplete goal streams under `wal_dir`, fold each into a Goal; settled / invalid streams are
     * pruned (removed). A pure recovery read (no live store needed) — seed a fresh GoalStore via adopt().
     * Empty wal_dir => no goals. */
    static std::vector<Goal> recover_incomplete(const std::string &wal_dir);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace hc::conductor
#endif /* HC_GOAL_HPP */
