/* project_session — see project_session.hpp. The per-project bring-up (lifted from main.cpp's body) + the
 * explicit ordered teardown (the blueprint's T1..T12). The two host helpers it alone uses — make_services +
 * seed_memory_from_file — moved here from main's anon namespace. main keeps the bus/broker/supervisor (global)
 * + the UI/headless/screenshot branching; this owns everything rooted under the project subtree. */

#include "project_session.hpp"

#include "hc_conductor.hpp"   /* Conductor — conductor_->stop()/reset() need the full type */
#include "hc_orch_model.hpp"  /* hc::orch::VerifyMode */
#include "hc_orchestrator.hpp"
#include "hc_planner.hpp"     /* hc::planner::decompose — the plan_fn lambda */
#include "cap_authority.hpp"  /* P09: CapabilityAuthority::start / stop */
#include "host_bridge.hpp"    /* UiAdapter / AuthGate / MemoryBroker */
#include "host_conductor.hpp" /* build_conductor, ConductorWiring */
#include "hc_bus.hpp"          /* D4c: delete the conductor's third-party BusClient (complete type at teardown) */
#include "host_storage.hpp"   /* open_project_storage, dir_is_host_private */
#include "skill_store.hpp"    /* W6 P6.2: scan_skills + format_skills_catalog for the per-project skills */

#include "hc_fs.h"   /* seed_memory_from_file reads a JSONL seed file */
#include "hc_json.h" /* and parses it */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h> /* mkdir — the WAL dir */
#include <unordered_set>
#include <vector>

