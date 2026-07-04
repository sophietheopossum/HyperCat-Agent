/* hc_fleet — see hc_fleet.hpp. The fleet manager: spawn-arg building + the roster, lifted out of main()'s
 * provision_fleet so worker lifecycle is one module. The supervisor (borrowed) does the actual fork/exec/reap;
 * this owns "who is in the fleet + how each spawns." */

#include "hc_fleet.hpp"

#include "hc_supervisor.hpp"
#include "ws_util.hpp" /* ws_subdir — per-agent vs shared jail (app/ include) */

#include <cerrno>
#include <cstdio>
#include <cstdlib> /* getenv */
#include <ctime>   /* nanosleep */
#include <mutex>
#include <sys/stat.h> /* mkdir */

namespace hcapp {

namespace {

/* A 4-line POSIX nanosleep wrapper, duplicated in a few host TUs (hc_supervisor.cpp, the gate tests) — one
 * function not worth a shared header. */
void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

/* Build a worker's spawn args = llm_args + (live: --workspace <per-agent dir>) + (--sessions) + extra.
 * Provisions the per-agent sandboxed workspace under `ws_root` (which must already exist — the sandbox
 * refuses to create its own root). A mkdir failure other than EEXIST is non-fatal: the agent spawns WITHOUT
 * a workspace (no fs_write tool) rather than being jailed to a wrong or shared directory. (Verbatim from the
 * former main.cpp::agent_args — the host shell's spawn-arg composition.) */
std::vector<std::string> agent_args(const std::string &id, bool live, bool shared_ws,
                                    const std::string &ws_root, const std::string &sessions_root,
                                    const std::string &skills_dir, const std::string &skills_catalog,
                                    const std::vector<std::string> &llm_args, std::vector<std::string> extra)
{
    std::vector<std::string> a = llm_args;
    if (live && !ws_root.empty()) { /* the fs_write workspace is a live-mode feature */
        /* per-agent jail by default; one shared dir for the whole fleet under W3's opt-in (ws_subdir) */
        std::string wdir = ws_root + "/" + ws_subdir(id, shared_ws);
        if (mkdir(wdir.c_str(), 0700) == 0 || errno == EEXIST) {
            a.push_back("--workspace");
            a.push_back(wdir);
        } else {
            std::fprintf(stderr, "host: %s gets no workspace (mkdir failed)\n", id.c_str());
        }
    }
    if (!sessions_root.empty()) { /* persist transcripts in BOTH modes (echo sessions list too) */
        a.push_back("--sessions");
        a.push_back(sessions_root);
    }
    /* W6 P6.2: per-project Skills (same for every worker). Non-secret, so they ride argv: the jailed skills/
     * dir (load_skill reads bodies) + the host-built fenced catalog (appended to the prompt). */
    if (!skills_dir.empty()) {
        a.push_back("--skills-dir");
        a.push_back(skills_dir);
    }
    if (!skills_catalog.empty()) {
        a.push_back("--skills-catalog");
        a.push_back(skills_catalog);
    }
    a.insert(a.end(), extra.begin(), extra.end());
    return a;
}

} // namespace

/* ---- pure spawn-arg helpers ---- */

std::string model_override(const Settings &s, const std::string &role)
{
    auto it = s.role_models.find(role);
    return (it != s.role_models.end()) ? it->second : std::string();
}

std::string resolve_role_model(const RoleTable &roles, const Settings &settings, const std::string &role,
                               const char *global_model)
{
    std::string m = model_override(settings, role);
    if (m.empty())
        if (const RoleDef *rd = roletable_find(roles, role))
            if (!rd->model.empty()) m = rd->model;
    if (m.empty() && global_model) m = global_model;
    return m;
}

/* The worker's spawn-time --role-tools csv AFTER applying the GLOBAL System Tools toggle (settings.system_tools;
 * a missing entry => ON). Start from the role's set (its explicit list, or ALL tools when the role doesn't
 * restrict), drop any tool the operator globally disabled, and render the csv. "" means "all tools" (today's
 * behaviour — emitted only when nothing is disabled AND the role was unrestricted). An all-disabled result emits
 * a non-empty sentinel so the worker registers ZERO worker tools (parse_role_tools fails safe on an unknown
 * token) rather than re-enabling everything via the "" = all-on rule. The per-role subset and this global toggle
 * compose: a tool runs only if the role allows it AND it is globally enabled (AND, for fs tools, a sandbox exists). */
static std::string effective_role_tools_csv(const RoleDef &rd, const Settings &settings)
{
    auto globally_off = [&settings](RoleTool t) {
        auto it = settings.system_tools.find(role_tool_name(t));
        return it != settings.system_tools.end() && !it->second;
    };
    std::vector<RoleTool> base;
    if (rd.tools.empty())
        for (size_t i = 0; i < role_tool_count(); i++) base.push_back((RoleTool)i);
    else
        base = rd.tools;

    std::vector<RoleTool> kept;
    bool                  any_off = false;
    for (RoleTool t : base) {
        if (globally_off(t)) any_off = true;
        else kept.push_back(t);
    }
    if (!any_off && rd.tools.empty()) return "";  /* nothing disabled + role unrestricted => all-on (today) */
    if (kept.empty()) return "none";              /* all disabled => zero tools (a non-empty sentinel, not "") */
    std::string s;
    for (size_t i = 0; i < kept.size(); i++) {
        if (i) s += ",";
        s += role_tool_name(kept[i]);
    }
    return s;
}

std::vector<std::string> role_spawn_args(const RoleTable &roles, const Settings &settings,
                                         const std::string &role, bool live, const char *global_model)
{
    std::vector<std::string> extra;
    if (live) {
        std::string model = resolve_role_model(roles, settings, role, global_model);
        if (!model.empty()) {
            extra.push_back("--model");
            extra.push_back(std::move(model));
        }
    }
    if (!role.empty()) {
        extra.push_back("--role");
        extra.push_back(role);
    }
    if (const RoleDef *rd = roletable_find(roles, role)) {
        if (!rd->prompt_overlay.empty()) {
            extra.push_back("--role-prompt");
            extra.push_back(rd->prompt_overlay);
        }
        std::string csv = effective_role_tools_csv(*rd, settings); /* role subset ∩ global System Tools toggle */
        if (!csv.empty()) { /* "" == all tools (today's behavior); only subset when the role/toggle limits */
            extra.push_back("--role-tools");
            extra.push_back(std::move(csv));
        }
    }
    return extra;
}

/* ---- the Fleet ---- */

struct Fleet::Impl {
    hc::Supervisor          *sup = nullptr;      /* borrowed */
    const RoleTable         *roles = nullptr;    /* borrowed */
    std::mutex              *roles_mu = nullptr; /* borrowed; guards *roles vs a runtime role edit (P2.3b). null => none */
    const Settings          *settings = nullptr; /* borrowed */
    FleetEnv                 env;
    std::vector<std::string> llm_args;       /* the shared spawn args (--controller [+ --exec-enabled]) */
    mutable std::mutex       mu;             /* guards roster */
    std::vector<WorkerDef>   roster;         /* guarded by mu */
    std::function<void(const std::vector<std::string> &)> on_change; /* set once at startup; fired on add/remove */
};

Fleet::Fleet() : p_(new Impl) {}
Fleet::~Fleet() { delete p_; }

std::unique_ptr<Fleet> Fleet::create(hc::Supervisor *sup, const RoleTable *roles, const Settings *settings,
                                     const std::string &ws_root_raw, const std::string &sessions_root_raw,
                                     std::mutex *roles_mu)
{
    std::unique_ptr<Fleet> f(new Fleet());
    Impl                  *im = f->p_;
    im->sup = sup;
    im->roles = roles;
    im->roles_mu = roles_mu;
    im->settings = settings;

    im->env.model = getenv("HC_MODEL");
    const char *key = getenv("OPENROUTER_API_KEY");
    im->env.live = im->env.model && *im->env.model && key && *key;
    /* W3: opt-in SHARED workspace (default-OFF). When set, the whole fleet jails to ONE ws_root/shared dir
     * instead of a per-agent dir each — a dev->qa->polish pipeline builds on the same files. Relaxes the locked
     * per-agent sandbox isolation to session-shared; sound only because the fleet is same-uid (a mediation jail,
     * not a privilege boundary) and the shared dir is still a jail. A conscious, logged choice. */
    im->env.shared_workspace = getenv("HC_SHARED_WORKSPACE") != nullptr;
    std::fprintf(stderr, "host: %s\n",
                 im->env.live ? "LIVE mode — workers run real agent turns"
                              : "offline mode — workers echo (set OPENROUTER_API_KEY + HC_MODEL for live)");
    if (im->env.shared_workspace)
        std::fprintf(stderr, "host: *** HC_SHARED_WORKSPACE ON — the fleet SHARES one workspace (per-agent "
                             "isolation relaxed to session-shared; same-uid mediation jail) ***\n");

    /* Every worker accepts task.assign only from the orchestrator (anti-injection). The MODEL is per-role
     * (resolved per worker in role_spawn_args, key via env inheritance — never argv), so the shared args carry
     * only the controller; each worker also gets its role identity + a per-agent sandboxed workspace. */
    im->llm_args = {"--controller", "orchestrator"};
    /* W4.3: register the `run` exec tool on the workers ONLY when the operator has a non-empty exec allowlist;
     * the host's ExecGate re-validates + operator-gates every run (offline it auto-denies). */
    if (!settings->exec_allow.empty()) im->llm_args.push_back("--exec-enabled");

    if (im->env.live) {
        im->env.ws_root = ws_root_raw;
        /* hc_sandbox_open requires the jail root to ALREADY exist — provision it 0700 here */
        if (mkdir(im->env.ws_root.c_str(), 0700) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "host: fs_write disabled (workspace root mkdir failed)\n");
            im->env.ws_root.clear();
        }
    }
    im->env.sessions_root = sessions_root_raw; /* both modes; persistent unless --ephemeral / fallback */
    if (mkdir(im->env.sessions_root.c_str(), 0700) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "host: session persistence disabled (sessions mkdir failed)\n");
        im->env.sessions_root.clear();
    }
    return f;
}

