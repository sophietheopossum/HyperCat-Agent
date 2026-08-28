#ifndef HC_FLEET_HPP
#define HC_FLEET_HPP

/* hc_fleet — the host's worker-fleet manager (C++ host module).
 *
 * Purpose:   the single owner of "which workers exist + what the orchestrator's pool is." It holds the
 *            roster (id -> role + spawn config) and the spawn/arg-building main() used to inline, so a
 *            runtime add/remove (P2.2) is ONE call here instead of scattered edits across the host shell.
 * Owns:      the roster (value-owned WorkerDefs) + the resolved FleetEnv. It BORROWS the Supervisor (process
 *            lifecycle — it never owns spawn/reap), the RoleTable, and the Settings (per-role prompt/tool/model
 *            resolution); all three must outlive the Fleet.
 * Threading: the roster is mutex-guarded. As of P2.4 it is mutated from TWO threads — the host/UI thread
 *            (provision + UI add/remove) AND the conductor thread (its add_worker/remove_worker tools) — while
 *            either may also read pool(). Every public method locks, and add_worker(WorkerDef) RE-CHECKS the id
 *            under the lock before spawning, so a concurrent add of the same id from the other thread loses
 *            cleanly (returns ""). The fleet lock is NEVER held across a Supervisor call (spawn/reap/is_alive
 *            take the supervisor's own lock) — no nested fleet-mu -> supervisor-mu, so no lock-order inversion.
 *            The on_change callback is fired OUTSIDE the lock, so it runs on whichever thread mutated (see
 *            set_on_change). No method is called re-entrantly from the supervisor.
 */

#include "hc_roles.hpp"    /* RoleTable, RoleDef, RoleTool */
#include "hc_settings.hpp" /* Settings (role_models, exec_allow) */

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace hc {
class Supervisor; /* borrowed, fwd-declared */
}

namespace hcapp {

/* (worker bus id, role) — what the orchestrator round-robins; same shape as host_services' Pool. */
using FleetPool = std::vector<std::pair<std::string, std::string>>;

/* One worker's definition. A role it instantiates; P2.3 adds optional per-instance prompt/tool/model overrides. */
struct WorkerDef {
    std::string id;   /* bus id, e.g. "agent:A" */
    std::string role; /* the RoleTable key it instantiates */
};

/* The spawn environment, resolved ONCE at Fleet::create (live mode + the provisioned roots). The host reads
 * these back — the pty/services/snapshot need ws_root/sessions_root/live/model/shared_workspace. */
struct FleetEnv {
    bool        live = false;
    const char *model = nullptr;       /* HC_MODEL — the global fallback model id (borrowed from environ; set
                                        * ONCE at startup before Fleet::create — do NOT setenv(HC_MODEL) after) */
    bool        shared_workspace = false;
    std::string ws_root;               /* "" if not provisioned / offline */
    std::string sessions_root;         /* "" if not provisioned */
    /* W6 P6.2: per-project Skills (same for every worker in the project). `skills_dir` = the jailed skills/ root
     * (the load_skill tool reads bodies from it); `skills_catalog` = the host-built, already fenced+defanged
     * catalog block appended to each worker's prompt — BOUNDED to 16 KiB by format_skills_catalog (kCatalogCap),
     * so it fits well within ARG_MAX when passed as the single --skills-catalog argv. Both "" => skills off. */
    std::string skills_dir;
    std::string skills_catalog;
};

/* A worker's live status for the UI roster. */
struct WorkerStatus {
    std::string id;
    std::string role;
    std::string state; /* "ready" | "spawned" | "dead" */
};

/* ---- pure spawn-arg helpers (no Supervisor; unit-tested directly) ---- */

/* The operator's per-role MODEL override (settings.role_models), or "" if the role is unassigned. */
std::string model_override(const Settings &, const std::string &role);

/* Resolve a role's chat model id (live only): operator role_models > the role table's built-in model > the
 * global model; "" if none applies. Shared by the fleet, the planner, and the conductor so the per-role-model
 * rule lives in ONE place. Only the model ID is resolved — the key never threads here (it rides the env). */
std::string resolve_role_model(const RoleTable &, const Settings &, const std::string &role,
                               const char *global_model);

/* The operator's per-role PROVIDER-ROUTING override (settings.role_providers), or "" if unassigned. */
std::string provider_override(const Settings &, const std::string &role);

/* Resolve a role's OpenRouter provider-routing block (the INNER object, canonical JSON): operator
 * role_providers > `global_provider` (the caller passes HC_OPENROUTER_PROVIDER); "" if neither applies,
 * which means free routing. TWO tiers, not three: the RoleTable carries no provider, deliberately — the
 * two roles this exists for (planner, conductor) are not RoleTable entries at all, so a table tier would
 * be a permanent no-op for exactly the cases that motivated the feature.
 *
 * Takes no RoleTable for the same reason, and takes the global as a parameter rather than calling getenv
 * so app/fleet stays pure and unit-testable — matching how `global_model` is threaded above. */
std::string resolve_role_provider(const Settings &, const std::string &role, const char *global_provider);

/* The per-worker `extra` spawn args carrying spawn-time identity: --model (live, resolved) + --provider
 * (live, resolved routing block) + --role + --role-prompt (the overlay APPENDED after the base) +
 * --role-tools (the tool-id subset). An empty overlay/csv is OMITTED, so an unconfigured role spawns
 * exactly as the base (base prompt, all tools, free routing). */
std::vector<std::string> role_spawn_args(const RoleTable &, const Settings &, const std::string &role,
                                         bool live, const char *global_model,
                                         const char *global_provider = nullptr);

class Fleet {
public:
    /* Resolve live mode (HC_MODEL + OPENROUTER_API_KEY present) + HC_SHARED_WORKSPACE, provision the workspace
     * + session roots 0700 (the sandbox needs its root to exist), and return an EMPTY fleet (no workers yet).
     * Borrows sup/roles/settings — they MUST outlive the Fleet. `roles_mu` (borrowed, may be null) guards the
     * role table: P2.3b lets the operator EDIT roles at runtime (on the host thread) while add_worker reads the
     * table at spawn (possibly on the conductor thread, P2.4), so the Fleet locks it around that read. Null =>
     * no locking (a single-threaded test/headless caller with an immutable table). The raw roots come from
     * host_storage (the active project subtree). Never null (provisioning failures degrade + log, like before). */
    static std::unique_ptr<Fleet> create(hc::Supervisor *sup, const RoleTable *roles, const Settings *settings,
                                          const std::string &ws_root_raw, const std::string &sessions_root_raw,
                                          std::mutex *roles_mu = nullptr);
    ~Fleet();
    Fleet(const Fleet &) = delete;
    Fleet &operator=(const Fleet &) = delete;