namespace hcapp {

using hc::Orchestrator;
using hc::Supervisor;
namespace orch = hc::orch;

namespace {

/* Seed the memory store from a JSONL file (one {"scope","text","source"} per line), via the broker's seed()
 * API — so test/operator memories are PASSED as data, never hardcoded. Best-effort. (Moved from main.) */
void seed_memory_from_file(hc::host::MemoryBroker *mb, const char *path)
{
    size_t flen = 0;
    char  *data = hc_fs_read_file(path, 1u << 20, &flen);
    if (!data) {
        std::fprintf(stderr, "host: memory-seed file '%s' is unreadable\n", path);
        return;
    }
    int seeded = 0;
    for (char *p = data; *p;) {
        char  *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            if (hc_json *o = hc_json_parse(p, len)) {
                const char *tx = hc_json_get_str(o, "text", "");
                if (tx[0] &&
                    mb->seed(hc_json_get_str(o, "scope", "shared"), tx, hc_json_get_str(o, "source", "operator")) ==
                        0)
                    seeded++;
                hc_json_free(o);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(data);
    std::fprintf(stderr, "host: seeded %d memories from %s\n", seeded, path);
}

/* Build the host's read-side services over the provisioned roots: the bus adapters (token/reasoning stream +
 * the tool-auth gate) and the file-browser jail + the session/artifact stores. The pty is added separately by
 * main (visible-UI only). The known-fleet filters start as the LIVE roster (EMPTY now — de-seeded) and are
 * refreshed by sync_fleet_filters() on every add/remove worker (never a static pool superset). (Moved from main.) */
HostServices make_services(const std::string &sock, const std::string &ws_root, const std::string &sessions_root,
                           const std::string &artifacts_root)
{
    std::unordered_set<std::string> fleet;
    HostServices                    svc;
    svc.adapter = hc::host::UiAdapter::start(sock, fleet);
    svc.gate = hc::host::AuthGate::start(sock, fleet);
    svc.capauth = hc::host::CapabilityAuthority::start(sock, fleet); /* P09: null on no-/dev/urandom -> caps off */
    if (svc.gate) svc.gate->set_cap_authority(svc.capauth); /* P09.3: resolve_scoped mints through the authority */
    svc.ws = ws_root.empty() ? nullptr : hc_sandbox_open(ws_root.c_str(), nullptr);
    svc.store = sessions_root.empty() ? nullptr : hc_store_open(sessions_root.c_str());
    svc.artifacts = artifacts_root.empty() ? nullptr : hc_artifacts_open(artifacts_root.c_str());
    return svc;
}

/* The host-formatted default title for a fresh conversation (the operator renames it later, P3a). The active-id
 * file read/validate/write helpers live in host_storage (the per-project host-private-file owner; unit-tested). */
std::string format_chat_title()
{
    time_t    t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[64];
    if (strftime(buf, sizeof buf, "Chat %Y-%m-%d %H:%M", &tmv) == 0) return std::string("Chat");
    return std::string(buf);
}

} // namespace

/* open() is ~155 LOC — over the function smell-test, but it is an IRREDUCIBLE linear bring-up sequence (each
 * block depends on the prior one: orch <- wal; fleet <- sup/roles; svc <- fleet env; planner/verify/memory/
 * conductor <- svc) and splitting it would only hide the state flow behind artificial helpers. On ANY mid-
 * bring-up failure it `delete s`s the partially-built session — ~ProjectSession() is null-guarded throughout
 * (it frees only the members actually constructed so far), so a new resource added here MUST keep that pattern. */
ProjectSession *ProjectSession::open(const std::string &project_dir, bool ephemeral, hc::Supervisor *sup,
                                     SettingsState *settings, RoleState *role_state, hc_audio_player *player,
                                     hc_sandbox *audio, const std::string &sock, ToolHost *toolhost,
                                     const std::string &tools_root)
{
    if (!sup || !settings || !role_state) return nullptr;
    ProjectSession *s = new ProjectSession();
    s->sup_ = sup;
    s->settings_ = settings;
    s->role_state_ = role_state;
    s->project_dir_ = project_dir;
    s->sock_ = sock; /* D4c: kept so install_conductor can (re)open the conductor's third-party tool client */
    s->roots_ = open_project_storage(project_dir, ephemeral);

    /* P14 durable agendas: journal under a 0700 host-private WAL subdir (ephemeral -> no journaling). Re-validate
     * it host-private (a same-uid peer could pre-create it loose / as a symlink redirecting the journal). */
    std::string wal_dir = ephemeral ? std::string() : s->roots_.wal;
    if (!wal_dir.empty()) {
        bool ok = (mkdir(wal_dir.c_str(), 0700) == 0 || errno == EEXIST);
        if (!ok || !dir_is_host_private(wal_dir)) {
            std::fprintf(stderr, "host: durable agendas disabled (WAL dir not host-private)\n");
            wal_dir.clear();
        }
    }
    s->wal_dir_ = wal_dir;

    s->orch_ = Orchestrator::create(sock, "orchestrator", sup, s->wal_dir_);
    if (!s->orch_) {
        std::fprintf(stderr, "host: orchestrator failed to start\n");
        delete s;
        return nullptr;
    }

    /* The de-seeded fleet (starts EMPTY; the operator/conductor adds workers). Borrows sup (global) + the role
     * table + settings + the role-table mutex (P2.3b: guards the table vs a runtime EditRole during a spawn). */
    s->fleet_ =
        Fleet::create(sup, &role_state->table, &settings->settings, s->roots_.workspaces, s->roots_.sessions,
                      &role_state->mu);
    /* W6 P6.2: per-project Skills. Ensure the skills/ root exists (so it is scannable + the operator/P6.3 can
     * populate it), scan projects/<id>/skills/ for SKILL.md catalogs + build the fenced disclosure block; pass
     * both to the fleet so every worker spawned hereafter gets the catalog in its prompt + a jailed skills dir
     * for load_skill. Empty (no skills authored) => skills stay off. Set BEFORE any spawn. */
    hc_fs_mkdirs(s->roots_.skills.c_str());
    std::string skills_catalog = hcapp::format_skills_catalog(hcapp::scan_skills(s->roots_.skills));
    if (!skills_catalog.empty()) s->fleet_->set_skills(s->roots_.skills, skills_catalog);
    s->fleet_->wait_ready(5000); /* no-op on an empty fleet */
    s->info_ = s->fleet_->env();

    s->svc_ = make_services(sock, s->info_.ws_root, s->info_.sessions_root, s->roots_.artifacts);
    s->svc_.settings = settings;     /* GLOBAL (borrowed) */
    s->svc_.roles = role_state;      /* GLOBAL (borrowed) */
    s->svc_.skills_root = s->roots_.skills; /* W6 P6.3: the Skills panel CRUD + fill_skills scan this dir (jailed) */
    s->svc_.project_dir = ephemeral ? std::string() : project_dir; /* Wave A: per-project persona file lives here */
    s->svc_.shared_workspace = s->info_.shared_workspace;
    s->svc_.player = player;         /* GLOBAL Music Player engine (borrowed; may be null)         */
    s->svc_.audio = audio;           /* GLOBAL audio-library jail (borrowed; may be null)          */
    s->svc_.toolhost = toolhost;     /* D4c: set BEFORE install_conductor below, so the startup conductor's */
    s->svc_.tools_root = tools_root; /* build sees the ToolHost (Tools panel + the conductor opt-in seam)   */
    if (s->svc_.gate && !settings->settings.exec_allow.empty()) {
        /* P06: resolve a requesting agent's PER-ROLE exec allowlist so the gate can intersect it with the global
         * one (subtract-only). agent -> role via the live fleet roster (fleet locks + releases its mu), then
         * role -> exec_allow via the role table (under its own mutex) — two sequential critical sections, the
         * established fleet-then-roles order, no nested hold. Captured pointers (fleet, role table) outlive the
         * gate (the dtor stops the gate before they die — same lifetime the on_change/verifier lambdas rely on). */
        Fleet     *fl = s->fleet_.get();
        RoleState *rs = role_state;
        auto role_exec_fn = [fl, rs](const std::string &agent) -> std::vector<std::string> {
            std::string role;
            for (const auto &pr : fl->pool())
                if (pr.first == agent) {
                    role = pr.second;
                    break;
                }
            if (role.empty()) return {};
            std::lock_guard<std::mutex> lk(rs->mu);
            if (const RoleDef *rd = roletable_find(rs->table, role)) return rd->exec_allow;
            return {};
        };
        s->svc_.gate->enable_exec(settings->settings.exec_allow, s->info_.ws_root, s->info_.shared_workspace,
                                  std::move(role_exec_fn));
    }

    /* W1.3: confirm a file-producing task actually wrote its declared deliverable before it settles Done. */
    if (s->svc_.ws)
        s->orch_->set_deliverable_verifier(
            [ws = s->svc_.ws, shared = s->info_.shared_workspace](const std::string &agent, const std::string &path) {
                return deliverable_present(ws, agent, path, shared);
            });

    /* P05a: the planner as the orchestrator's decomposer (live only). Its model resolves through the role chain.
     * The planner llm is used ONLY by the driver thread; freed in the dtor AFTER the driver is joined. */
    if (s->info_.live) {
        std::string planner_model =
            resolve_role_model(role_state->table, settings->settings, "planner", s->info_.model);
        s->planner_llm_ = open_chat_llm(getenv("HC_BASE_URL"), planner_model.c_str(),
                                        getenv("OPENROUTER_API_KEY"), &s->planner_http_);
        if (s->planner_llm_) {
            hc_llm *pl = s->planner_llm_;
            Fleet  *fl = s->fleet_.get();
            s->plan_fn_ = [pl, fl](const std::string &goal) {
                /* plan against the KNOWN roles, not the (initially empty) live pool — see host_conductor.cpp */
                return hc::planner::decompose(pl, goal, fl->known_roles());
            };
            s->orch_->set_decomposer(s->plan_fn_);
            std::fprintf(stderr, "host: planner ONLINE — a goal-only agenda decomposes via the LLM\n");
            if (const char *rb = getenv("HC_REPLAN")) {
                long b = strtol(rb, nullptr, 10);
                s->replan_budget_ = b <= 0 ? 3 : (b > 100 ? 100 : (int)b);
                std::fprintf(stderr, "host: replan ON — a Failed agenda re-plans up to %d time(s)\n",
                             s->replan_budget_);
            }
        }
    }

    /* P04: opt-in sibling self-check (live + HC_VERIFY). */
    if (s->info_.live && getenv("HC_VERIFY")) {
        s->orch_->set_verify({orch::VerifyMode::Sibling, 1, 1});
        std::fprintf(stderr, "host: verification ON — sibling self-check before a task is accepted\n");
    }

    /* P01 memory: the per-project semantic store + the recall broker (when an embeddings model is configured). */
    const char *embed_model = getenv("HC_EMBED_MODEL");
    const char *or_key = getenv("OPENROUTER_API_KEY");
    /* Bind the store to the embedding model that produced its vectors. A swap to a DIFFERENT model of
     * the same dimension passes every length check and silently returns nonsense from recall, so it is
     * reported loudly here rather than absorbed. */
    /* An id the store cannot record leaves the binding unenforced. That is survivable, but it must not
     * be silent: the whole point of the field is that an undetected model swap is the expensive one. */
    bool embed_model_printable = true; /* reused below: an id we refuse to BIND we also refuse to ECHO */
    if (embed_model && *embed_model) {
        size_t idn = 0;
        bool   ok = true;
        for (; embed_model[idn]; idn++)
            if ((unsigned char)embed_model[idn] < 0x20 || (unsigned char)embed_model[idn] > 0x7e) ok = false;
        embed_model_printable = ok;
        if (!ok || idn >= (size_t)HC_MEM_MODEL_MAX)
            std::fprintf(stderr,
                         "host: *** HC_EMBED_MODEL is not a bindable id (%zu bytes; must be printable "
                         "ASCII under %d). The store cannot record which model produced its vectors, so "
                         "a later model swap will NOT be detected. ***\n",
                         idn, HC_MEM_MODEL_MAX);
    }
    s->svc_.memory = hc_memory_open_model(s->roots_.memory.c_str(), embed_model);
    const bool mem_model_clash = s->svc_.memory && hc_memory_model_mismatch(s->svc_.memory);
    if (mem_model_clash) {
        /* An ADOPTED id was inferred from a store that predates the recording, not witnessed — say so,
         * rather than asserting a provenance this process cannot actually vouch for. */
        const char *had = hc_memory_model(s->svc_.memory);
        const bool  assumed = hc_memory_model_adopted(s->svc_.memory) != 0;
        std::fprintf(stderr,
                     "host: *** MEMORY DISABLED — this store's vectors are %s embedding model '%s', but "
                     "HC_EMBED_MODEL is '%s'. Two models' vectors are not comparable, so recall and "
                     "writes are refused rather than served as nonsense. Set HC_EMBED_MODEL back to "
                     "'%s', or delete %s to start a fresh store on the new model. ***\n",
                     assumed ? "ASSUMED (inferred, not recorded at the time) to come from"
                             : "recorded as coming from",
                     had,
                     /* The store side already refuses to echo a non-printable id, because this line goes
                      * to a terminal and control bytes there can forge log lines or inject ANSI into the
                      * very message the operator has to act on. The CONFIGURED id arrives from the
                      * environment and deserves the same treatment -- an operator env var is not a
                      * trusted string just because it is not the attacker's first choice of vector. */
                     !embed_model          ? "(unset)"
                     : embed_model_printable ? embed_model
                                             : "(unprintable)",
                     had, s->roots_.memory.c_str());
    }
    if (s->svc_.memory && !mem_model_clash && embed_model && *embed_model && or_key && *or_key) {
        const char                     *base = getenv("HC_BASE_URL");
        std::unordered_set<std::string> fleet; /* the LIVE roster (empty now; refreshed on add/remove) */
        s->svc_.mbroker = hc::host::MemoryBroker::start(sock, fleet, s->svc_.memory,
                                                        base && *base ? base : "https://openrouter.ai/api/v1",
                                                        or_key, embed_model);
        if (s->svc_.mbroker) {
            if (const char *seedf = getenv("HC_MEMORY_SEED")) seed_memory_from_file(s->svc_.mbroker, seedf);
            std::fprintf(stderr, "host: memory recall ONLINE (embed model %s)\n", embed_model);
        }
    }

    /* Record WHY memory is in whatever state it is, while the store handle, the configured id and the
     * store path are all still in scope. The stderr lines above are invisible to anyone running the
     * app normally; this is the same information for the Memory panel. */
    {
        hc::ui::MemoryStatus &ms = s->svc_.mem_status;
        ms.config_model = embed_model ? embed_model : "";
        ms.store_path = s->roots_.memory;
        if (s->svc_.memory) {
            ms.store_model = hc_memory_model(s->svc_.memory);
            ms.store_model_assumed = hc_memory_model_adopted(s->svc_.memory) != 0;
        }
        ms.state = mem_model_clash      ? hc::ui::MemoryStatus::State::ModelClash
                   : s->svc_.mbroker    ? hc::ui::MemoryStatus::State::Online
                                        : hc::ui::MemoryStatus::State::NoEmbedModel;
    }

    /* P2.3 security: keep the bus known-fleet filters == the LIVE roster (refreshed by the UI panel AND a
     * conductor tool). svc_ is a stable heap member, so the captured pointer is valid until the dtor clears it. */
    HostServices *svcp = &s->svc_;
    s->fleet_->set_on_change([svcp](const std::vector<std::string> &ids) { sync_fleet_filters(*svcp, ids); });

    /* Conductor P5: the front-door agent. It gets the PROJECT dir so its durable goals land under
     * projects/<id>/conductor_goals (the per-project isolation). Its own chat client (NOT planner_llm). */
    /* P1 conductor conversations: a DEDICATED per-project conversation store (separate from the worker svc_.store),
     * so the conductor's chats get their own browsable picker + auto-resume the last one. Mirrors the
     * conductor_goals host-private gating; ephemeral / non-host-private -> null -> the conductor runs WITHOUT
     * conversation persistence (degrades like the offline worker Session browser). */
    std::string resume_id;
    std::string new_chat_title = format_chat_title();
    if (!ephemeral && !project_dir.empty()) {
        std::string cdir = project_dir + "/conductor_sessions";
        bool        ok = (mkdir(cdir.c_str(), 0700) == 0 || errno == EEXIST);
        if (ok && dir_is_host_private(cdir)) {
            s->conductor_store_ = hc_store_open(cdir.c_str());
            if (s->conductor_store_) resume_id = read_conductor_session(project_dir); /* auto-resume the last chat */
        } else {
            std::fprintf(stderr, "host: conductor conversation persistence disabled (dir not host-private)\n");
        }
    }
    s->svc_.conductor_store = s->conductor_store_; /* P3a/P3b: the picker lists it + inactive rename/delete use it */

    s->install_conductor(resume_id, new_chat_title); /* build+start, publish on svc_, persist the active id */
    if (s->conductor_)
        std::fprintf(stderr, "host: conductor ONLINE\n");
    else if (s->conductor_llm_)
        std::fprintf(stderr, "host: conductor failed to start\n");

    /* P2: the runtime conversation-switch seam — the chat panel's New/Resume commands reach the host-thread switch
     * through this. svc_ is a stable member; the lambda captures the session (cleared when svc_ is reset at teardown,
     * before the session dies). Driven from the dispatch loop => serialized against fill_conductor (no dangling). */
    s->svc_.switch_conductor = [s](const std::string &id) { return s->switch_conductor_conversation(id); };

    return s;
}

ProjectSession::~ProjectSession()
{
    /* The blueprint's T1..T12 — the EXACT order main used. Do NOT reorder (R2: the settle observer holds a raw
     * Conductor*; R3: the conductor session borrows svc_.store; R4: the planner llm is freed after the driver
     * joins, the mbroker stops before the memory it guards closes). Each freed handle is nulled so the implicit
     * member destruction that follows is provably inert. */
    /* T1/T5/T6 (+ the observer-unbind half of the old T3): stop+join the conductor (its tools call the orch/memory/
     * gate facades), UNBIND the settle observer it captured, and free it + its own llm — all via the shared helper
     * the runtime SWITCH reuses. It drains a blocked in-host gate wait first so a conductor frozen in
     * request_and_wait can't hang the join. The conductor is freed BEFORE the orch driver is joined (T3 below), but
     * the unbind is a BARRIER (see teardown_conductor / set_settle_observer), so no in-flight settle fire can
     * dereference the freed conductor — the R2 invariant, now enforced by the orchestrator rather than by ordering. */
    teardown_conductor();
    /* T2: stop refreshing the bus filters — both mutators (UI loop returned; conductor joined) are now gone. */
    if (fleet_) fleet_->set_on_change(nullptr);
    /* T3 (the delete-orch half): JOIN the orchestrator driver. teardown_conductor already barrier-unbound the settle
     * observer, so no settle can fire into the freed conductor between teardown and the join. */
    delete orch_;
    orch_ = nullptr;
    /* T4: free the planner llm now that the only thread that drove it (the orch driver) is joined. */
    if (planner_llm_) close_chat_llm(planner_llm_, planner_http_);
    planner_llm_ = nullptr;
    planner_http_ = nullptr;
    /* T6b: the DEDICATED conductor conversation store — AFTER teardown_conductor() (its hc_session borrowed this). */
    if (conductor_store_) hc_store_close(conductor_store_);
    conductor_store_ = nullptr;
    /* (T7 = hc_secrets_close is GLOBAL — main owns the key store + zeroizes it after `delete session`; see the
     * blueprint. The dtor deliberately skips it, hence T6 -> T8 here.) */
    /* T8: the pty + the workspace jail. */
    if (svc_.pty) hc_pty_close(svc_.pty);
    if (svc_.ws) hc_sandbox_close(svc_.ws);
    /* T9: the stores — AFTER teardown_conductor() (the conductor's hc_session borrowed svc_.store; the R3 invariant). */
    if (svc_.store) hc_store_close(svc_.store);
    if (svc_.artifacts) hc_artifacts_close(svc_.artifacts);
    /* T10: STOP the recall threads BEFORE closing the memory store they guard (T11). */
    if (svc_.mbroker) {
        svc_.mbroker->stop();
        delete svc_.mbroker;
    }
    /* T11. */
    if (svc_.memory) hc_memory_close(svc_.memory);
    /* T12: the bus adapters last. */
    if (svc_.gate) {
        svc_.gate->stop();
        delete svc_.gate;
    }
    if (svc_.capauth) { /* P09: scrubs its signing key on stop() */
        svc_.capauth->stop();
        delete svc_.capauth;
    }
    if (svc_.adapter) {
        svc_.adapter->stop();
        delete svc_.adapter;
    }
    svc_ = HostServices{}; /* every handle freed above -> null them so member destruction is inert */
    /* The fleet LAST: its dtor reaps workers via the BORROWED sup_, which outlives this session (main deletes
     * the session before the supervisor). Nothing above still references the fleet (conductor stopped+freed,
     * on_change cleared). */
    fleet_.reset();
}

/* Stop + free the conductor (+ its own llm), unbinding the orchestrator settle observer that captured it. Leaves
 * conductor_store_ OPEN (the project owns it; only the hc_session inside changes on a switch). Runs on the host
 * thread; idempotent-ish (no-op once conductor_ is null, but still frees a stray conductor_llm_ from a "built the
 * llm but the conductor failed to start" open()). */
void ProjectSession::teardown_conductor()
{
    if (conductor_) {
        if (svc_.gate) svc_.gate->cancel_inhost_waiters(); /* drain a blocked ask_user wait so the join can't hang,
                                                            * AND latch the gate so the dying conductor can't block on
                                                            * a NEW request mid-teardown (install_conductor un-latches
                                                            * it for the rebuilt conductor on a switch). */
        if (orch_) orch_->set_settle_observer(nullptr);    /* unbind — a BARRIER: set_settle_observer waits for any
                                                            * in-flight observer fire, so conductor_.reset() below
                                                            * cannot race a settle callback dereferencing the freed
                                                            * conductor (the CRIT-1 use-after-free fix). */
        /* D4c (security review Low-1): if the conductor is blocked in a third-party tool.invoke recv (past the
         * gate, awaiting the tool's reply), the stop() join below would stall until the tool answers or its
         * timeout (~63s) — a hostile/buggy tool could hold the host thread that long on a switch/shutdown.
         * Shutting the tool client's recv down here makes that recv return at once (-> "the tool did not respond"
         * -> the turn settles -> the thread joins promptly). The client is deleted only after the join. */
        if (cond_tool_bus_) cond_tool_bus_->shutdown();
        conductor_->stop();                                /* join the conductor thread (exits at the turn boundary) */
        svc_.conductor = nullptr;
        conductor_.reset();
    }
    if (conductor_llm_) close_chat_llm(conductor_llm_, cond_http_);
    conductor_llm_ = nullptr;
    cond_http_ = nullptr;
    /* D4c: the conductor's third-party tool client — freed AFTER the conductor thread is joined (conductor_.reset()
     * above), so its invoke seam can no longer touch it. */
    delete cond_tool_bus_;
    cond_tool_bus_ = nullptr;
}

/* Build + start the conductor for `session_id` ("" => fresh), publish it on svc_, and record the active conversation
 * id. `new_title` titles a fresh session ONLY — it is IGNORED on resume (a non-empty session_id). build_conductor
 * RE-BINDS the orchestrator settle observer to the new conductor, so a switch needs no separate re-bind. Re-enables
 * in-host gating (a switch's teardown latched it off). Assumes any prior conductor was torn down first. */
void ProjectSession::install_conductor(const std::string &session_id, const std::string &new_title)
{
    if (svc_.gate) svc_.gate->resume_inhost_gate(); /* un-latch the gate cancel_inhost_waiters() set (no-op on open) */
    std::string model = resolve_role_model(role_state_->table, settings_->settings, "conductor", info_.model);
    ConductorWiring cw =
        build_conductor(info_.live, model.c_str(), fleet_.get(), orch_, svc_.artifacts, svc_.mbroker, svc_.gate,
                        conductor_store_, roots_.ephemeral, project_dir_, svc_.player, svc_.audio, settings_,
                        session_id, new_title, svc_.toolhost, sock_); /* D4c: the conductor's opt-in tool seam */
    conductor_ = std::move(cw.conductor);
    conductor_llm_ = cw.llm;
    cond_http_ = cw.http;
    cond_tool_bus_ = cw.tool_bus; /* D4c: owned here; freed in teardown AFTER the conductor thread is joined */
    if (conductor_) {
        svc_.conductor = conductor_.get();
        if (conductor_store_) write_conductor_session(project_dir_, conductor_->session_id());
    }
}

/* The runtime conversation SWITCH: tear down the current conductor + build a new one bound to `session_id`
 * ("" => a fresh chat). Reuses the tested create() resume path; the orch/fleet/memory/gate facades + the store stay
 * alive (only the conductor + its llm + its hc_session change). Runs on the host/dispatch thread (serialized against
 * fill_conductor). Returns false if conversation persistence is off (no store) or the rebuild failed.
 * KNOWN v1 limitation: teardown_conductor() JOINS the conductor thread, which exits at the next turn boundary — so a
 * switch issued WHILE the conductor is mid-LLM-turn blocks the host thread until that turn finishes (usually a few
 * seconds; up to the LLM HTTP timeout on a stuck provider). Switching when the conductor is idle is instant. An
 * off-thread reaper / cooperative mid-turn cancel is the deferred refinement (see STATE/EXPANSIONS). */
bool ProjectSession::switch_conductor_conversation(const std::string &session_id)
{
    if (!conductor_store_) return false; /* no persistence -> no conversations to switch between */
    teardown_conductor();
    install_conductor(session_id, session_id.empty() ? format_chat_title() : std::string());
    return conductor_ != nullptr;
}

} // namespace hcapp