bool Fleet::add_worker(const WorkerDef &def)
{
    if (def.id.empty()) return false;
    std::vector<std::string> args;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (const auto &w : p_->roster)
            if (w.id == def.id) return false; /* already in the roster */
        /* P2.3b: the role table can be EDITED at runtime (host thread) while this spawn reads it (here, possibly
         * the conductor thread) — guard the read with the borrowed roles_mu so a concurrent edit can't tear the
         * role strings. Nested INSIDE p_->mu (lock order: fleet-mu -> roles-mu); EditRole takes roles_mu alone,
         * so there is no inverse path and no deadlock. null roles_mu (single-threaded caller) => no lock. */
        std::vector<std::string> extra;
        {
            std::unique_lock<std::mutex> rlk;
            if (p_->roles_mu) rlk = std::unique_lock<std::mutex>(*p_->roles_mu);
            extra = role_spawn_args(*p_->roles, *p_->settings, def.role, p_->env.live, p_->env.model);
        }
        args = agent_args(def.id, p_->env.live, p_->env.shared_workspace, p_->env.ws_root, p_->env.sessions_root,
                          p_->env.skills_dir, p_->env.skills_catalog, p_->llm_args, std::move(extra));
    }
    /* Spawn OUTSIDE the lock — fork/exec is slow-ish; a concurrent pool()/ids() reader (the conductor) must not
     * block on it. As of P2.4 add_worker can be called from TWO threads (host/UI + conductor), so the dup-check
     * above is necessary-but-not-sufficient: two threads could both pass it for the same id, both release the
     * lock, and both reach spawn. The supervisor's spawn is the authoritative dedup backstop — it already holds
     * the id, so the loser gets false here and adds nothing. On any spawn failure the roster is left unchanged
     * (the id was never pushed). */
    if (!p_->sup->spawn(def.id, args)) return false; /* fork/exec failed (or the supervisor already has the id) */
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->roster.push_back(def);
    }
    if (p_->on_change) p_->on_change(ids()); /* refresh the bus known-fleet filters — OUTSIDE the roster lock */
    return true;
}