    /* Spawn + register a worker (its role resolves model/prompt/tools). False if the id is already in the
     * roster or the spawn failed (the roster is then unchanged). Non-blocking — poll roster()/wait_ready(). */
    bool add_worker(const WorkerDef &def);

    /* Add a worker of `role` at the next FREE id from id_pool() — the operator picks a role; the slot id is
     * auto-assigned (an internal detail). Returns the assigned id, or "" if the pool is exhausted or the
     * spawn failed. The host UI "+ add worker" path. */
    std::string add_worker(const std::string &role);

    /* The fixed pool of potential worker ids (agent:A .. agent:P) — add_worker(role) assigns the next FREE
     * slot from it, so it BOUNDS the fleet size. (The bus known-fleet filters track the LIVE roster via
     * sync_fleet_filters, NOT this pool — a static superset would let a same-uid peer squat an unspawned id.) */
    static const std::vector<std::string> &id_pool();

    /* Deliberately retire a worker: reap its process + REVOKE its bus routing (Supervisor::reap — so the freed
     * id can't be squatted), then drop it from the roster. False if the supervisor does not know the id. BLOCKS
     * briefly (reap waits for the process to exit, escalating SIGTERM->SIGKILL). A worker mid-task degrades into
     * the orchestrator's existing reassignment (its liveness goes false). */
    bool remove_worker(const std::string &id);

    /* Install a callback fired AFTER any add/remove changes the roster, with the new live ids(). The host uses
     * it to keep the bus known-fleet filters (sync_fleet_filters) in step — so a worker added by EITHER the UI
     * or a conductor tool is recognized, and a removed worker's id leaves the filters. Set ONCE at startup
     * (before the UI/conductor run, read-only after); fired on whichever thread calls add/remove, OUTSIDE the
     * roster lock (the callback is thread-safe). Pass nullptr to clear (teardown does, before the adapters free). */
    void set_on_change(std::function<void(const std::vector<std::string> &ids)> cb);

    /* W6 P6.2: set the per-project Skills passed to every worker spawned hereafter — the jailed skills/ dir
     * (load_skill reads bodies) + the host-built fenced catalog (appended to the prompt). Call ONCE at setup,
     * BEFORE any worker is spawned (read-only after; not synchronized — same single-threaded-setup contract as
     * set_on_change). Empty strings => skills disabled. */
    void set_skills(std::string skills_dir, std::string skills_catalog);

    /* Bounded wait until every rostered worker has checked in (used right after provisioning). */
    bool wait_ready(int timeout_ms);

    FleetEnv                  env() const;    /* the resolved live/model/roots (host reads these back)  */
    FleetPool                 pool() const;   /* (id, role) snapshot — the orchestrator's pool          */
    /* The KNOWN role-table keys (dev/qa/research/ops/generalist + any operator-defined roles) — the PLANNER's
     * capability vocabulary, independent of which workers are currently live. Distinct from pool() (the live roster):
     * a goal is planned against the roles that CAN exist, then the conductor provisions workers for them. This is the
     * fix for "empty fleet -> the planner is told 'pick from []' -> it invents an unmatchable capability." Guarded by
     * the borrowed roles_mu (the table is editable at runtime); takes ONLY roles_mu (no fleet-mu -> no lock-order tie). */
    std::vector<std::string>  known_roles() const;
    std::vector<std::string>  ids() const;    /* the worker ids (e.g. the memory-broker fleet set)      */
    std::vector<WorkerStatus> roster() const; /* live status for the UI fleet panel                     */

private:
    Fleet();
    struct Impl;
    Impl *p_;
};

} // namespace hcapp

#endif /* HC_FLEET_HPP */
