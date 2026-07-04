#ifndef HC_APP_PROJECT_SESSION_HPP
#define HC_APP_PROJECT_SESSION_HPP

/* project_session — owns the PER-PROJECT runtime bring-up + teardown (Wave 3). ONE responsibility: given the
 * truly-global layer (the bus broker + supervisor + operator settings + role table, all BORROWED) and a project
 * subtree, stand up the per-project orchestrator + fleet + host services + memory + conductor rooted under that
 * subtree, and tear them down in the exact documented order. main.cpp constructs ONE ProjectSession instead of
 * ~12 interdependent locals, and the riskiest part of the host — the teardown ORDER — lives in one dtor.
 *
 * Ownership: the session OWNS the orchestrator, the fleet, the conductor, the two chat clients (planner +
 * conductor), and the HostServices' C handles (adapter/gate/ws/store/artifacts/memory/mbroker). It BORROWS the
 * Supervisor, the SettingsState, and the RoleState (the global layer main owns) — those MUST outlive the session,
 * so the session is destroyed BEFORE them (main's teardown deletes the session, then secrets, then sup, broker).
 * The bus broker is up + the host ids authorized before open() is called; the session connects its adapters/orch
 * over `sock`. Threading: built + torn down on the host thread; the orchestrator/conductor run their own threads
 * behind their APIs (the dtor joins them in order). See app/main.cpp + Docs/Plan_HyperCat/P3.1-projectsession-blueprint.md. */

#include "hc_fleet.hpp"      /* Fleet (unique_ptr member) */
#include "hc_replan.hpp"     /* hc::planner::Decomposer (value member) */
#include "host_services.hpp" /* HostServices, SettingsState, RoleState */
#include "host_storage.hpp"  /* StorageRoots (value member) */

#include <memory>
#include <string>

namespace hc {
class Orchestrator;
class Supervisor;
class BusClient; /* D4c: the conductor's third-party tool.invoke client held below */
namespace conductor {
class Conductor;
}
} // namespace hc
struct hc_llm;
struct hc_http;
struct hc_store;

namespace hcapp {

class ProjectSession {
public:
    /* Bring up the per-project runtime over `project_dir` (= <data_dir>/projects/<id>). Borrows the GLOBALS
     * (sup/settings/role_state — must outlive the session) + `sock` (the live bus) + `ephemeral` (the global
     * flag, drives the WAL/conductor "no journaling" branch). Returns nullptr if the orchestrator/fleet fail to
     * come up (the caller exits, as main did). The conductor's durable goals + the per-project stores all land
     * under project_dir. `player`/`audio` are the GLOBAL Music Player engine + audio-library jail (main owns +
     * frees them; may be null) — set on the services so the panel + the conductor mood tool reach them. */
    static ProjectSession *open(const std::string &project_dir, bool ephemeral, hc::Supervisor *sup,
                                SettingsState *settings, RoleState *role_state, hc_audio_player *player,
                                hc_sandbox *audio, const std::string &sock, ToolHost *toolhost,
                                const std::string &tools_root);

    ~ProjectSession(); /* explicit ordered teardown (the blueprint's T1..T12) */
    ProjectSession(const ProjectSession &) = delete;
    ProjectSession &operator=(const ProjectSession &) = delete;

    /* Accessors — main keeps the UI/headless/screenshot BRANCHING; the session provides the wired objects. */
    hc::Orchestrator              &orch() { return *orch_; }
    hc::Supervisor                &sup() { return *sup_; } /* the borrowed global, re-exposed for the loop signature */
    Fleet                         &fleet() { return *fleet_; }
    HostServices                  &services() { return svc_; } /* mutable: main sets svc.pty for the live branch */
    const FleetEnv                &env() const { return info_; }
    const hc::planner::Decomposer &plan_fn() const { return plan_fn_; }
    int                            replan_budget() const { return replan_budget_; }
    const std::string             &wal_dir() const { return wal_dir_; }

private:
    ProjectSession() = default;

    /* P2 conductor conversations — the runtime conversation SWITCH (and the shared teardown/build the dtor + open
     * reuse). All three run on the HOST thread (the dispatch loop = the same thread as fill_conductor), so
     * svc_.conductor is never observed dangling mid-swap. */
    void teardown_conductor();  /* cancel in-host waiters, unbind the settle observer, stop+join, free conductor + llm */
    void install_conductor(const std::string &session_id, const std::string &new_title); /* build+start, re-bind observer (build_conductor does), publish on svc_, persist the active id */
    bool switch_conductor_conversation(const std::string &session_id); /* teardown + install("" => new chat); false if no store */

    /* Borrowed globals (NOT owned — never freed here). */
    hc::Supervisor *sup_ = nullptr;
    SettingsState  *settings_ = nullptr;
    RoleState      *role_state_ = nullptr;

    /* Owned + per-project. The dtor frees these in the explicit T1..T12 order; member-destruction is a no-op. */
    std::string                               project_dir_;
    StorageRoots                              roots_;
    std::string                               wal_dir_;
    hc::Orchestrator                         *orch_ = nullptr; /* OWNED (delete at T3 — the precise join point) */
    std::unique_ptr<Fleet>                    fleet_;
    FleetEnv                                  info_;
    HostServices                              svc_; /* value; OWNS the C handles inside (closed T8..T12) */
    hc_http                                  *planner_http_ = nullptr;
    hc_llm                                   *planner_llm_ = nullptr;
    hc::planner::Decomposer                   plan_fn_;
    int                                       replan_budget_ = 0;
    std::string                               sock_;        /* the live bus socket path — needed to (re)open the
                                                              * conductor's third-party tool client on each switch (D4c) */
    std::unique_ptr<hc::conductor::Conductor> conductor_;
    hc_llm                                   *conductor_llm_ = nullptr;
    hc_http                                  *cond_http_ = nullptr;
    hc::BusClient                            *cond_tool_bus_ = nullptr; /* D4c: the conductor's third-party tool.invoke
                                                              * client (build_conductor opens it; freed AFTER the
                                                              * conductor in teardown — its seam runs on that thread) */
    hc_store                                 *conductor_store_ = nullptr; /* P1: the DEDICATED per-project conductor
                                                * conversation store (projects/<id>/conductor_sessions/); the worker
                                                * Session browser keeps svc_.store. Closed at teardown after the
                                                * conductor (its session borrows this). Null => no persistence. */
};

} // namespace hcapp

#endif /* HC_APP_PROJECT_SESSION_HPP */