const std::vector<std::string> &Fleet::id_pool()
{
    static const std::vector<std::string> pool = [] {
        std::vector<std::string> v;
        for (char c = 'A'; c <= 'P'; c++) v.push_back(std::string("agent:") + c); /* agent:A .. agent:P (16) */
        return v;
    }();
    return pool;
}

std::string Fleet::add_worker(const std::string &role)
{
    std::string id; /* the first pool id not already in the roster */
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (const auto &cand : id_pool()) {
            bool taken = false;
            for (const auto &w : p_->roster)
                if (w.id == cand) {
                    taken = true;
                    break;
                }
            if (!taken) {
                id = cand;
                break;
            }
        }
    }
    if (id.empty()) return ""; /* pool exhausted */
    /* add_worker(WorkerDef) re-checks the dup under the lock + spawns off the lock; "" if it lost the slot. */
    return add_worker(WorkerDef{id, role}) ? id : std::string();
}

bool Fleet::remove_worker(const std::string &id)
{
    /* reap() OUTSIDE the roster lock — it blocks (SIGTERM -> reaped, maybe escalating to SIGKILL), and a
     * concurrent pool()/roster() reader (the conductor) must not stall on it. reap is the authoritative
     * teardown (process + bus-id revoke); we drop the roster entry only once it succeeds. */
    if (!p_->sup->reap(id)) return false; /* the supervisor does not know the id */
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto it = p_->roster.begin(); it != p_->roster.end(); ++it)
            if (it->id == id) {
                p_->roster.erase(it);
                break;
            }
    }
    if (p_->on_change) p_->on_change(ids()); /* refresh the bus known-fleet filters — OUTSIDE the roster lock */
    return true;
}

void Fleet::set_on_change(std::function<void(const std::vector<std::string> &)> cb)
{
    p_->on_change = std::move(cb); /* set once at startup before the UI/conductor run -> read-only, no lock */
}

void Fleet::set_skills(std::string skills_dir, std::string skills_catalog)
{
    p_->env.skills_dir = std::move(skills_dir); /* set once at setup before any spawn -> read at agent_args, no lock */
    p_->env.skills_catalog = std::move(skills_catalog);
}

bool Fleet::wait_ready(int timeout_ms)
{
    FleetPool p = pool();
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        bool all = true;
        for (const auto &w : p)
            if (!p_->sup->is_ready(w.first)) all = false;
        if (all) return true;
        sleep_ms(20);
    }
    for (const auto &w : p)
        if (!p_->sup->is_ready(w.first)) return false;
    return true;
}

FleetEnv Fleet::env() const { return p_->env; } /* set once in create(); copied out (model is a borrowed env ptr) */

FleetPool Fleet::pool() const
{
    std::lock_guard<std::mutex> lk(p_->mu);
    FleetPool out;
    out.reserve(p_->roster.size());
    for (const auto &w : p_->roster) out.emplace_back(w.id, w.role);
    return out;
}

std::vector<std::string> Fleet::ids() const
{
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<std::string> out;
    out.reserve(p_->roster.size());
    for (const auto &w : p_->roster) out.push_back(w.id);
    return out;
}

std::vector<std::string> Fleet::known_roles() const
{
    /* Reads the borrowed role table (the planner's vocabulary), NOT the roster — so it takes ONLY roles_mu (which
     * guards a runtime role edit, P2.3b), never p_->mu. No nesting => no lock-order concern. null roles_mu (a
     * single-threaded test/headless caller with an immutable table) => no lock. */
    std::unique_lock<std::mutex> rlk;
    if (p_->roles_mu) rlk = std::unique_lock<std::mutex>(*p_->roles_mu);
    std::vector<std::string> out;
    out.reserve(p_->roles->roles.size());
    for (const auto &r : p_->roles->roles) out.push_back(r.role);
    return out;
}

std::vector<WorkerStatus> Fleet::roster() const
{
    /* Two passes so the roster lock is NEVER held while calling the Supervisor (which takes its OWN lock):
     * snapshot (id,role) under p_->mu, RELEASE it, then query liveness — no nested fleet-mu -> supervisor-mu
     * ordering for a future caller to invert into a deadlock. */
    std::vector<WorkerDef> defs;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        defs = p_->roster;
    }
    std::vector<WorkerStatus> out;
    out.reserve(defs.size());
    for (const auto &w : defs) {
        WorkerStatus s;
        s.id = w.id;
        s.role = w.role;
        s.state = !p_->sup->is_alive(w.id) ? "dead" : (p_->sup->is_ready(w.id) ? "ready" : "spawned");
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace hcapp
