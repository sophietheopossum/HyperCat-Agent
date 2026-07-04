/* host_services — see host_services.hpp. The per-frame snapshot/command bridge + the two drive loops,
 * extracted from app/main.cpp. Internal helpers (the command dispatch, the artifact record, the
 * timeline accumulator) are file-static; only build_snapshot / the listings / the loops are exposed.
 * SIZE NOTE: this TU is intentionally large (~1950 LOC) — its functions all operate on the one borrowed
 * HostServices aggregate (snapshot build, the fills incl. fill_audio_status/scan_audio_library, the jailed
 * read_audio_bytes/play_audio_track, dispatch incl. the Audio* transport arms, the loops, the consolidation
 * pass), so they share that context and splitting would thread the struct through a new file for no gain.
 * (run_live_loop [~300 LOC] + dispatch_ui_command [~190 LOC] run long for the same reason: their per-frame/
 * per-command state is loop-local — e.g. the OpenFile/SaveFile/watch view state — and threading it through
 * helpers would add coupling for no isolation gain.) */

#include "host_services.hpp"

#include "consolidation.hpp" /* P6: the compaction + distillation passes */
#include "exe_path.hpp"       /* resolve hc_audio_helper relative to this binary (relocatable bundle) */
#include "hc_agent.h"        /* hc_agent_hosted_backend — the chat backend for consolidation */
#include "hc_conductor.hpp"  /* Conductor P5: ConductorView for fill_conductor + say() routing */
#include "hc_http.h"
#include "hc_llm.h"
#include "hc_orch_model.hpp"
#include "hc_orchestrator.hpp"
#include "hc_policy.hpp"  /* WI-2 E2: hc::egress_allow_edit — the host-authoritative allowlist mutation */
#include "hc_secrets.h"   /* WI-2 E1: the process-local API-key store (the key never reaches a Settings field) */
#include "hc_supervisor.hpp"
#include "hc_fleet.hpp" /* W2 P2.3: the live fleet roster (run_live_loop reads pool() each frame; add/remove) */
#include "prompt_defang.hpp" /* A: defang an attached file's untrusted basename before it enters the conductor turn */
#include "skill_store.hpp" /* W6 P6.3: the Skills panel list + the jailed read/write/delete authoring ops */
#include "hc_toolhost.hpp" /* Wave D/E: third-party tool functions + the live kill-switch + per-tool launch/reap */
#include "hc_fs.h"         /* Wave E: read a tool manifest + the lock when scanning/approving an install */
#include "hc_projects.h" /* W3 P3.2: the project index — fill_projects + the Create/SwitchProject commands */
#include "host_storage.hpp" /* P3b: conductor_session_id_ok; Wave A: read/write_project_persona */
#include "conductor_prompt.hpp" /* Wave A: the persona spine/default/presets surfaced to the persona editor */
#include "cap_authority.hpp" /* P09: CapabilityAuthority::set_known_agents (sync_fleet_filters) */
#include "host_bridge.hpp"
#include "host_time.hpp" /* hcapp::realtime_ms — the project-create timestamp (shared with main) */

#include "hc_sysstat.h" /* WI-3: the per-process CPU/RSS sampler */

#include <ctime>
#include <deque>
#include "text_diff.hpp"
#include "ws_util.hpp" /* the shared bus-id -> workspace-component encoder (single source of truth) */

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <deque>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <dirent.h> /* Wave E: scan the third-party tool install root */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef AUDIO_PROBE_HELPER_PATH
#include "audio_helper_wire.h" /* W-Audio.1: the host<->confined-probe-helper reply contract (POD over a pipe) */
#endif

namespace hcapp {

using hc::Orchestrator;
using hc::Supervisor;
namespace orch = hc::orch;

namespace {

void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

/* Saturating add for the per-frame token re-sum: each per-agent total is already bounded, but summing a
 * fleet of them could still overflow the signed total (UB), so clamp at LONG_MAX. */
long sat_add_tokens(long total, long delta)
{
    if (delta <= 0) return total;
    return (total > LONG_MAX - delta) ? LONG_MAX : total + delta;
}

/* Read the CURRENT bytes of an agent's about-to-be-written file so the diff is computed against the
 * authoritative on-disk version (not a worker-supplied "old"). Rooted at the SAME jail the worker writes to
 * (via ws_subdir): by default the REQUESTING agent's OWN per-agent dir — so "../other-agent/secret" is refused
 * exactly as the worker's write would be, and the diff can never read a sibling's file (security review F1);
 * under W3's shared flag, the one fleet-shared dir (where the worker also wrote — sharing within that jail is
 * the opt-in's point, and "../escape" out of it is still refused). *existed reports whether the file was
 * readable (drives is_new). "" + existed=false on absent/refused/unreadable. Bounded to the fs_write cap. */
std::string read_current_file(hc_sandbox *ws, const std::string &agent, const std::string &path,
                              bool shared, bool *existed)
{
    if (existed) *existed = false;
    if (!ws) return "";
    const char *root = hc_sandbox_root(ws);
    if (!root) return "";
    std::string agent_dir = std::string(root) + "/" + ws_subdir(agent, shared);
    hc_sandbox *jail = hc_sandbox_open(agent_dir.c_str(), nullptr); /* the agent's own jail, like the worker */
    if (!jail) return "";
    std::string   out;
    hc_sandbox_fd fd = HC_SANDBOX_FD_INVALID;
    if (hc_sandbox_open_fd(jail, path.c_str(), O_RDONLY, 0, &fd) == HC_SANDBOX_OK) {
        if (existed) *existed = true;
        char    buf[8192];
        ssize_t r;
        while ((r = read(fd, buf, sizeof buf)) > 0) {
            out.append(buf, (size_t)r);
            if (out.size() > 256u * 1024) break; /* the proposed content is also capped at this */
        }
        close(fd);
    }
    hc_sandbox_close(jail);
    return out;
}

/* Pre-flight an operator-built agenda against the LIVE fleet before submitting it: every task's
 * capability must have a live agent to route to. The orchestrator now fails an unroutable task itself
 * (so a doomed agenda always settles), but catching it HERE gives the operator immediate, specific
 * feedback ("no live 'qa' agent") instead of watching a task fail a moment later — and avoids starting
 * a doomed run at all. Returns "" if every task can route, else the reason for the first that cannot. */
std::string preflight_agenda(const std::vector<hc::ui::TaskSpec> &tasks, Supervisor &sup,
                             const Pool &pool)
{
    for (const auto &t : tasks) {
        bool routable = false;
        for (const auto &w : pool)
            if ((t.capability.empty() || t.capability == w.second) && sup.is_alive(w.first)) {
                routable = true;
                break;
            }
        if (!routable) {
            char               msg[160];
            const std::string &what = t.title.empty() ? t.id : t.title;
            std::snprintf(msg, sizeof msg, "no live '%s' agent — task '%s' can't run",
                          t.capability.c_str(), what.c_str());
            return msg;
        }
    }
    return "";
}

/* Record an approved fs_write/fs_update as a content-addressed artifact with provenance (P02). Best-effort:
 * a null store or a failed put is non-fatal (the worker still writes the file). The producing task is the
 * one currently assigned to the approving agent. fs_update sends the full post-edit content (same wire
 * shape as fs_write), so the approved bytes are the authoritative new file either way. */
void record_write_artifact(hc_artifacts *art, Orchestrator &orch, const hc::host::AuthResolution &r)
{
    if (!art || (r.tool != "fs_write" && r.tool != "fs_update") || r.path.empty()) return;
    char id[HC_ARTIFACT_ID_LEN];
    if (hc_artifacts_put(art, r.content.data(), r.content.size(), id) != 0) return;
    orch::Agenda a = orch.snapshot();
    std::string  task;
    for (const auto &t : a.tasks)
        if (t.assignee == r.agent &&
            (t.state == orch::TaskState::Assigned || t.state == orch::TaskState::Running)) {
            task = t.id;
            break;
        }
    hc_provenance p = {};
    p.agent = r.agent.c_str();
    p.task = task.c_str();
    p.agenda = a.title.c_str();
    p.tool = r.tool.c_str();
    p.label = r.path.c_str();
    p.size = (long)r.content.size();
    hc_artifacts_record(art, id, &p);
}

/* Dispatch a command the UI emitted (the inverse of build_snapshot): CreateAgenda -> the orchestrator;
 * ToolVerdict -> the AuthGate's bus reply (+ record an approved fs_write as an artifact). OpenSession is
 * handled by the caller (run_live_loop), NOT here — it mutates that loop's local viewed transcript state.
 * Returns a short human status for the UI's transient notice line (empty if the command produces none).
 * `c` is non-const so the SetSecret case can zeroize its transient key copy; `settings` is the key/settings
 * store (null if unconfigured). OpenSession/TermInput/ForgetMemory stay in run_live_loop (loop-local state). */
/* defined below in this same (anonymous) namespace; the SaveSettings case uses it */
Settings from_ui_settings(const hc::ui::UiSettings &u);

std::string dispatch_ui_command(hc::ui::UiCommand &c, Orchestrator &orch, Supervisor &sup, Fleet &fleet,
                                hc::host::AuthGate *gate, hc_artifacts *art, hc::host::MemoryBroker *mbroker,
                                const Pool &pool, SettingsState *settings, RoleState *roles)
{
    switch (c.kind) {
    case hc::ui::UiCommand::Kind::CreateAgenda: {
        std::string blocked = preflight_agenda(c.tasks, sup, pool);
        if (!blocked.empty()) return blocked;
        orch::Agenda ag;
        ag.id = "ui-agenda";
        ag.title = c.a.empty() ? "agenda" : c.a;
        ag.goal = c.b;
        for (const auto &ts : c.tasks) {
            orch::Task t;
            t.id = ts.id;
            t.title = ts.title;
            t.description = ts.description;
            t.capability = ts.capability;
            t.deps = ts.deps;
            t.artifact_path = ts.artifact_path; /* W1.3: the host verifies this file exists before Done */
            if (ts.verify) /* P04: a per-task sibling self-check (only when the builder enabled it) */
                t.verify = {orch::VerifyMode::Sibling, ts.verifiers > 0 ? ts.verifiers : 1,
                            ts.quorum > 0 ? ts.quorum : 1};
            ag.tasks.push_back(std::move(t));
        }
        bool started = orch.run_agenda(ag, pool);
        char msg[160];
        if (started)
            std::snprintf(msg, sizeof msg, "agenda '%s' started — %zu task%s", ag.title.c_str(),
                          ag.tasks.size(), ag.tasks.size() == 1 ? "" : "s");
        else
            std::snprintf(msg, sizeof msg, "an agenda is already running — wait for it to finish");
        return msg;
    }
    case hc::ui::UiCommand::Kind::ToolVerdict: {
        if (!gate) return "";
        hc::host::AuthResolution r = gate->resolve(c.a, c.n != 0);
        if (r.approved) {
            if (r.tool == "memory_write" && mbroker)
                /* An approved memory_write means exactly ONE thing — a fleet-shared write — so the host
                 * names the scope itself with a CONSTANT. It must never trust the worker-supplied r.path:
                 * a hostile worker could otherwise craft its own tool.authorize frame with path="agent:B"
                 * and, on one operator click, land a record in another agent's PRIVATE scope (recalled by
                 * B as its own trusted memory). Self writes never reach the gate; only `shared` does. The
                 * reviewed bytes are r.content (bound to the operator's decision in the Approvals panel). */
                mbroker->seed("shared", r.content, r.agent /*broker-stamped requester = provenance*/);
            else
                record_write_artifact(art, orch, r); /* an approved fs_write -> a content-addressed artifact */
        }
        return c.n != 0 ? "tool request allowed" : "tool request denied";
    }
    case hc::ui::UiCommand::Kind::ToolDismiss: {
        if (!gate) return "";
        gate->dismiss(c.a); /* B1: clear the prompt without a verdict — the worker gets "deferred", not "denied" */
        return "tool request deferred";
    }
    case hc::ui::UiCommand::Kind::ToolGrantScoped: {
        /* P09.3: approve THIS fs_write AND mint a scoped capability for `c.n` prompt-free writes under the
         * file's DIRECTORY (the host derives the prefix from the pending path, so the operator never types it;
         * the cap is bound to the broker-stamped requester). Degrades to a plain approve for a top-level file
         * (empty prefix) or when no authority is wired. The approved write is still recorded as an artifact. */
        if (!gate) return "";
        std::string ppath, pcontent, prefix;
        if (gate->peek_content(c.a, ppath, pcontent)) {
            size_t slash = ppath.find_last_of('/');
            if (slash != std::string::npos) prefix = ppath.substr(0, slash + 1); /* keep the trailing '/' */
        }
        /* re-bound the budget at the host (defense in depth): never mint a runaway grant even if a future UI
         * path sends a hostile/typo'd count. The button sends 10. */
        int                      budget = c.n < 1 ? 1 : (c.n > 100 ? 100 : c.n);
        hc::host::AuthResolution r = gate->resolve_scoped(c.a, prefix, (uint32_t)budget);
        if (r.approved) record_write_artifact(art, orch, r);
        return prefix.empty() ? "write allowed"
                              : ("granted " + std::to_string(budget) + " writes under " + prefix);
    }
    case hc::ui::UiCommand::Kind::SetSecret: {
        /* WI-2 E1: the provider API key -> the process-local key store, NEVER disk. c.n carries the
         * operator's per-session "export to worker env" opt-in (SECURITY: re-exposes it to /proc/<pid>/environ
         * for spawns that follow). The transient command copy is scrubbed before return. */
        std::string note;
        if (settings && settings->secrets && !c.a.empty()) {
            hc_secrets_set(settings->secrets, "OPENROUTER_API_KEY", c.a.c_str());
            /* Persist to the OS keychain so a GUI-entered key survives a restart; the note is honest when no
             * keychain is reachable (then it's only this session, exactly as before). */
            const bool persisted = hc_secrets_persist(settings->secrets, "OPENROUTER_API_KEY") == HC_SECRETS_OK;
            if (c.n) setenv("OPENROUTER_API_KEY", c.a.c_str(), 1);
            note = persisted
                       ? (c.n ? "API key saved to the OS keychain + exported to worker env (this session)"
                              : "API key saved to the OS keychain (persists across restarts)")
                       : (c.n ? "API key stored for this session + exported to worker env (no keychain)"
                              : "API key stored for this session (no keychain reachable — not persisted)");
        }
        if (!c.a.empty()) hc_secrets_zero(&c.a[0], c.a.size());
        c.a.clear();
        return note;
    }
    case hc::ui::UiCommand::Kind::ForgetSecret: {
        /* Drop the persisted provider key from the OS keychain AND the in-memory store. Does NOT unsetenv — an
         * operator's own env export is theirs to manage (consistent with "env wins"). */
        if (!settings || !settings->secrets) return "";
        hc_secrets_forget_keychain(settings->secrets, "OPENROUTER_API_KEY");
        hc_secrets_delete(settings->secrets, "OPENROUTER_API_KEY");
        return "stored provider key forgotten (keychain + memory)";
    }
    case hc::ui::UiCommand::Kind::SaveSettings: {
        if (!settings) return "";
        /* The egress allowlist is owned LIVE by the E2 editor (EditAllowlist persists it immediately), so a
         * stale draft from [Apply] must NOT clobber it — preserve the authoritative list across the map. */
        std::vector<std::string> preserved_egress = settings->settings.egress_allow;
        std::vector<std::string> preserved_exec = settings->settings.exec_allow; /* W4: live-owned by EditExecAllowlist */
        auto                     preserved_roles = settings->settings.role_models; /* W2: live-owned by AssignRoleModel */
        /* the audio settings are LIVE-owned by the Music Player panel (its volume slider + the mood/spectrum
         * toggles persist them), NOT the Settings panel — preserve them so an [Apply] here can't reset them. */
        bool preserved_mood = settings->settings.conductor_mood_enabled;
        int  preserved_vol = settings->settings.audio_volume;
        bool preserved_spec = settings->settings.audio_spectrum;
        /* the conductor persona is LIVE-OWNED by the persona editor (SetConductorPersona persists it), NOT the
         * Settings draft — preserve it so an [Apply] here can't wipe the operator's chosen voice. */
        std::string preserved_persona = settings->settings.conductor_persona;
        /* the Custom Tooling toggles are LIVE-OWNED by the Tools panel (ToggleTool / DisableAllThirdParty persist
         * them), NOT the Settings draft — preserve them so an [Apply] here can't reset the System/third-party
         * toggles or the kill-switch / conductor opt-in. */
        auto preserved_system_tools = settings->settings.system_tools;
        auto preserved_thirdparty_tools = settings->settings.thirdparty_tools;
        bool preserved_tp_enabled = settings->settings.thirdparty_tools_enabled;
        bool preserved_tp_conductor = settings->settings.thirdparty_tools_conductor;
        settings->settings = from_ui_settings(c.settings);
        settings->settings.egress_allow = std::move(preserved_egress);
        settings->settings.exec_allow = std::move(preserved_exec);
        settings->settings.role_models = std::move(preserved_roles);
        settings->settings.conductor_mood_enabled = preserved_mood;
        settings->settings.audio_volume = preserved_vol;
        settings->settings.audio_spectrum = preserved_spec;
        settings->settings.conductor_persona = std::move(preserved_persona);
        settings->settings.system_tools = std::move(preserved_system_tools);
        settings->settings.thirdparty_tools = std::move(preserved_thirdparty_tools);
        settings->settings.thirdparty_tools_enabled = preserved_tp_enabled;
        settings->settings.thirdparty_tools_conductor = preserved_tp_conductor;
        settings_validate(settings->settings);
        bool ok = settings_save(settings->settings, settings->path.c_str());
        inject_settings_env(settings->settings); /* limits re-injected for the next spawn */
        return ok ? "settings saved (provider/limits apply on restart)" : "settings save FAILED";
    }
    case hc::ui::UiCommand::Kind::EditAllowlist: {
        /* WI-2 E2 (security-WEAKENING, confirm-gated in the UI): add/remove ONE egress re-permit, persisted
         * immediately. The host RE-VALIDATES the IP (the UI is untrusted): a numeric IPv4/IPv6 only — never a
         * hostname/CIDR. There is no "disable" path: an empty list is just default-deny; the worker always
         * installs the guard. The change reaches workers started AFTER it (via the injected HC_EGRESS_ALLOW). */
        if (!settings) return "";
        const bool add = (c.b == "add");
        /* the host-authoritative validated mutation (numeric-IP-only, dedup, capped) — shared with
         * test_hc_policy; the UI's inet_pton + confirm modal were advisory. */
        if (!hc::egress_allow_edit(settings->settings.egress_allow, c.a.c_str(), add))
            return add ? "egress entry rejected (not a numeric IP, or already present / at the cap)"
                       : "egress entry not found";
        settings_validate(settings->settings);
        bool ok = settings_save(settings->settings, settings->path.c_str());
        inject_settings_env(settings->settings); /* reaches workers started after this change */
        return ok ? (add ? "egress entry added (restart workers to apply)" : "egress entry removed")
                  : "egress edit save FAILED";
    }
    case hc::ui::UiCommand::Kind::EditExecAllowlist: {
        /* W4 (security-WEAKENING, confirm-gated in the UI): add/remove ONE allowlisted binary for the brokered
         * `run` tool, persisted immediately. The host RE-VALIDATES (the UI is untrusted): an ABSOLUTE path to an
         * existing regular file only. An empty list is default-deny (exec disabled). The ExecGate (W4.3)
         * re-validates again at exec time (realpath, reject setuid). */
        if (!settings) return "";
        const bool add = (c.b == "add");
        if (!hc::exec_allow_edit(settings->settings.exec_allow, c.a.c_str(), add))
            return add ? "exec entry rejected (not an absolute path to an existing file, or already present / at the cap)"
                       : "exec entry not found";
        settings_validate(settings->settings);
        bool ok = settings_save(settings->settings, settings->path.c_str());
        return ok ? (add ? "exec allowlist entry added" : "exec allowlist entry removed")
                  : "exec edit save FAILED";
    }
    case hc::ui::UiCommand::Kind::AssignRoleModel: {
        /* W2: assign (or clear, when b is empty) which model a role runs, persisted IMMEDIATELY — the
         * assignment is live-owned like the egress list (SaveSettings preserves it). settings_validate keeps
         * it referentially consistent (an assignment to a model not in the catalog is dropped). The change
         * reaches workers (re)spawned after it. */
        if (!settings || c.a.empty()) return "";
        if (c.b.empty()) settings->settings.role_models.erase(c.a);
        else settings->settings.role_models[c.a] = c.b;
        settings_validate(settings->settings);
        bool ok = settings_save(settings->settings, settings->path.c_str());
        if (!ok) return "model assignment save FAILED";
        return c.b.empty() ? ("role '" + c.a + "' uses the global model")
                           : ("role '" + c.a + "' -> " + c.b + " (restart workers to apply)");
    }
    case hc::ui::UiCommand::Kind::AddWorker: {
        /* W2 P2.3: spawn a worker of role c.a at the next free pool id (auto-assigned). The fleet resolves the
         * role's model/prompt/tools; run_live_loop then refreshes the bus known-fleet filters to include it. */
        if (c.a.empty()) return "";
        std::string id = fleet.add_worker(c.a);
        if (id.empty()) return "could not add a worker (the id pool is full, or the spawn failed)";
        return "added worker " + id + " [" + c.a + "]";
    }
    case hc::ui::UiCommand::Kind::RemoveWorker: {
        /* W2 P2.3: reap the worker + REVOKE its bus routing (Supervisor::reap), then drop it from the fleet. */
        if (c.a.empty()) return "";
        return fleet.remove_worker(c.a) ? ("removed worker " + c.a) : ("unknown worker " + c.a);
    }
    case hc::ui::UiCommand::Kind::EditRole: {
        /* W2 P2.3b: UPSERT an operator-edited role TEMPLATE, persisted immediately to roles.json. Unknown tool
         * names are dropped HERE (never granted); roletable_validate bounds the overlay + role name + the role
         * count on save. The edit resolves at the role's NEXT spawn — existing workers keep their spawn-time
         * identity (the host owns the table; Fleet borrows it and reads it fresh in role_spawn_args). */
        if (!roles || c.role_edit.role.empty()) return "";
        RoleDef d;
        d.role = c.role_edit.role;
        d.prompt_overlay = c.role_edit.prompt_overlay;
        d.model = c.role_edit.model;
        for (const auto &nm : c.role_edit.tools) {
            RoleTool rt;
            if (role_tool_from_name(nm, rt)) d.tools.push_back(rt); /* unknown -> skip (never granted) */
        }
        /* lock the table: a concurrent conductor-thread spawn (Fleet::add_worker -> role_spawn_args) reads it */
        std::lock_guard<std::mutex> lk(roles->mu);
        RoleDef                    *existing = nullptr;
        for (auto &r : roles->table.roles)
            if (r.role == d.role) { existing = &r; break; }
        if (existing) *existing = std::move(d);
        else roles->table.roles.push_back(std::move(d));
        roletable_validate(roles->table);
        if (!roletable_find(roles->table, c.role_edit.role)) /* a new role beyond the cap got clamped away */
            return "role table is full (edit dropped)";
        bool ok = roletable_save(roles->table, roles->path.c_str());
        return ok ? ("role '" + c.role_edit.role + "' saved (applies to new workers of that role)")
                  : "role save FAILED";
    }
    case hc::ui::UiCommand::Kind::RemoveRole: {
        /* W2 P2.3b: drop a role template + persist. A later add_worker(role) for a removed role falls back to
         * the base prompt + all tools (roletable_find returns null) — the safe default, never a privilege gain. */
        if (!roles || c.a.empty()) return "";
        std::lock_guard<std::mutex> lk(roles->mu); /* guards the table vs a conductor-thread spawn read */
        auto                       &v = roles->table.roles;
        bool                        found = false;
        for (size_t i = 0; i < v.size(); i++)
            if (v[i].role == c.a) { v.erase(v.begin() + i); found = true; break; }
        if (!found) return "unknown role " + c.a;
        roletable_validate(roles->table);
        bool ok = roletable_save(roles->table, roles->path.c_str());
        return ok ? ("role '" + c.a + "' removed") : "role remove FAILED";
    }
    default:
        break;
    }
    return "";
}

/* Load one session's transcript ("role: content" per message) for the transcript panel. */
void load_transcript(hc_store *store, const std::string &id, std::vector<std::string> &out)
{
    out.clear();
    if (!store) return;
    hc_session *sess = hc_session_load(store, id.c_str());
    if (!sess) return;
    size_t n = hc_session_count(sess);
    for (size_t i = 0; i < n; i++) {
        const char *role = nullptr, *content = nullptr;
        if (hc_session_message(sess, i, &role, &content))
            out.push_back(std::string(role ? role : "?") + ": " + (content ? content : ""));
    }
    hc_session_free(sess);
}

/* A monotonic clock in milliseconds for the activity timeline (CLOCK_MONOTONIC — same source the
 * orchestrator stamps Task::assigned_at_ms with, so the timeline and the model agree on "now"). */
uint64_t mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The activity-timeline collector (P10). observe() each frame opens a span when a task is Running/
 * Assigned (on its assignee's lane), closes it when the task stops (Crash on a failure/reassignment),
 * and closes spans for tasks no longer present. Open spans live in a map (drawn to "now"); closed spans
 * in a bounded deque (drop-oldest). Single-threaded (host/UI thread) — no bus, no lock. */
struct TimelineLog {
    struct Open {
        std::string lane, label;
        uint64_t    start_ms;
    };
    std::unordered_map<std::string, Open> open;
    std::deque<hc::ui::TimelineSpan>      closed;
    static constexpr size_t               kMax = 2000;

    void observe(const std::vector<hc::ui::TaskRow> &tasks, uint64_t now)
    {
        using K = hc::ui::TimelineSpan::Kind;
        for (const auto &t : tasks) {
            bool running = (t.state == "running" || t.state == "assigned");
            auto it = open.find(t.id);
            if (running) {
                if (it == open.end())
                    open[t.id] = {t.assignee, t.title.empty() ? t.id : t.title, now};
                else if (it->second.lane != t.assignee && !t.assignee.empty()) {
                    closed.push_back({it->second.lane, it->second.label, t.id, it->second.start_ms, now,
                                      K::Crash}); /* reassigned mid-flight */
                    it->second = {t.assignee, t.title.empty() ? t.id : t.title, now};
                }
            } else if (it != open.end()) {
                closed.push_back({it->second.lane, it->second.label, t.id, it->second.start_ms, now,
                                  t.state == "failed" ? K::Crash : K::Task});
                open.erase(it);
            }
        }
        /* close any span whose task is no longer present (a new agenda replaced the set) so `open` never
         * retains stale entries across runs — the map stays bounded even if re-runs are ever allowed. */
        for (auto it = open.begin(); it != open.end();) {
            bool present = false;
            for (const auto &t : tasks)
                if (t.id == it->first) {
                    present = true;
                    break;
                }
            if (present) {
                ++it;
            } else {
                closed.push_back({it->second.lane, it->second.label, it->first, it->second.start_ms,
                                  now, K::Task});
                it = open.erase(it);
            }
        }
        while (closed.size() > kMax) closed.pop_front();
    }

    void copy_into(std::vector<hc::ui::TimelineSpan> &out) const
    {
        out.assign(closed.begin(), closed.end());
        for (const auto &kv : open) /* open spans render to now_ms */
            out.push_back({kv.second.lane, kv.second.label, kv.first, kv.second.start_ms, 0,
                           hc::ui::TimelineSpan::Kind::Task});
    }
};

} // namespace

/* ---- exposed API (see host_services.hpp) ------------------------------------------------------- */

/* WI-3: a process-lifetime, host-thread-only sampler of THIS host's CPU%/RSS/uptime/clock, throttled to
 * ~2 Hz with short rolling rings for the Dashboard sparklines. Lazily opened; the handle leaks at process
 * exit (intentional — one process-lifetime resource). Self-contained file-statics so the snapshot
 * enrichment stays in build_snapshot (one place, host thread only). */
namespace {
hc_sysstat              *g_sysstat = nullptr;
bool                     g_sysstat_failed = false, g_sysstat_have = false;
std::deque<float>        g_cpu_ring, g_rss_ring;
uint64_t                 g_sysstat_last_ms = 0;
struct hc_sysstat_sample g_sysstat_last = {};
constexpr std::size_t    kSysRing = 120; /* ~1 min of history at 2 Hz */

void fill_sysstat(hc::ui::UiSnapshot &s)
{
    if (g_sysstat_failed) return;
    if (!g_sysstat && !(g_sysstat = hc_sysstat_open(0))) { /* 0 => self */
        g_sysstat_failed = true;
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t mono = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    if (g_sysstat_last_ms == 0 || mono - g_sysstat_last_ms >= 500) { /* sample at ~2 Hz, not per-frame */
        struct hc_sysstat_sample cur = {};
        if (hc_sysstat_sample(g_sysstat, &cur) == 0) {
            g_sysstat_last = cur;
            g_sysstat_have = true;
            g_cpu_ring.push_back((float)cur.cpu_pct);
            g_rss_ring.push_back((float)((double)cur.rss_bytes / (1024.0 * 1024.0)));
            if (g_cpu_ring.size() > kSysRing) g_cpu_ring.pop_front();
            if (g_rss_ring.size() > kSysRing) g_rss_ring.pop_front();
        }
        g_sysstat_last_ms = mono;
    }
    if (!g_sysstat_have) return;
    s.sysstat.present = true;
    s.sysstat.cpu_pct = g_sysstat_last.cpu_pct;
    s.sysstat.rss_bytes = g_sysstat_last.rss_bytes;
    s.sysstat.uptime_ms = g_sysstat_last.uptime_ms;
    s.sysstat.wall_ms = g_sysstat_last.wall_ms;
    s.sysstat.cpu_history.assign(g_cpu_ring.begin(), g_cpu_ring.end());
    s.sysstat.rss_mb_history.assign(g_rss_ring.begin(), g_rss_ring.end());
}
} // namespace

hc::ui::UiSnapshot build_snapshot(Orchestrator &orch_, Supervisor &sup, const Pool &pool, const RoleTable *roles)
{
    hc::ui::UiSnapshot s;
    orch::Agenda      a = orch_.snapshot();
    s.agenda_title = a.title.empty() ? "agenda" : a.title;
    /* progress comes from the agenda we just copied under one lock — calling orch_.progress() would
     * re-lock the orchestrator and re-scan the same tasks for a value already in hand. */
    s.agenda_progress = orch::agenda_progress(a);

    for (const auto &w : pool) {
        hc::ui::AgentRow r;
        r.id = w.first;
        r.role = w.second;
        r.state = !sup.is_alive(w.first) ? "dead" : (sup.is_ready(w.first) ? "ready" : "spawned");
        s.agents.push_back(r);
    }
    s.roles = distinct_capabilities(pool); /* the agenda-builder capability combo reflects the LIVE fleet's roles */
    /* P2.3b: the editable role TEMPLATES for the Worker Builder (the editor seeds its draft from these). No
     * RoleState lock here: build_snapshot + EditRole both run on the HOST thread (sequential), and the only
     * cross-thread reader (a conductor-thread Fleet spawn) takes the table under roles_mu separately — so this
     * host-thread read never races a write. */
    if (roles) {
        for (const auto &d : roles->roles) {
            hc::ui::UiRoleRow r;
            r.role = d.role;
            r.prompt_overlay = d.prompt_overlay;
            r.model = d.model;
            for (RoleTool t : d.tools) r.tools.push_back(role_tool_name(t)); /* enabled tool-ids; empty = all */
            s.role_defs.push_back(std::move(r));
        }
        /* the full tool-id list the editor's checkboxes enumerate — sourced HERE (the host owns the role types)
         * so the UI stays free of hcapp::RoleTool (it reads only these strings). */
        for (size_t i = 0; i < role_tool_count(); i++) s.tool_catalog.push_back(role_tool_name((RoleTool)i));
    }
    int done = 0, failed = 0;
    for (const auto &t : a.tasks) {
        hc::ui::TaskRow r;
        r.id = t.id;
        r.title = t.title;
        r.state = orch::task_state_str(t.state);
        r.assignee = t.assignee;
        r.description = t.description;
        r.result = t.result;
        r.deps = t.deps; /* the DAG edges (P10) */
        s.tasks.push_back(r);
        if (t.state == orch::TaskState::Done) done++;
        if (t.state == orch::TaskState::Failed) failed++;
    }
    char line[160];
    std::snprintf(line, sizeof line, "agenda '%s' — %d/%zu done%s", s.agenda_title.c_str(), done,
                  a.tasks.size(), failed ? " (with a reassigned crash)" : "");
    s.logs.push_back(line);
    for (const auto &t : a.tasks)
        if (t.state == orch::TaskState::Done) {
            std::snprintf(line, sizeof line, "  %s done -> %s", t.id.c_str(), t.result.c_str());
            s.logs.push_back(line);
        }
    fill_sysstat(s); /* WI-3: host CPU/RSS/uptime + sparkline history (samples at ~2 Hz internally) */
    return s;
}

namespace {
constexpr size_t kMaxFileEntries = 4096; /* bound the recursive listing vs a huge/hostile workspace      */
constexpr int    kMaxFileDepth = 16;     /* and bound recursion depth (dirs can't cycle — defense in depth) */

/* List the in-jail directory `rel` ("" = the root) and recurse into its real subdirectories, appending full
 * ws-relative paths to `out`. Bounded by kMaxFileEntries (total) + kMaxFileDepth; `truncated` is raised if a
 * cap trips. hc_sandbox_list does the O_NOFOLLOW walk + reports-but-never-follows symlinks (is_dir==0), so the
 * recursion only ever descends real directories — no symlink cycle is reachable. */
void list_workspace_rec(hc_sandbox *ws, const std::string &rel, int depth,
                        std::vector<hc::ui::FileEntry> &out, bool &truncated)
{
    if (out.size() >= kMaxFileEntries) {
        truncated = true;
        return;
    }
    hc_sandbox_dirent *ents = nullptr;
    size_t             n = 0;
    if (hc_sandbox_list(ws, rel.empty() ? "." : rel.c_str(), &ents, &n) != HC_SANDBOX_OK) return;
    for (size_t i = 0; i < n; i++) {
        if (out.size() >= kMaxFileEntries) {
            truncated = true;
            break;
        }
        std::string path = rel.empty() ? std::string(ents[i].name) : rel + "/" + ents[i].name;
        bool        is_dir = ents[i].is_dir != 0;
        out.push_back({path, ents[i].size, is_dir});
        if (is_dir) {
            if (depth + 1 > kMaxFileDepth) truncated = true; /* keep the dir row, just don't descend past the cap */
            else list_workspace_rec(ws, path, depth + 1, out, truncated);
        }
    }
    hc_sandbox_list_free(ents);
}
} // namespace

void list_workspace(hc_sandbox *ws, std::vector<hc::ui::FileEntry> &out, bool *truncated)
{
    out.clear();
    if (truncated) *truncated = false;
    if (!ws) return;
    bool t = false;
    list_workspace_rec(ws, "", 0, out, t);
    if (truncated) *truncated = t;
}

void list_sessions(hc_store *store, std::vector<hc::ui::SessionRow> &out)
{
    out.clear();
    if (!store) return;
    hc_session_info *infos = nullptr;
    size_t           n = 0;
    if (hc_store_list(store, &infos, &n) != 0) return;
    for (size_t i = 0; i < n; i++)
        out.push_back({infos[i].id, infos[i].title, infos[i].model, infos[i].updated, infos[i].turns});
    hc_store_list_free(infos, n);
}

/* P3b: the conductor conversations picker list — mirror list_sessions over the DEDICATED conductor store, sorted
 * newest-first by the updated timestamp (ISO-8601 sorts lexically). Throttled like the other listings. */
void list_conductor_conversations(hc_store *store, std::vector<hc::ui::ConductorConversationRow> &out)
{
    out.clear();
    if (!store) return;
    hc_session_info *infos = nullptr;
    size_t           n = 0;
    if (hc_store_list(store, &infos, &n) != 0) return;
    for (size_t i = 0; i < n; i++) out.push_back({infos[i].id, infos[i].title, infos[i].updated, infos[i].turns});
    hc_store_list_free(infos, n);
    std::sort(out.begin(), out.end(),
              [](const hc::ui::ConductorConversationRow &a, const hc::ui::ConductorConversationRow &b) {
                  return a.updated > b.updated; /* newest first */
              });
}

/* P3b: delete a conversation's on-disk session dir from the conductor store — a JAILED two-step (list the flat
 * session dir, unlink each file, then remove the now-empty dir), reusing the audited no-follow hc_sandbox unlink
 * (NO recursive remove). The id is validated as a single path component first (conductor_session_id_ok: rejects
 * '/'/'..'/empty/oversized); the ACTIVE conversation is refused by the caller. Returns true iff the dir was removed. */
static bool delete_conductor_session(hc_store *store, const std::string &id)
{
    if (!store || !conductor_session_id_ok(id)) return false;
    const char *root = hc_store_root(store);
    if (!root) return false;
    hc_sandbox *sb = hc_sandbox_open(root, nullptr);
    if (!sb) return false;
    hc_sandbox_dirent *ents = nullptr;
    size_t             n = 0;
    if (hc_sandbox_list(sb, id.c_str(), &ents, &n) == HC_SANDBOX_OK) {
        for (size_t i = 0; i < n; i++) {
            if (ents[i].is_dir) continue; /* a session dir is flat — never descend (defensive) */
            std::string p = id + "/" + ents[i].name;
            hc_sandbox_unlink(sb, p.c_str(), 0); /* a file (no-follow, jailed) */
        }
        hc_sandbox_list_free(ents);
    }
    bool ok = hc_sandbox_unlink(sb, id.c_str(), 1) == HC_SANDBOX_OK; /* the now-empty dir (AT_REMOVEDIR) */
    hc_sandbox_close(sb);
    return ok;
}

void list_artifacts(hc_artifacts *art, std::vector<hc::ui::ArtifactRow> &out)
{
    out.clear();
    if (!art) return;
    hc_artifact_rec *recs = nullptr;
    size_t           n = 0;
    if (hc_artifacts_recent(art, 256, &recs, &n) != 0) return;
    for (size_t i = 0; i < n; i++)
        out.push_back(
            {recs[i].id, recs[i].task, recs[i].agent, recs[i].label, recs[i].created, recs[i].size});
    hc_artifacts_recs_free(recs, n);
}

/* P12: fill the snapshot's per-agent token usage + saturating totals from the adapter's accumulated
 * "turn.usage" pubs. Shared by the live loop and the headless capture so the two never drift (the values
 * are already bounded per-agent; the re-sum saturates). No-op when there is no adapter. */
void fill_usage(hc::ui::UiSnapshot &s, hc::host::UiAdapter *adapter)
{
    if (!adapter) return;
    std::vector<hc::host::UiAdapter::UsageView> uv;
    adapter->copy_usage(uv);
    for (const auto &u : uv) {
        s.usage_by_agent.push_back({u.agent, u.input_tokens, u.output_tokens, u.calls});
        s.tokens_in = sat_add_tokens(s.tokens_in, u.input_tokens);
        s.tokens_out = sat_add_tokens(s.tokens_out, u.output_tokens);
    }
}

void fill_egress(hc::ui::UiSnapshot &s, hc::host::UiAdapter *adapter)
{
    if (!adapter) return;
    std::vector<hc::host::UiAdapter::EgressEvent> ev;
    adapter->copy_egress(ev);
    for (const auto &e : ev) s.egress.push_back({e.agent, e.host, e.ip, e.verdict, e.port, e.at_ms});
}

void fill_memory(hc::ui::UiSnapshot &s, hc::host::MemoryBroker *mb)
{
    if (!mb) return;
    std::vector<hc::host::MemRow> rows;
    mb->list(rows);
    for (const auto &r : rows) s.memory.push_back({r.id, r.scope, r.text, r.source});
}

void fill_projects(hc::ui::UiSnapshot &s, hc_projects *projects)
{
    if (!projects) return;
    char active[HC_PROJECT_ID_CAP];
    bool have_active = (hc_projects_get_active(projects, active, sizeof active) == 0);
    if (have_active) s.active_project = active; /* overwritten with the display name below if the row is found */
    hc_project *list = nullptr;
    int         n = hc_projects_list(projects, &list);
    for (int i = 0; i < n; i++) {
        hc::ui::ProjectRow r;
        r.id = list[i].id;
        r.display = list[i].display;
        r.active = have_active && std::strcmp(list[i].id, active) == 0;
        if (r.active) s.active_project = r.display[0] ? r.display : r.id;
        s.projects.push_back(std::move(r));
    }
    if (n > 0) hc_projects_free_list(list);
}

/* W6 P6.3: the per-project Skills list (name + description), scanned JAILED from the skills dir into `out`
 * (cleared first). Host-side throttled (~2 Hz) like the file/session listings — NOT per frame (each call opens
 * a sandbox + reads each SKILL.md). The screenshot capture path calls it once. */
void fill_skills(std::vector<hc::ui::SkillRow> &out, const std::string &skills_root)
{
    out.clear();
    if (skills_root.empty()) return;
    for (const auto &m : hcapp::scan_skills(skills_root)) out.push_back({m.name, m.description});
}

#ifdef AUDIO_PROBE_HELPER_PATH
/* W-Audio: the untrusted audio decoders run in a CONFINED CHILD, never in the host. Forward-declared so the public
 * play_audio_track (just below) can route to the decode path defined further down in the confined-helper block. */
static bool probe_audio_file_confined(hc_sandbox *audio, const std::string &name, hc_audio_meta *out);
static bool decode_and_load_confined(hc_audio_player *player, hc_sandbox *audio, const std::string &name);
#else
/* No confined helper compiled in (a degenerate build only): the legacy in-host read for play_audio_track's
 * fallback. Music Player: read a jailed audio file into `out`; files past the cap are name-only (return false). */
static bool read_audio_bytes(hc_sandbox *audio, const std::string &name, std::string &out)
{
    out.clear();
    constexpr size_t kCap = 32u * 1024u * 1024u; /* covers any normal song incl. lossless; bigger -> name-only */
    hc_sandbox_fd    fd = HC_SANDBOX_FD_INVALID;
    if (hc_sandbox_open_fd(audio, name.c_str(), O_RDONLY | O_NONBLOCK, 0, &fd) != HC_SANDBOX_OK || fd < 0)
        return false;
    struct stat fst;
    if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_size <= 0 || (size_t)fst.st_size > kCap) {
        close(fd);
        return false;
    }
    std::string buf((size_t)fst.st_size, '\0');
    size_t      total = 0;
    while (total < buf.size()) {
        ssize_t r = read(fd, &buf[total], buf.size() - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    close(fd);
    buf.resize(total);
    out = std::move(buf);
    return total > 0;
}
#endif /* AUDIO_PROBE_HELPER_PATH */

/* Read `name` from `jail` (jailed) + load it into the player + start playing. Returns false on a missing/
 * unreadable/undecodable track (the player is left as it was). Used by AudioPlay + AudioNext/Prev + the
 * conductor's set_mood tool (declared in host_services.hpp for that cross-TU reuse). The untrusted bytes are
 * decoded to PCM in a CONFINED CHILD (W-Audio.2) — the host plays only raw samples it has clamped, no decoder. */
bool play_audio_track(hc_audio_player *player, hc_sandbox *jail, const std::string &name)
{
    if (!player || !jail || name.empty()) return false;
#ifdef AUDIO_PROBE_HELPER_PATH
    return decode_and_load_confined(player, jail, name);
#else
    std::string bytes;
    if (!read_audio_bytes(jail, name, bytes)) return false;
    if (hc_audio_player_load(player, (const unsigned char *)bytes.data(), bytes.size(), name.c_str(), nullptr) != 0)
        return false;
    hc_audio_player_play(player);
    return true;
#endif
}

/* The now-playing engine status — cheap, called EVERY frame so the spectrum + position stay smooth. */
void fill_audio_status(hc::ui::UiSnapshot &s, hc_audio_player *player, bool enabled, bool spectrum_on)
{
    s.audio_present = (player != nullptr);
    s.audio_enabled = enabled;
    s.audio_spectrum_on = spectrum_on;
    if (!player) return;
    hc_audio_status st;
    hc_audio_player_get_status(player, &st);
    s.audio_title = st.title;
    s.audio_artist = st.artist;
    s.audio_state = (int)st.state;
    s.audio_pos_ms = (long)st.pos_ms;
    s.audio_dur_ms = (long)st.dur_ms;
    s.audio_volume = (int)(st.volume * 100.0f + 0.5f);
    if (spectrum_on && st.n_spectrum > 0) s.audio_spectrum.assign(st.spectrum, st.spectrum + st.n_spectrum);
}

#ifdef AUDIO_PROBE_HELPER_PATH
/* monotonic milliseconds — for the helper-read total deadlines (immune to wall-clock changes). */
static long audio_mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Read exactly `n` bytes from `fd` under a TOTAL absolute `deadline_ms` (NOT per-poll: a compromised helper that
 * drips one byte just under each interval could otherwise reset a per-call timeout forever and stall this
 * single-threaded drive loop). Returns the bytes actually read (== n on success). */
static size_t audio_read_exact(int fd, unsigned char *dst, size_t n, long deadline_ms)
{
    size_t got = 0;
    while (got < n) {
        long remaining = deadline_ms - audio_mono_ms();
        if (remaining <= 0) break; /* total budget spent */
        struct pollfd pfd = {fd, POLLIN, 0};
        int           pr = poll(&pfd, 1, (int)remaining);
        if (pr <= 0) break; /* timeout (0) or error (<0) */
        ssize_t r = read(fd, dst + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break; /* EOF — the child exited early */
        got += (size_t)r;
    }
    return got;
}

/* Open `name` jailed + spawn the confined audio helper for `job`, with the file on its stdin and a reply pipe on
 * its stdout. Returns true + sets *out_read_fd (the host's read end) + *out_pid on success. KEY properties (each
 * verified in the W-Audio.1 security review): the jailed open uses O_NONBLOCK + S_ISREG to defeat a same-uid
 * FIFO/device wedge; the dup2 TARGETS (stdin=0, stdout=1) sit below the SOURCE fds (>=3), so no dup2-clobber, and
 * the helper close_range(3,~)'s every other inherited fd; the env is EMPTY so OPENROUTER_API_KEY (and every host
 * secret) never enters the untrusted decoder's address space (a decoder RCE reads its own envp directly, which
 * neither Landlock nor the fd-drop would contain). The child does the file read + decode, never the host. */
static bool spawn_confined_helper(hc_sandbox *audio, const std::string &name, const char *job, int *out_read_fd,
                                  pid_t *out_pid)
{
    hc_sandbox_fd fd = HC_SANDBOX_FD_INVALID;
    if (hc_sandbox_open_fd(audio, name.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0, &fd) != HC_SANDBOX_OK ||
        fd < 0)
        return false;
    struct stat fst;
    /* the host's practical audio-library input cap (32 MiB covers any normal lossless track). It is INTENTIONALLY
     * below the decoder's HC_AUDIO_MAX_BYTES (256 MiB hard adversarial ceiling, which the helper ALSO enforces) —
     * the effective cap is the min of the two, and the host bounds what it hands the child independently. */
    constexpr size_t kAudioInputCap = 32u * 1024u * 1024u;
    if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_size <= 0 || (size_t)fst.st_size > kAudioInputCap) {
        close(fd);
        return false;
    }
    int fl = fcntl(fd, F_GETFL, 0); /* the child does blocking reads of a regular file — clear O_NONBLOCK (tidy) */
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

    int resp[2];
    if (pipe(resp) != 0) {
        close(fd);
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, resp[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fd);
    posix_spawn_file_actions_addclose(&fa, resp[0]);
    posix_spawn_file_actions_addclose(&fa, resp[1]);
    /* Resolve hc_audio_helper next to THIS host binary so a relocated bundle finds it; AUDIO_PROBE_HELPER_PATH (the
     * build-tree path) is the dev fallback. /proc/self/exe-based, no env/argv/CWD influence (app/exe_path.hpp). The
     * helper spawns per audio op (rare), so the readlink cost is negligible. */
    std::string helper = hc::resolve_sibling_exe("hc_audio_helper", AUDIO_PROBE_HELPER_PATH);
    char       *argv[] = {(char *)helper.c_str(), (char *)job, nullptr};
    char *const empty_env[] = {nullptr};
    pid_t       pid = -1;
    int         rc = posix_spawn(&pid, helper.c_str(), &fa, nullptr, argv, empty_env);
    posix_spawn_file_actions_destroy(&fa);
    close(fd);
    close(resp[1]);
    if (rc != 0) {
        close(resp[0]);
        return false;
    }
    *out_read_fd = resp[0];
    *out_pid = pid;
    return true;
}

/* SIGKILL + reap a confined helper. Throwaway: once we have (or abandoned) the reply we never need it alive, so a
 * SIGKILL guarantees no zombie + no hang regardless of the child's state (a clean exit makes the signal a harmless
 * no-op on the already-dead pid). */
static void reap_confined_helper(pid_t pid)
{
    kill(pid, SIGKILL);
    int wst = 0;
    while (waitpid(pid, &wst, 0) < 0 && errno == EINTR) {
    }
}

/* W-Audio.1: probe ONE jailed audio file OUT-OF-PROCESS in a throwaway hc_confine'd child, so the vendored decoders
 * parse the UNTRUSTED bytes in a separate address space with no fs/network/exec — a decoder RCE on a hostile file
 * cannot read the host's API key/state, pivot through the filesystem, or exfiltrate. Returns true + fills *out
 * (every field force-bounded — the reply is untrusted) on a clean probe; false => the caller lists it name-only.
 * Runs on the single-threaded drive loop, so the log-once static is race-free. */
static bool probe_audio_file_confined(hc_sandbox *audio, const std::string &name, hc_audio_meta *out)
{
    int   rfd = -1;
    pid_t pid = -1;
    if (!spawn_confined_helper(audio, name, HC_AUDIO_HELPER_JOB_PROBE, &rfd, &pid)) return false;

    hc_audio_helper_reply reply;
    std::memset(&reply, 0, sizeof reply);
    size_t got = audio_read_exact(rfd, (unsigned char *)&reply, sizeof reply, audio_mono_ms() + 3000);
    close(rfd);
    reap_confined_helper(pid);
    if (got != sizeof reply || !reply.ok) return false;

    /* the reply is UNTRUSTED (the child parsed hostile bytes and could be compromised): force-terminate every meta
     * string and clamp every numeric field to the hc_audio caps before the host uses them. Tag strings are also
     * control-stripped at the decoder source; the UI renders them format-string-safe and the conductor defangs them
     * at the LLM-injection seam — "bounded" is enforced here, "defanged" at each sink. */
    hc_audio_meta m = reply.meta;
    m.title[HC_AUDIO_META_STR - 1] = '\0';
    m.artist[HC_AUDIO_META_STR - 1] = '\0';
    m.album[HC_AUDIO_META_STR - 1] = '\0';
    m.genre[HC_AUDIO_META_STR - 1] = '\0';
    if (m.channels > HC_AUDIO_MAX_CHANNELS) m.channels = 0;
    if (m.samplerate > HC_AUDIO_MAX_SAMPLERATE) m.samplerate = 0;
    if (m.duration_ms > HC_AUDIO_MAX_DURATION_MS) m.duration_ms = 0;
    if ((unsigned)m.fmt > (unsigned)HC_AUDIO_FMT_OGG) m.fmt = HC_AUDIO_FMT_UNKNOWN;
    *out = m;

    static bool logged = false; /* log the helper's TRUE confinement posture once (or its honest absence) */
    if (!logged) {
        logged = true;
        std::fprintf(
            stderr,
            "host: audio probe helper confine: seccomp=%d landlock_fs=%d abi=%d net_denied=%d (status %d)\n",
            reply.seccomp, reply.landlock_fs, reply.landlock_abi, reply.net_denied, reply.confine_status);
    }
    return true;
}

/* W-Audio.2: decode ONE jailed track to interleaved PCM OUT-OF-PROCESS in the same confined helper, then load the
 * RAW SAMPLES into the player — so the untrusted compressed bytes are decoded in a separate address space (no
 * fs/net/exec) and the host runs NO decoder, only clamps integers + plays samples. Reads the fixed header then the
 * bounded PCM under a TOTAL deadline (a wedged/looping decoder can't hang the host), SIGKILL+reaps, validates the
 * untrusted header against the caps, then hc_audio_player_load_pcm + play. Returns false (track unchanged) on any
 * failure. The decode-all-then-play model adds a short start latency vs the old in-host streaming — the accepted
 * cost of keeping the decoder out of the host. */
static bool decode_and_load_confined(hc_audio_player *player, hc_sandbox *audio, const std::string &name)
{
    int   rfd = -1;
    pid_t pid = -1;
    if (!spawn_confined_helper(audio, name, HC_AUDIO_HELPER_JOB_DECODE, &rfd, &pid)) return false;

    /* one TOTAL deadline for the whole (decode + transfer): the header arrives only after the child finishes
     * decoding, then the PCM streams; 12 s bounds a pathological file while a real track returns in ~1-3 s. */
    const long deadline = audio_mono_ms() + 12000;

    hc_audio_helper_decode_reply hdr;
    std::memset(&hdr, 0, sizeof hdr);
    size_t hgot = audio_read_exact(rfd, (unsigned char *)&hdr, sizeof hdr, deadline);

    /* validate the UNTRUSTED header before trusting any size: bound channels/rate/frames against the hc_audio caps
     * AND the transfer cap, so frames*channels*2 cannot overflow or exceed what the helper was allowed to send. */
    bool ok = hgot == sizeof hdr && hdr.ok && hdr.channels >= 1 && hdr.channels <= HC_AUDIO_MAX_CHANNELS &&
              hdr.samplerate >= 1 && hdr.samplerate <= HC_AUDIO_MAX_SAMPLERATE && hdr.frames >= 1 &&
              hdr.frames <= (uint64_t)(HC_AUDIO_MAX_PCM_BYTES / (hdr.channels * sizeof(int16_t)));
    if (!ok) {
        close(rfd);
        reap_confined_helper(pid);
        return false;
    }

    size_t               n_samples = (size_t)hdr.frames * hdr.channels;
    std::vector<int16_t> pcm;
    try {
        pcm.resize(n_samples); /* bounded by the header validation above (<= the 256 MiB transfer cap) */
    } catch (...) {            /* a cap-sized track under memory pressure -> graceful name-only, never terminate */
        close(rfd);
        reap_confined_helper(pid);
        return false;
    }
    size_t pgot = audio_read_exact(rfd, (unsigned char *)pcm.data(), n_samples * sizeof(int16_t), deadline);
    close(rfd);
    reap_confined_helper(pid);
    if (pgot != n_samples * sizeof(int16_t)) return false;

    hc_audio_meta m = hdr.meta; /* untrusted strings — bound them before they reach the now-playing display */
    m.title[HC_AUDIO_META_STR - 1] = '\0';
    m.artist[HC_AUDIO_META_STR - 1] = '\0';
    m.album[HC_AUDIO_META_STR - 1] = '\0';
    m.genre[HC_AUDIO_META_STR - 1] = '\0';
    if ((unsigned)m.fmt > (unsigned)HC_AUDIO_FMT_OGG) m.fmt = HC_AUDIO_FMT_UNKNOWN;

    if (hc_audio_player_load_pcm(player, pcm.data(), hdr.frames, hdr.samplerate, hdr.channels, &m) != 0)
        return false;
    hc_audio_player_play(player);

    static bool logged = false; /* posture once, like the probe path */
    if (!logged) {
        logged = true;
        std::fprintf(
            stderr,
            "host: audio decode helper confine: seccomp=%d landlock_fs=%d abi=%d net_denied=%d (status %d)\n",
            hdr.seccomp, hdr.landlock_fs, hdr.landlock_abi, hdr.net_denied, hdr.confine_status);
    }
    return true;
}
#endif /* AUDIO_PROBE_HELPER_PATH */

/* The library list — THROTTLED (~2 Hz, like files/skills). Each file is metadata-probed ONCE (cached by name);
 * at most a few new files are probed per call so a large library fills in over a few ticks (no UI hitch). The
 * PROBE runs in a CONFINED CHILD (W-Audio.1), so the host never parses untrusted bytes during the library scan;
 * PLAYBACK decode (play_audio_track) runs in the SAME confined child (W-Audio.2), so the host plays only clamped
 * raw samples and NO audio decoder runs in-host on any path. */
void scan_audio_library(std::vector<hc::ui::AudioTrack> &out, hc_sandbox *audio,
                        std::map<std::string, hc::ui::AudioTrack> &cache)
{
    out.clear();
    if (!audio) return;
    hc_sandbox_dirent *ents = nullptr;
    size_t             n = 0;
    if (hc_sandbox_list(audio, ".", &ents, &n) != HC_SANDBOX_OK) return;
    std::unordered_set<std::string> present;
    int                             probed = 0;
    constexpr int                   kProbePerTick = 8;
    for (size_t i = 0; i < n; i++) {
        if (ents[i].is_dir) continue; /* a flat library — files only */
        std::string name = ents[i].name;
        present.insert(name);
        auto it = cache.find(name);
        if (it != cache.end()) { out.push_back(it->second); continue; }
        hc::ui::AudioTrack t;
        t.name = name;
        t.title = name; /* fallback to the file name until probed */
        if (probed < kProbePerTick) {
            probed++;
            hc_audio_meta m;
#ifdef AUDIO_PROBE_HELPER_PATH
            bool ok = probe_audio_file_confined(audio, name, &m); /* untrusted bytes parsed in a confined child */
#else
            bool ok = false; /* no confined helper compiled in -> name-only; NEVER parse untrusted audio in-host */
#endif
            if (ok) {
                if (m.title[0]) t.title = m.title;
                t.artist = m.artist;
                t.duration_ms = (long)m.duration_ms;
                t.fmt = hc_audio_fmt_label(m.fmt);
            } else {
                t.fmt = "?";
            }
            cache[name] = t; /* cached (probed or unreadable) — not re-probed next tick */
        }
        out.push_back(t); /* a cached row, or a name-only placeholder retried next scan */
    }
    hc_sandbox_list_free(ents);
    for (auto it = cache.begin(); it != cache.end();) /* drop vanished files */
        if (!present.count(it->first)) it = cache.erase(it);
        else ++it;
}

/* --- conductor chat attachments (A) ------------------------------------------------------------------ */

/* What a staged file IS, deciding its send action: Text -> minted to the artifact store (the conductor reads it
 * via read_artifact); Image -> shown INLINE to the operator (the conductor is text-only — vision parked); Other
 * -> noted only (a binary has no useful text read). */
enum class AttachKind { Text, Image, Other };
/* One file queued for the NEXT conductor message (A). `bytes` are the jail-read, capped contents (shared into the
 * snapshot for images); `name` is the DEFANGED basename — the only part of an untrusted path ever shown/sent. */
struct StagedAttach {
    std::string                  name;
    AttachKind                   kind = AttachKind::Other;
    bool                         qoi  = false; /* Image only: QOI vs PNG/JPEG, for the UI decoder */
    uint64_t                     hash = 0;     /* content fingerprint (image cache key)           */
    std::shared_ptr<std::string> bytes;
};
/* An image bound to a SENT user message for inline display. `owner_text` is the message's exact composed text —
 * matched against the conversation tail in fill_conductor (stable across the bounded-tail erase, unlike an index). */
struct SentImage {
    std::string                  owner_text;
    std::string                  name;
    bool                         qoi  = false;
    uint64_t                     hash = 0;
    std::shared_ptr<std::string> bytes;
};
struct ChatAttachState {
    std::vector<StagedAttach> staged;      /* attached-but-not-sent (the chip tray)          */
    std::deque<SentImage>     sent_images; /* images bound to sent messages (inline display) */
};
constexpr size_t kMaxStaged     = 12; /* cap the queued-attachment backlog                */
constexpr size_t kMaxSentImages = 24; /* cap the inline-image history (older images drop)  */

void fill_conductor(hc::ui::UiSnapshot &s, hc::conductor::Conductor *c, ChatAttachState &attach)
{
    if (!c) return; /* offline -> conductor_online stays false; the chat panel shows the offline state */
    s.conductor_online = true;
    s.conductor_current_session_id = c->session_id(); /* P3b: highlight the active conversation in the picker */
    hc::conductor::ConductorView v = c->snapshot();
    s.conductor_chat.reserve(v.conversation.size());
    for (const auto &m : v.conversation) s.conductor_chat.push_back({m.role, m.text, m.at_ms, {}});
    s.conductor_streaming = std::move(v.streaming);
    s.conductor_busy = v.busy;
    s.conductor_active_tool = std::move(v.active_tool);
    s.conductor_notice = std::move(v.notice);
    s.conductor_goals.reserve(v.goals.size());
    for (const auto &g : v.goals)
        s.conductor_goals.push_back(
            {g.id, g.title, hc::conductor::goal_status_str(g.status), (int)g.agenda_ids.size()});
    /* A: the staged-attachment chips (queued for the next message) */
    for (const auto &a : attach.staged) s.conductor_staged.push_back({a.name, a.kind == AttachKind::Image});
    /* A: bind each sent image to its message by exact composed-text match (index-free, so the bounded-tail erase
     * can't misalign it) — the conductor stays text-only; this is operator-facing display only. */
    for (auto &msg : s.conductor_chat) {
        if (msg.role != "user") continue;
        for (const auto &si : attach.sent_images)
            if (si.owner_text == msg.text) msg.attachments.push_back({si.name, si.bytes, si.hash, si.qoi});
    }
}

namespace {

hc::ui::Accent accent_from_str(const std::string &a)
{
    /* "white" + any unrecognized string -> White (the safe default; an unknown accent in the file degrades). */
    if (a == "cyan") return hc::ui::Accent::Cyan;
    if (a == "amber") return hc::ui::Accent::Amber;
    if (a == "emerald") return hc::ui::Accent::Emerald;
    if (a == "violet") return hc::ui::Accent::Violet;
    if (a == "crimson") return hc::ui::Accent::Crimson;
    return hc::ui::Accent::White;
}

const char *accent_to_str(hc::ui::Accent a)
{
    switch (a) {
    case hc::ui::Accent::Cyan: return "cyan";
    case hc::ui::Accent::Amber: return "amber";
    case hc::ui::Accent::Emerald: return "emerald";
    case hc::ui::Accent::Violet: return "violet";
    case hc::ui::Accent::Crimson: return "crimson";
    case hc::ui::Accent::White: return "white";
    }
    return "white";
}

/* The edited UI draft (SaveSettings) -> hcapp::Settings to persist. The key is NEVER here; export_key_to_env
 * is a SESSION-only toggle (carried separately on the command), deliberately not persisted (the SSRF/secret-
 * exposure weakening resets to OFF every launch). data_dir/ephemeral round-trip but stay bootstrap-resolved. */
Settings from_ui_settings(const hc::ui::UiSettings &u)
{
    Settings s;
    s.accent = accent_to_str(u.accent);
    s.mascot = u.mascot;
    s.poll_hz = u.poll_hz;
    s.model = u.model;
    s.base_url = u.base_url;
    s.embed_model = u.embed_model;
    s.data_dir = u.data_dir;
    s.ephemeral = u.ephemeral;
    s.llm_call_total_ms = u.llm_call_total_ms;
    s.llm_connect_ms = u.llm_connect_ms;
    s.deep_reason_budget = u.deep_reason_budget;
    s.task_deadline_ms = u.task_deadline_ms;
    s.egress_allow = u.egress_allow;
    s.exec_allow = u.exec_allow;
    s.auto_approve_contained = u.auto_approve_contained; /* B3 */
    s.allow_all_approvals = u.allow_all_approvals;       /* B4 */
    /* models (W2): the UI catalog -> the settings catalog; the assignment grid (vector of pairs) -> the map. */
    for (const auto &me : u.models) s.models.push_back({me.id, me.note});
    for (const auto &kv : u.role_models)
        if (!kv.first.empty() && !kv.second.empty()) s.role_models[kv.first] = kv.second;
    return s;
}

/* Reflect the host settings state into the per-frame snapshot. */
/* A one-line human description for a built-in System Tool (the Tools panel detail; the full spec lives in the
 * worker, a separate process). Keyed by the role_tool_name id. ADD A CASE when adding a RoleTool — an unlisted
 * tool falls back to a generic description (a cosmetic gap caught at the next UI review, never a crash/stub). */
static const char *system_tool_desc(const std::string &name)
{
    if (name == "deep_reason") return "staged multi-step reasoning for one hard question";
    if (name == "memory_recall") return "search the semantic memory store (read-only)";
    if (name == "memory_write") return "save a durable note to memory (shared scope is operator-gated)";
    if (name == "fs_read") return "read a file from the workspace (sandboxed)";
    if (name == "fs_list") return "list a workspace directory (sandboxed)";
    if (name == "fs_write") return "create or replace a file in the workspace (operator-gated)";
    if (name == "fs_update") return "edit a file in place (operator-gated)";
    if (name == "load_skill") return "load a per-project skill's instructions on demand";
    return "a built-in System Tool";
}

/* ---- Wave E: the third-party tool MANAGEMENT backend (drop-in package -> review -> consent-enable -> remove) ---- */

/* a single-component id is traversal-safe (it IS the install dir name); reject anything else as defense in depth. */
bool tool_id_ok(const std::string &id)
{
    if (id.empty() || id.size() > 128 || id == "." || id == "..") return false;
    return id.find('/') == std::string::npos && id.find('\\') == std::string::npos;
}

/* (the manifest.lock READER lives in hc_toolhost — hcapp::read_tool_lock — shared with the launch-time pin
 * verify so the lock-file format has one owner.) */

/* compute + WRITE <dir>/manifest.lock over the CURRENT package bytes — the operator-approval moment on enable
 * (the consent modal gated this). A later launch re-verifies the lock, so a byte change after approval is caught. */
bool write_tool_lock(const std::string &tools_root, const std::string &id)
{
    if (!tool_id_ok(id)) return false;
    std::string dir = tools_root + "/" + id;
    size_t      mlen = 0;
    char       *mbuf = hc_fs_read_file((dir + "/manifest.json").c_str(), 64u * 1024, &mlen);
    if (!mbuf) return false;
    std::string hex = hcapp::tool_lock_hex(dir, mbuf, mlen);
    free(mbuf);
    if (hex.empty()) return false;
    FILE *f = std::fopen((dir + "/manifest.lock").c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(hex.data(), 1, hex.size(), f) == hex.size();
    std::fclose(f);
    return ok;
}

/* recursively remove a tool package dir (the Remove path; the caller bounds `path` to the install root + a
 * validated single-component id). DEFENSIVE (security review F2): a single-component id can still POINT outside
 * via a symlinked dir, so we lstat the root (and every entry) and NEVER follow a symlink — a symlinked root or
 * entry is unlinked (the link itself), never recursed through, so a planted `<tools_root>/<id> -> /elsewhere`
 * deletes only the link. Mirrors main.cpp's nftw(FTW_PHYS) rm_rf (the sibling remover; see its banner). */
void rm_tree(const std::string &path)
{
    struct stat root;
    if (lstat(path.c_str(), &root) != 0) return;            /* gone already */
    if (!S_ISDIR(root.st_mode)) { unlink(path.c_str()); return; } /* a symlink/file root: drop the link, don't follow */
    if (DIR *d = opendir(path.c_str())) {
        for (struct dirent *e; (e = readdir(d));) {
            std::string nm = e->d_name;
            if (nm == "." || nm == "..") continue;
            std::string p = path + "/" + nm;
            struct stat stt;
            if (lstat(p.c_str(), &stt) == 0 && S_ISDIR(stt.st_mode)) rm_tree(p);
            else unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

/* a one-line limits summary for the PERMISSIONS table (honest: only the timeout is enforced in v1). */
std::string tool_limits_str(const hcapp::ToolManifest &m)
{
    char b[176];
    std::snprintf(b, sizeof b, "timeout %dms (enforced); cpu %ds, mem %ldMiB, %d files (declared, v1-reserved)",
                  m.timeout_ms, m.cpu_seconds, m.mem_bytes / (1024 * 1024), m.max_files);
    return b;
}

/* Scan the install root for third-party PACKAGES (one dir each) -> one Tools-panel row per package. Cheap per
 * frame (a small dir scan + manifest read + a 64-byte lock read — no binary hash; the byte-exact pin verify is
 * the ToolHost's launch-time job). enabled = the operator setting AND the kill-switch is clear; disclaimed = a
 * manifest.lock exists (the operator already approved this package; a fresh drop-in has none -> consent on first
 * enable); source notes whether the ToolHost has it running. Manifest strings are author-supplied -> "%s". */
void fill_third_party_tools(hc::ui::UiSnapshot &s, const HostServices &svc)
{
    if (svc.tools_root.empty() || !svc.settings) return;
    /* Re-check the host-private gate at the point of use (security review F1): the startup path refuses to walk a
     * non-0700 install root; the live panel scan must hold the same line, not trust that it was checked once. A
     * missing dir is simply nothing to show; a non-private dir is refused (matches main's stance). Cheap (a stat). */
    if (!hcapp::dir_is_host_private(svc.tools_root)) return;
    const auto                     &st = svc.settings->settings;
    std::unordered_set<std::string> running; /* "tool:<id>" the ToolHost has confirmed up */
    if (svc.toolhost)
        for (const auto &fn : svc.toolhost->functions()) running.insert(fn.tool_id);
    DIR *d = opendir(svc.tools_root.c_str());
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        std::string id = e->d_name;
        if (!tool_id_ok(id)) continue;
        std::string dir = svc.tools_root + "/" + id;
        size_t      mlen = 0;
        char       *mbuf = hc_fs_read_file((dir + "/manifest.json").c_str(), 64u * 1024, &mlen);
        if (!mbuf) continue; /* not a package */
        hcapp::ToolManifest man;
        std::string         err;
        bool                ok = hcapp::tool_manifest_parse(mbuf, mlen, id, man, err);
        free(mbuf);
        if (!ok) continue; /* an invalid/rejected package is not shown (a future pass could surface it as an error row) */

        hc::ui::ToolRow r;
        r.name = id; /* the package id is the toggle/remove key (ToggleTool/RemoveTool a=id) */
        r.kind = hc::ui::ToolRow::Kind::ThirdParty;
        auto it = st.thirdparty_tools.find(id);
        r.enabled = (it != st.thirdparty_tools.end() && it->second) && st.thirdparty_tools_enabled;
        r.disclaimed = !read_tool_lock(dir).empty(); /* a lock exists => already operator-approved (no re-consent) */
        r.description = !man.description.empty() ? man.description : (man.name.empty() ? id : man.name);
        std::string fns; /* the package's exposed functions, shown in the detail */
        for (const auto &fn : man.tools) {
            if (!fns.empty()) fns += ", ";
            fns += fn.name;
        }
        r.spec_json = fns;
        if (man.fs_mode != hcapp::ToolFsMode::None)
            r.manifest.fs_scopes.push_back(std::string("workspace (") + hcapp::tool_fs_mode_str(man.fs_mode) + ")");
        r.manifest.egress_hosts = man.egress_hosts; /* declared intent; unrestricted at the floor (see DISCLAIMER) */
        r.manifest.exec_paths = man.exec_allow;     /* empty in v1 (exec rejected at install) */
        r.manifest.resource_limits = tool_limits_str(man);
        r.manifest.source = running.count("tool:" + id) ? "installed (running)" : "installed";
        r.manifest.version = man.version;
        s.tools.push_back(std::move(r));
    }
    closedir(d);
}

void fill_settings(hc::ui::UiSnapshot &s, const HostServices &svc)
{
    if (svc.settings) s.settings = to_ui_settings(*svc.settings);
    /* Wave A: the per-project persona override (a persistent project only). The global default + the locked
     * spine + presets came from to_ui_settings; this adds the project tier the editor lets the operator set. */
    if (!svc.project_dir.empty()) {
        s.settings.conductor_persona_per_project = true;
        s.settings.conductor_persona_project = read_project_persona(svc.project_dir);
    }
    /* Wave C: the Tools panel rows. System Tools = the built-in worker tool set (role_tool_name is the
     * authoritative list the role editor already uses); enabled = the global system_tools toggle (missing => ON).
     * Wave D appends the live third-party functions the ToolHost is running. */
    if (svc.settings) {
        const auto &st = svc.settings->settings;
        s.third_party_tools_disabled = !st.thirdparty_tools_enabled;
        s.third_party_conductor = st.thirdparty_tools_conductor; /* D4c: the conductor opt-in (Tools panel checkbox) */
        s.tools.clear();
        for (size_t i = 0; i < hcapp::role_tool_count(); i++) {
            const char *nm = hcapp::role_tool_name((hcapp::RoleTool)i);
            auto        it = st.system_tools.find(nm);
            hc::ui::ToolRow r;
            r.name = nm;
            r.kind = hc::ui::ToolRow::Kind::System;
            r.enabled = (it == st.system_tools.end()) ? true : it->second; /* missing => ON */
            r.description = system_tool_desc(nm);
            s.tools.push_back(std::move(r));
        }
    }
    /* Wave E: the installed third-party PACKAGES (one row each), scanned from the install root with their
     * declared manifest + enable/approval/running state — the management view (enable, review, remove). */
    fill_third_party_tools(s, svc);
}

/* FNV-1a (64-bit) over a byte span — the Viewer's CONTENT fingerprint for an opened file. Computed once on
 * OpenFile so the UI's texture cache keys on content (a re-open of identical bytes hits; a changed file
 * misses), and it is the change signal the W5 IDE live-diff will compare against. Not a security hash. */
uint64_t fnv1a64(const unsigned char *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* A cheap magic-byte sniff so the OpenFile read uses a larger cap for IMAGES (which need the whole compressed
 * stream to decode) than for text (whose render + per-frame snapshot copy we keep tight). PNG/JPEG/QOI only —
 * the formats the Viewer can actually display; everything else takes the text cap. No hc_ui dependency. */
bool looks_like_image(const unsigned char *p, size_t n)
{
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G' && p[4] == 0x0D && p[5] == 0x0A &&
        p[6] == 0x1A && p[7] == 0x0A)
        return true;                                                   /* PNG  */
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return true; /* JPEG */
    if (n >= 4 && p[0] == 'q' && p[1] == 'o' && p[2] == 'i' && p[3] == 'f') return true; /* QOI */
    return false;
}

/* Read a workspace file through the JAILED sandbox into `out` (image-aware cap: up to 8 MiB for an image so
 * it decodes, else truncated to 256 KiB so the text render + snapshot copy stay tight), with its true size
 * + an FNV-1a content fingerprint. The single read path shared by the OpenFile command and the open-file
 * change WATCH (W5 P5.2). Returns false (out cleared) if the path can't be opened in the jail. */
bool read_open_file(hc_sandbox *ws, const std::string &path, std::string &out, long &out_size, uint64_t &out_hash)
{
    out.clear();
    out_size = 0;
    out_hash = 0;
    if (!ws || path.empty()) return false;
    hc_sandbox_fd fd = HC_SANDBOX_FD_INVALID;
    if (hc_sandbox_open_fd(ws, path.c_str(), O_RDONLY, 0, &fd) != HC_SANDBOX_OK || fd < 0) return false;
    struct stat fst;
    /* out_size is the file's TRUE size — NOT the capped read length. The editor's data-loss guard
     * (open_size > open_bytes.size() => block editing) depends on this: if this ever returned the capped
     * size instead, a truncated file would look complete and a save could destroy the unseen tail. */
    if (fstat(fd, &fst) == 0) out_size = (long)fst.st_size;
    constexpr size_t kImageCap = 8u * 1024u * 1024u; /* 8 MiB compressed-image input ceiling */
    constexpr size_t kTextCap = 256u * 1024u;        /* text render + snapshot-copy bound     */
    std::string      buf(kImageCap, '\0');
    size_t           total = 0;
    while (total < kImageCap) {
        ssize_t r = read(fd, &buf[total], kImageCap - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    close(fd);
    buf.resize(total);
    if (!looks_like_image((const unsigned char *)buf.data(), buf.size()) && buf.size() > kTextCap)
        buf.resize(kTextCap);
    out_hash = fnv1a64((const unsigned char *)buf.data(), buf.size());
    out = std::move(buf);
    return true;
}

/* --- conductor chat attachment stage/send helpers (A) ------------------------------------------------ */

/* basename: the trailing path component — the only part of an untrusted path we ever display/send. */
std::string attach_basename(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

/* A printable-text sniff: a file the conductor can usefully READ (worth minting) vs opaque binary. Bounded scan;
 * a NUL or a high control-byte density => binary. */
bool looks_like_text(const unsigned char *p, size_t n)
{
    if (!p || n == 0) return false;
    const size_t scan = n < 4096 ? n : 4096;
    size_t       ctrl = 0;
    for (size_t i = 0; i < scan; i++) {
        const unsigned char c = p[i];
        if (c == 0) return false;                       /* a NUL -> binary */
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ctrl++; /* a control byte (not \t\n\v\f\r) */
    }
    return ctrl * 32 < scan; /* < ~3% control bytes -> text */
}

/* Classify already-read bytes (image / readable-text / other) + queue them for the next message — shared by the
 * jailed (A.3) and os-drop (A.4) stage paths. The display name is DEFANGED here, the one place it is set. */
bool stage_classified(ChatAttachState &st, const std::string &display_name, std::string &&body, uint64_t hash)
{
    if (st.staged.size() >= kMaxStaged) return false;
    StagedAttach a;
    a.name = hcapp::defang_inline(display_name, {"[", "]"});
    a.hash = hash;
    const unsigned char *p = (const unsigned char *)body.data();
    if (looks_like_image(p, body.size())) {
        a.kind = AttachKind::Image;
        a.qoi  = body.size() >= 4 && p[0] == 'q' && p[1] == 'o' && p[2] == 'i' && p[3] == 'f';
    } else if (looks_like_text(p, body.size())) {
        a.kind = AttachKind::Text;
    }
    a.bytes = std::make_shared<std::string>(std::move(body));
    st.staged.push_back(std::move(a));
    return true;
}

/* Read an ABSOLUTE OS path the operator DRAGGED onto the window (A.4) — the ONE non-jailed file read in the host,
 * so it is hardened: O_NOFOLLOW (a symlink at the final component is refused) + reject anything but a regular file
 * + the SAME image/text byte caps as the jailed read. It is operator-initiated (she dragged her own file; the
 * human IS the AuthGate) and the bytes are content-addressed + defanged downstream — so this widens the FS read
 * surface only to regular files the operator explicitly chose. Returns false (out cleared) on ANY failure. */
bool read_os_file(const std::string &path, std::string &out, uint64_t &out_hash)
{
    out.clear();
    out_hash = 0;
    if (path.empty()) return false;
    /* O_NONBLOCK so a FIFO-with-no-writer or a device returns from open() IMMEDIATELY instead of wedging this UI
     * thread (read_os_file runs in the per-frame loop) — the same FIFO/device-wedge defense the jailed audio read
     * uses (spawn_confined_helper); cleared below once S_ISREG holds. O_NOFOLLOW refuses a symlink at the final
     * component. NOTE (accepted residual): a /proc or /sys pseudo-file is S_ISREG and can synthesize content on
     * read; the st_size>0 gate rejects the common st_size==0 cases, but a /sys page-sized entry could still be
     * dragged — an operator foot-gun (she dragged her own path; the human IS the AuthGate), not an agent path. */
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat fst;
    if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_size <= 0) { /* dir/dev/fifo/symlink/empty/pseudo */
        close(fd);
        return false;
    }
    int fl = fcntl(fd, F_GETFL, 0); /* a regular file never returns EAGAIN -> clear O_NONBLOCK so the reads block */
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    constexpr size_t kImageCap = 8u * 1024u * 1024u;
    constexpr size_t kTextCap  = 256u * 1024u;
    std::string      buf(kImageCap, '\0');
    size_t           total = 0;
    while (total < kImageCap) {
        ssize_t r = read(fd, &buf[total], kImageCap - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    close(fd);
    buf.resize(total);
    if (!looks_like_image((const unsigned char *)buf.data(), buf.size()) && buf.size() > kTextCap)
        buf.resize(kTextCap);
    out_hash = fnv1a64((const unsigned char *)buf.data(), buf.size());
    out = std::move(buf);
    return true;
}

/* Read + classify one JAILED workspace file and queue it (A.3). False on a jail-read failure / full queue. */
bool stage_attach_file(ChatAttachState &st, hc_sandbox *ws, const std::string &path)
{
    std::string body;
    long        sz = 0;
    uint64_t    h  = 0;
    if (!read_open_file(ws, path, body, sz, h)) return false;
    return stage_classified(st, attach_basename(path), std::move(body), h);
}

/* Read + classify one DRAGGED OS file and queue it (A.4 — the hardened non-jailed path). */
bool stage_attach_os_file(ChatAttachState &st, const std::string &os_path)
{
    std::string body;
    uint64_t    h = 0;
    if (!read_os_file(os_path, body, h)) return false;
    return stage_classified(st, attach_basename(os_path), std::move(body), h);
}

/* Compose the operator's typed text + the staged attachments into ONE conductor turn and send it (A.0/A.1): a
 * text file is minted to the artifact store (the conductor reads it via read_artifact), an image gets an honest
 * "not visible to you" note + is bound for inline display, anything else is noted (never minted — a binary has no
 * useful read). On a successful say() the queue is cleared + images committed; on a busy/budget refusal NOTHING is
 * consumed (the content-addressed mint is idempotent, so a retry re-uses the same ids). Returns say()'s result. */
bool send_with_attachments(ChatAttachState &st, hc::conductor::Conductor *c, hc_artifacts *art,
                           const std::string &operator_text)
{
    if (!c) return false;
    std::string            composed = operator_text;
    std::vector<SentImage> pend; /* images to bind once `composed` is final */
    for (const auto &a : st.staged) {
        if (!composed.empty()) composed += "\n\n";
        if (a.kind == AttachKind::Text && art && a.bytes) {
            char id[HC_ARTIFACT_ID_LEN] = {0};
            if (hc_artifacts_put(art, a.bytes->data(), a.bytes->size(), id) == 0)
                composed += "[attached file: " + a.name + " \xE2\x80\x94 artifact " + id
                            + "; use read_artifact to view it]";
            else
                composed += "[attached file: " + a.name + " \xE2\x80\x94 could not be stored]";
        } else if (a.kind == AttachKind::Image) {
            composed += "[attached image: " + a.name + " \xE2\x80\x94 not visible to you]";
            pend.push_back(SentImage{std::string(), a.name, a.qoi, a.hash, a.bytes});
        } else {
            composed += "[attached file: " + a.name + " \xE2\x80\x94 binary, not readable]";
        }
    }
    if (composed.empty()) return false;  /* nothing to send */
    if (!c->say(composed)) return false; /* busy / budget — keep the queue for a retry */
    for (auto &si : pend) {
        si.owner_text = composed;
        st.sent_images.push_back(std::move(si));
        while (st.sent_images.size() > kMaxSentImages) st.sent_images.pop_front();
    }
    st.staged.clear();
    return true;
}

/* Operator-direct JAILED ATOMIC write (W5 P5.3): write `content` to a temp sibling, fsync it, then rename it
 * over `path` (an atomic replace — a crash leaves either the old file or the new, never a torn one). Composes
 * the EXISTING reviewed sandbox primitives only — `hc_sandbox_open_fd(O_CREAT|O_TRUNC|O_NOFOLLOW)` (a symlink
 * planted at the temp name is refused) + `hc_sandbox_rename` (both jailed, parent-walked with O_NOFOLLOW) — so
 * NO new sandbox surface. On any failure best-effort unlinks the temp and returns the failing status. The
 * content is the operator's edit, bounded by the editor; the operator IS the human-in-the-loop (agent writes
 * stay AuthGate-gated). */
hc_sandbox_status write_open_file_atomic(hc_sandbox *ws, const std::string &path, const std::string &content)
{
    if (!ws || path.empty()) return HC_SANDBOX_ERR_INVALID;
    const std::string tmp = path + ".hcsave~"; /* a sibling in the same jailed dir (same-dir rename = atomic) */
    /* Clear any stale temp (a prior crash between create and rename) then create EXCLUSIVELY: O_EXCL + the
     * sandbox's forced O_NOFOLLOW means we never reuse/clobber an existing file or write through a symlink. */
    hc_sandbox_unlink(ws, tmp.c_str(), 0);
    hc_sandbox_fd     fd = HC_SANDBOX_FD_INVALID;
    hc_sandbox_status st = hc_sandbox_open_fd(ws, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600, &fd);
    if (st != HC_SANDBOX_OK || fd < 0) return st == HC_SANDBOX_OK ? HC_SANDBOX_ERR_IO : st;
    bool        ok = true;
    const char *p = content.data();
    size_t      left = content.size();
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) {
            ok = false;
            break;
        }
        p += (size_t)w;
        left -= (size_t)w;
    }
    if (ok && fsync(fd) != 0) ok = false; /* durability before the rename commits */
    close(fd);
    if (!ok) {
        hc_sandbox_unlink(ws, tmp.c_str(), 0);
        return HC_SANDBOX_ERR_IO;
    }
    st = hc_sandbox_rename(ws, tmp.c_str(), path.c_str());
    if (st != HC_SANDBOX_OK) hc_sandbox_unlink(ws, tmp.c_str(), 0);
    return st;
}

} // namespace

hc::ui::UiSettings to_ui_settings(const SettingsState &st)
{
    const Settings     &s = st.settings;
    const EnvOverrides &o = st.ov;
    hc::ui::UiSettings  u;
    u.accent = accent_from_str(s.accent);
    u.poll_hz = s.poll_hz;
    u.mascot = s.mascot;
    u.model = s.model;
    u.base_url = s.base_url;
    u.embed_model = s.embed_model;
    u.data_dir = s.data_dir;
    u.ephemeral = s.ephemeral;
    u.llm_call_total_ms = s.llm_call_total_ms;
    u.llm_connect_ms = s.llm_connect_ms;
    u.deep_reason_budget = s.deep_reason_budget;
    u.task_deadline_ms = s.task_deadline_ms;
    u.egress_allow = s.egress_allow;
    u.exec_allow = s.exec_allow;
    u.auto_approve_contained = s.auto_approve_contained; /* B3 */
    u.allow_all_approvals = s.allow_all_approvals;       /* B4 */
    /* models (W2): the settings catalog -> the UI catalog; the role->model map -> the assignment grid (pairs). */
    for (const auto &me : s.models) u.models.push_back({me.id, me.note});
    for (const auto &kv : s.role_models) u.role_models.emplace_back(kv.first, kv.second);
    u.key_present = st.secrets && hc_secrets_has(st.secrets, "OPENROUTER_API_KEY");
    /* keychain availability is a SYNCHRONOUS D-Bus probe -> never call it every render frame (60Hz). Re-probe on
     * a coarse cadence (~ every 2s @60fps) + cache; a mid-session keyring unlock still reflects within the
     * interval. UI-thread-only function, so the static counter needs no lock. Drives the "persists" wording. */
    {
        static bool keychain_avail = false;
        static int  keychain_probe = 0;
        if ((keychain_probe++ % 120) == 0) keychain_avail = hc_secrets_keychain_available();
        u.keychain_available = keychain_avail;
    }
    u.export_key_to_env = false; /* session-only; the UI draft holds the operator's per-session toggle */
    u.ov_model = o.model;
    u.ov_base_url = o.base_url;
    u.ov_embed_model = o.embed_model;
    u.ov_data_dir = o.data_dir;
    u.ov_ephemeral = o.ephemeral;
    u.ov_llm_call_total_ms = o.llm_call_total_ms;
    u.ov_llm_connect_ms = o.llm_connect_ms;
    u.ov_deep_reason_budget = o.deep_reason_budget;
    u.ov_task_deadline_ms = o.task_deadline_ms;
    u.ov_egress_allow = o.egress_allow;
    /* conductor personality (Wave A): the GLOBAL default slot + the LOCKED spine/default for the preview + the
     * preset table. The per-project override + the per_project-available flag are filled in fill_settings (they
     * need the project dir, which lives on HostServices, not in SettingsState). */
    u.conductor_persona = s.conductor_persona;
    u.conductor_spine_identity = conductor_spine_identity();
    u.conductor_persona_default = conductor_persona_default();
    u.conductor_spine_floor = conductor_spine_floor();
    {
        std::size_t                 np = 0;
        const hcapp::PersonaPreset *pp = hcapp::persona_presets(&np);
        for (std::size_t i = 0; i < np; i++) {
            if (std::string(pp[i].key) == "custom") continue; /* "custom" is implicit (any edit) — not a pickable text */
            u.persona_presets.emplace_back(pp[i].label, pp[i].text);
        }
    }
    return u;
}

void inject_settings_env(const Settings &s)
{
    if (!s.model.empty()) setenv("HC_MODEL", s.model.c_str(), 1);
    if (!s.base_url.empty()) setenv("HC_BASE_URL", s.base_url.c_str(), 1);
    if (!s.embed_model.empty()) setenv("HC_EMBED_MODEL", s.embed_model.c_str(), 1);
    setenv("HC_LLM_CALL_TOTAL_MS", std::to_string(s.llm_call_total_ms).c_str(), 1);
    setenv("HC_LLM_CONNECT_MS", std::to_string(s.llm_connect_ms).c_str(), 1);
    setenv("HC_DEEP_REASON_BUDGET", std::to_string(s.deep_reason_budget).c_str(), 1);
    setenv("HC_TASK_DEADLINE_MS", std::to_string(s.task_deadline_ms).c_str(), 1);
    if (!s.egress_allow.empty()) {
        std::string csv;
        for (size_t i = 0; i < s.egress_allow.size(); i++) {
            if (i) csv += ',';
            csv += s.egress_allow[i];
        }
        setenv("HC_EGRESS_ALLOW", csv.c_str(), 1);
    }
}

/* W1.3: did the worker actually materialize its declared deliverable? Opens the agent's OWN jail (mirrors
 * read_current_file — "../escape" refused, never a sibling's tree) and confirms `path` is a NON-EMPTY regular
 * file. The orchestrator calls this (via set_deliverable_verifier) before accepting a Done result, so an
 * agenda never reports a false Done for a task that claimed success but wrote no file. Cheap (fstat, no read).
 * Called on the orchestrator driver thread — pure filesystem syscalls, no shared host state touched. */
bool deliverable_present(hc_sandbox *ws, const std::string &agent, const std::string &path, bool shared)
{
    if (!ws || path.empty()) return false;
    const char *root = hc_sandbox_root(ws);
    if (!root) return false;
    std::string agent_dir = std::string(root) + "/" + ws_subdir(agent, shared);
    hc_sandbox *jail = hc_sandbox_open(agent_dir.c_str(), nullptr);
    if (!jail) return false;
    bool          ok = false;
    hc_sandbox_fd fd = HC_SANDBOX_FD_INVALID;
    if (hc_sandbox_open_fd(jail, path.c_str(), O_RDONLY, 0, &fd) == HC_SANDBOX_OK) {
        struct stat st;
        ok = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
        close(fd);
    }
    hc_sandbox_close(jail);
    return ok;
}

std::vector<std::string> distinct_capabilities(const Pool &pool)
{
    std::vector<std::string> caps;
    for (const auto &w : pool) {
        bool seen = false;
        for (const auto &c : caps)
            if (c == w.second) seen = true;
        if (!seen) caps.push_back(w.second);
    }
    return caps;
}

hc_llm *open_chat_llm(const char *base_url, const char *model, const char *key, hc_http **out_http)
{
    *out_http = nullptr;
    if (!model || !*model || !key || !*key) return nullptr;
    if (!hc_http_global_init()) return nullptr;
    hc_http *http = hc_http_new();
    if (!http) {
        hc_http_global_shutdown(); /* balance the init (refcounted; other clients keep their own ref) */
        return nullptr;
    }
    hc_http_set_timeouts_ms(http, 60000, 10000);
    static const char *hdrs[] = {"HTTP-Referer: https://hypercat.local", "X-Title: HyperCat", nullptr};
    hc_llm_provider    p = {};
    p.name = "openrouter";
    p.base_url = (base_url && *base_url) ? base_url : "https://openrouter.ai/api/v1";
    p.api_key = key;
    p.model = model;
    p.extra_headers = hdrs;
    hc_llm *llm = hc_llm_new(&p, http);
    if (!llm) {
        hc_http_free(http);
        hc_http_global_shutdown();
        return nullptr;
    }
    *out_http = http;
    return llm;
}

void close_chat_llm(hc_llm *llm, hc_http *http)
{
    if (llm) hc_llm_free(llm);
    if (http) hc_http_free(http);
    hc_http_global_shutdown(); /* balances open_chat_llm's init */
}

int consolidate_sessions(const HostServices &svc, const char *base_url, const char *model,
                         const char *key)
{
    if (!svc.store || !svc.mbroker) return 0;
    hc_http *http = nullptr;
    hc_llm  *llm = open_chat_llm(base_url, model, key, &http); /* a CHAT client (not the broker's embed) */
    if (!llm) return 0;
    hc_agent_backend be = hc_agent_hosted_backend(llm);

    hc_session_info *list = nullptr;
    size_t           n = 0;
    int              written = 0;
    if (hc_store_list(svc.store, &list, &n) == 0) {
        hc::host::MemoryBroker *mb = svc.mbroker;
        /* the human-trusted host write: the operator initiated this pass, so a distilled fact is an
         * operator-authored memory (like a seed file), carrying the session id as mandatory provenance. */
        hc::consolidation::WriteFn sink = [mb](const std::string &scope, const std::string &text,
                                               const std::string &source) {
            return mb->seed(scope, text, source) == 0;
        };
        for (std::size_t i = 0; i < n; i++) {
            std::string id = list[i].id;
            if (hc::consolidation::is_consolidated(svc.store, id)) continue; /* already done in a prior run */
            hc::consolidation::compact_session(svc.store, be, id);    /* episode summary beside the raw */
            written += (int)hc::consolidation::distill_session(svc.store, be, sink, id).written;
            hc::consolidation::mark_consolidated(svc.store, id); /* idempotency: process each session once */
        }
        hc_store_list_free(list, n);
    }
    close_chat_llm(llm, http);
    return written;
}

void sync_fleet_filters(const HostServices &svc, const std::vector<std::string> &ids)
{
    std::unordered_set<std::string> known(ids.begin(), ids.end());
    if (svc.adapter) svc.adapter->set_known_agents(known);
    if (svc.gate) svc.gate->set_known_agents(known);
    if (svc.mbroker) svc.mbroker->set_known_agents(known);
    if (svc.capauth) svc.capauth->set_known_agents(known); /* P09: filter + revoke-on-reap (a departed id's caps) */
}

std::string run_live_loop(hc::ui::UiApp &ui, Orchestrator &orch_, Supervisor &sup, Fleet &fleet, bool live,
                          const char *model, const HostServices &svc)
{
    std::string                      switch_to;    /* W3 P3.2: non-empty -> the project id to re-exec into */
    std::vector<hc::ui::FileEntry>   files_cache; /* host-listed at ~2Hz, not every frame (stable) */
    bool                             files_truncated_cache = false; /* the recursive listing hit a cap */
    std::vector<hc::ui::SessionRow>  sessions_cache;
    std::vector<hc::ui::ConductorConversationRow> conductor_conv_cache; /* P3b: the conversations picker (~2Hz) */
    std::vector<hc::ui::ArtifactRow> artifacts_cache; /* P02 produced outputs (refreshed at ~2Hz)  */
    std::vector<hc::ui::SkillRow>    skills_cache;     /* W6 P6.3: the Skills panel list (refreshed at ~2Hz + on CRUD) */
    std::vector<hc::ui::AudioTrack>  audio_lib_cache;  /* Music Player: the library list (throttled scan)        */
    std::map<std::string, hc::ui::AudioTrack> audio_probe_cache; /* per-file metadata, probed once               */
    std::string                      current_audio_name; /* the now-playing LIBRARY track (drives AudioNext/Prev) */
    std::string                      viewed_id;
    std::vector<std::string>         viewed_transcript;
    std::string                      opened_path;    /* W4 P4.2: the file open in the Viewer ("" = none) */
    std::shared_ptr<const std::string> opened_bytes; /* its content (read once on OpenFile; SHARED into the
                                                       * snapshot so the per-frame rebuild is a refcount bump) */
    long                             opened_size = 0; /* the file's true byte size                        */
    uint64_t                         opened_hash = 0; /* FNV-1a of opened_bytes (the Viewer texture key)  */
    /* W5 P5.2: the open-file change WATCH — the host re-reads the open file on the ~2 Hz throttle; if the disk
     * version differs from the DISPLAYED (opened_*) version, it flags it + computes the diff once. The editor
     * shows a "changed on disk" banner + the reused diff renderer; [Reload] re-opens (adopts the disk version). */
    bool                             open_changed = false;     /* disk differs from the displayed version */
    hc::ui::PendingDiff              open_diff;                /* displayed -> disk (text; empty for image) */
    uint64_t                         watched_disk_hash = 0;    /* the last disk hash diffed (skip re-diffs) */
    std::string                      selected_skill;           /* W6 P6.3: the skill open in the Skills editor */
    std::shared_ptr<const std::string> selected_skill_body;   /* its SKILL.md content (read once on OpenSkill)  */
    std::string                      term_buf;       /* the terminal output, tail-bounded         */
    std::string                      notice;         /* transient status from the last action      */
    int                              notice_ttl = 0; /* frames the notice stays up (~3s at 60fps)  */
    /* B3/B4: the persistent-toast ring (AutoApproved traces, the allow-all armed Warning) — each {toast, ttl_frames}.
     * Distinct from the approval-pending toasts, which are rebuilt from the live pending list each frame. */
    std::vector<std::pair<hc::ui::Toast, int>> toast_ring;
    TimelineLog                      timeline;       /* activity-span accumulator (P10)            */
    std::unordered_map<std::string, hc::ui::PendingDiff> diff_cache; /* fs_write diffs, computed once (P11) */
    int                              tick = 0;
    ChatAttachState                  chat_attach; /* A: conductor chat attachments (staged queue + sent-image history) */
    for (;;) {
        Pool pool = fleet.pool(); /* the LIVE roster each frame — reflects a runtime add/remove worker */
        hc::ui::UiSnapshot s = build_snapshot(orch_, sup, pool, svc.roles ? &svc.roles->table : nullptr);
        s.provider = live ? "openrouter" : "offline";
        s.model = live ? model : "";
        if (svc.adapter) {
            svc.adapter->copy_into(s.chat);
            svc.adapter->copy_reasoning(s.reasoning);
            fill_usage(s, svc.adapter); /* P12: per-agent token totals (shared with the capture path) */
            fill_egress(s, svc.adapter); /* P08.2: recent egress decisions for the Network panel */
        }
        fill_memory(s, svc.mbroker);  /* P01: the Memory panel rows (no-op offline) */
        fill_conductor(s, svc.conductor, chat_attach); /* Conductor P5 + A: chat conversation/goals/staged attachments */
        fill_projects(s, svc.projects); /* W3 P3.2: the Projects panel list + active project */
        s.skills = skills_cache; /* W6 P6.3: the per-project Skills panel list (throttled cache, below) */
        s.audio_library = audio_lib_cache; /* Music Player: the library list (throttled scan, below) */
        /* read the mood-enable + spectrum flags LIVE from settings each frame (an operator Save takes effect
         * immediately) — never a stale per-session snapshot. */
        fill_audio_status(s, svc.player, svc.settings && svc.settings->settings.conductor_mood_enabled,
                          !svc.settings || svc.settings->settings.audio_spectrum); /* every frame */
        s.selected_skill = selected_skill; /* W6 P6.3: the skill open in the editor + its body (read on OpenSkill) */
        s.selected_skill_body = selected_skill_body;
        s.open_path = opened_path; /* W4 P4.2: the Viewer's opened file (read once on OpenFile, below) */
        s.open_bytes = opened_bytes;
        s.open_size = opened_size;
        s.open_hash = opened_hash;
        s.open_disk_changed = open_changed; /* W5 P5.2: the open file changed on disk (the editor banner) */
        s.open_diff = open_diff;
        fill_settings(s, svc);        /* WI-2 E1: accent/provider/limits/key_present for the Settings panel */
        if (svc.gate) { /* map the backend gate's view into the snapshot (host_bridge is UI-agnostic) */
            /* B3/B4: drive the gate's delegated-approval flags LIVE from the operator's settings (both default OFF). */
            const bool auto_on = svc.settings && svc.settings->settings.auto_approve_contained;
            const bool allow_all_on = svc.settings && svc.settings->settings.allow_all_approvals;
            svc.gate->set_auto_mode(auto_on);
            svc.gate->set_allow_all(allow_all_on);
            /* B3/B4: age the persistent-toast ring + copy the live ones into the snapshot (the approval-pending
             * toasts are appended below from the live pending list). */
            for (auto it = toast_ring.begin(); it != toast_ring.end();) {
                if (--it->second <= 0) {
                    it = toast_ring.erase(it);
                } else {
                    s.toasts.push_back(it->first);
                    ++it;
                }
            }
            /* B4 + B3-F1: a STICKY armed indicator while a delegated-approval mode is on — rebuilt each frame so it
             * persists while armed and self-clears on disarm (no silent armed state). Allow-all is the loud one. */
            if (allow_all_on)
                s.toasts.push_back({"armed-allow-all",
                                    "ALLOW-ALL ARMED \xE2\x80\x94 every tool request is auto-approved",
                                    hc::ui::Toast::Kind::Warning});
            else if (auto_on)
                s.toasts.push_back({"armed-auto", "auto-approve (sandboxed writes) is ON",
                                    hc::ui::Toast::Kind::Warning});
            /* B3: drain the auto-approved trace -> the SAME content-addressed artifact a human approve records
             * (provenance) + a quiet AutoApproved toast. Auto-approval is visible, never silent. */
            std::vector<hc::host::AutoApprovedView> aa;
            svc.gate->copy_auto_approved(aa);
            for (const auto &av : aa) {
                /* WHITELIST by tool: record the SAME content-addressed artifact a human approve records, but ONLY for
                 * the two sandbox-write tools (fs_write/fs_update). An allow-all `run` or memory_write is NOT a file
                 * write — its result returns via its own reply — so it gets the visibility toast below but no write
                 * artifact. Whitelisting (not blacklisting) means a future auto_approvable tool cannot be silently
                 * mis-recorded as a write here. */
                if (av.tool == "fs_write" || av.tool == "fs_update") {
                    hc::host::AuthResolution r;
                    r.approved = true;
                    r.agent = av.agent;
                    r.tool = av.tool;
                    r.path = av.path;
                    r.content = av.content;
                    record_write_artifact(svc.artifacts, orch_, r);
                }
                hc::ui::Toast t{av.agent, "auto-approved " + av.tool + (av.path.empty() ? "" : " " + av.path),
                                hc::ui::Toast::Kind::AutoApproved};
                toast_ring.emplace_back(t, 180); /* ~3s at 60fps */
            }
            std::vector<hc::host::PendingAuthView> pend;
            svc.gate->snapshot(pend);
            std::unordered_set<std::string> live;
            for (const auto &p : pend) {
                std::string summary = p.summary; /* default: the worker's bounded caption */
                if (p.tool == "memory_write") {
                    /* Bind the verdict to the ACTUAL bytes, like the fs_write diff: show the host-held
                     * content that will be written, not the worker's free-text caption (a hostile worker
                     * can make the caption understate the payload). The operator reviews what lands. */
                    std::string scope, content;
                    if (svc.gate->peek_content(p.id, scope, content))
                        summary = "save to SHARED memory:\n" + content;
                }
                s.pending_auth.push_back({p.id, p.agent, p.tool, std::move(summary), p.age_ms});
                /* B2: a clickable toast per pending request — keyed to the id, so it appears with the prompt and
                 * self-clears when the request is resolved/dismissed (the snapshot is rebuilt each frame). Makes a
                 * patient approval UNMISSABLE without the operator watching the Approvals panel. */
                s.toasts.push_back({p.id, p.agent + " wants " + p.tool + " \xE2\x80\x94 click to review",
                                    hc::ui::Toast::Kind::ApprovalPending});
                live.insert(p.id);
                /* P11: for an fs_write / fs_update prompt, compute the diff ONCE (against the authoritative
                 * current file) and cache it; the bounded content is fetched once, not copied every frame.
                 * fs_update sends the full post-edit content (same wire shape), so the diff is correct for
                 * an in-place edit too — the operator sees exactly what the file becomes. */
                if ((p.tool == "fs_write" || p.tool == "fs_update") &&
                    diff_cache.find(p.id) == diff_cache.end()) {
                    std::string path, content;
                    if (svc.gate->peek_content(p.id, path, content)) {
                        bool                existed = false;
                        std::string current = read_current_file(svc.ws, p.agent, path, svc.shared_workspace, &existed);
                        hc::ui::PendingDiff pd;
                        pd.id = p.id;
                        pd.agent = p.agent;
                        pd.path = path;
                        pd.is_new = !existed; /* "new file" only when the current file was truly absent */
                        bool big = false;
                        pd.hunks = diff_hunks(current, content, &big, &pd.added, &pd.removed);
                        pd.too_large = big;
                        diff_cache[p.id] = std::move(pd);
                    }
                }
            }
            for (auto it = diff_cache.begin(); it != diff_cache.end();) /* drop resolved prompts */
                it = live.count(it->first) ? std::next(it) : diff_cache.erase(it);
            for (const auto &p : pend) /* surface the diffs in prompt order */
                if (diff_cache.count(p.id)) s.pending_diff.push_back(diff_cache[p.id]);
        }
        /* The host listing cadence (the workspace + session/artifact stores change rarely). The render loop
         * runs ~60 fps; settings.poll_hz (clamped 1..60, default 2) sets how often per second we re-list —
         * a LIVE setting (the operator changes it, it takes effect next frame). */
        int poll_hz = svc.settings ? svc.settings->settings.poll_hz : 2;
        if (poll_hz < 1) poll_hz = 1;
        else if (poll_hz > 60) poll_hz = 60;
        const int poll_period = 60 / poll_hz; /* frames between re-lists (>=1) */
        if (tick % poll_period == 0) {
            if (svc.ws) list_workspace(svc.ws, files_cache, &files_truncated_cache);
            if (!svc.skills_root.empty()) fill_skills(skills_cache, svc.skills_root); /* W6 P6.3 (throttled) */
            if (svc.audio) scan_audio_library(audio_lib_cache, svc.audio, audio_probe_cache); /* Music Player (throttled) */
            if (svc.store) list_sessions(svc.store, sessions_cache);
            if (svc.conductor_store) list_conductor_conversations(svc.conductor_store, conductor_conv_cache); /* P3b */
            if (svc.artifacts) list_artifacts(svc.artifacts, artifacts_cache);
            /* W5 P5.2: WATCH the open file. Re-read it (jailed) and compare to the DISPLAYED version. If it
             * changed on disk (e.g. an agent rewrote it), flag it + compute the displayed->disk diff ONCE
             * (skipped for images: a binary text-diff is noise — the banner offers Reload only). The diff is
             * recomputed only when the disk hash itself moves (watched_disk_hash), not every poll. */
            if (!opened_path.empty() && opened_bytes && svc.ws) {
                std::string disk;
                long        dsz = 0; /* the disk size (unused by the watch — only the content + hash matter) */
                uint64_t    dh = 0;
                if (read_open_file(svc.ws, opened_path, disk, dsz, dh)) {
                    /* CONTENT-based detection (not just the FNV hash): FNV-1a is not collision-resistant, and a
                     * hostile author-model controls file bytes — so a same-hash-but-different-content rewrite
                     * must NOT be missed (else a later operator Save could clobber it unseen). The bounded
                     * byte-compare (<= the read cap) is cheap at the ~2 Hz poll; the hash only dedups re-diffs. */
                    const bool differs = disk.size() != opened_bytes->size() || disk != *opened_bytes;
                    if (differs) {
                        if (!open_changed || dh != watched_disk_hash) {
                            hc::ui::PendingDiff pd;
                            pd.path = opened_path; /* pd.id stays empty: this diff is not keyed to a ToolVerdict */
                            const bool img =
                                looks_like_image((const unsigned char *)opened_bytes->data(), opened_bytes->size()) ||
                                looks_like_image((const unsigned char *)disk.data(), disk.size());
                            if (img) {
                                pd.too_large = true; /* image/binary: no text diff — Reload only */
                            } else {
                                bool big = false;
                                pd.hunks = diff_hunks(*opened_bytes, disk, &big, &pd.added, &pd.removed);
                                pd.too_large = big;
                            }
                            open_diff = std::move(pd);
                            watched_disk_hash = dh;
                            open_changed = true;
                        }
                    } else if (open_changed) { /* reverted back to the displayed version */
                        open_changed = false;
                        open_diff = {};
                        watched_disk_hash = 0;
                    }
                }
            }
        }
        tick++;
        if (svc.pty) { /* drain all the shell output available this frame into the tail-bounded buffer */
            char buf[4096];
            long n;
            while ((n = hc_pty_read(svc.pty, buf, sizeof buf)) > 0) {
                term_buf.append(buf, (size_t)n);
                if (term_buf.size() > 256u * 1024) term_buf.erase(0, term_buf.size() - 256u * 1024);
            }
        }
        s.files = files_cache;
        s.files_truncated = files_truncated_cache;
        s.sessions = sessions_cache;
        s.conductor_conversations = conductor_conv_cache; /* P3b: the picker reads this; fill_conductor sets the active id */
        s.artifacts = artifacts_cache;
        uint64_t now = mono_ms(); /* accumulate the activity timeline from this frame's task states (P10) */
        timeline.observe(s.tasks, now);
        timeline.copy_into(s.timeline);
        s.now_ms = now;
        s.viewed_session = viewed_id;
        s.transcript = viewed_transcript;
        s.terminal = term_buf;
        if (notice_ttl > 0) { /* surface the last action's status, then let it fade */
            s.notice = notice;
            notice_ttl--;
        }
        ui.set_snapshot(std::move(s)); /* MOVE: s is a fresh per-frame build, unused after this */
        std::vector<hc::ui::UiCommand> cmds = ui.drain_commands();
        /* A.4: files the operator DRAGGED onto the window this frame — staged via the hardened non-jailed os-file
         * read (operator-initiated; O_NOFOLLOW + regular-file + capped). They ride the SAME staging/send path. */
        for (const std::string &drop : ui.drain_dropped_paths())
            if (!stage_attach_os_file(chat_attach, drop)) {
                notice = "could not attach a dropped file";
                notice_ttl = 180;
            }
        for (hc::ui::UiCommand &c : cmds) { /* mutable: a SetSecret command's key copy is zeroized below */
            if (c.kind == hc::ui::UiCommand::Kind::OpenSession) { /* load the clicked transcript */
                viewed_id = c.a;
                load_transcript(svc.store, viewed_id, viewed_transcript);
                char m[160]; /* feedback so a click on a bad/empty session isn't a silent blank panel */
                if (viewed_transcript.empty())
                    std::snprintf(m, sizeof m, "session '%s' is empty or unreadable", viewed_id.c_str());
                else
                    std::snprintf(m, sizeof m, "loaded session '%s' — %zu message%s", viewed_id.c_str(),
                                  viewed_transcript.size(), viewed_transcript.size() == 1 ? "" : "s");
                notice = m;
                notice_ttl = 180;
            } else if (c.kind == hc::ui::UiCommand::Kind::TermInput) { /* operator typed a line */
                if (svc.pty) {
                    std::string line = c.a + "\n";
                    hc_pty_write(svc.pty, line.data(), line.size());
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::ConductorSay) { /* operator chatted to the conductor */
                if (svc.conductor && !send_with_attachments(chat_attach, svc.conductor, svc.artifacts, c.a)) {
                    notice = "the conductor is busy or the session budget is reached";
                    notice_ttl = 180;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::ConductorStopTurn) { /* E: interrupt the in-flight turn */
                if (svc.conductor) svc.conductor->cancel_turn(); /* keeps the session; settles the partial reply */
            } else if (c.kind == hc::ui::UiCommand::Kind::ConductorAttach) { /* A: queue a workspace file for the next msg */
                if (!stage_attach_file(chat_attach, svc.ws, c.a)) {
                    notice = "could not attach that file";
                    notice_ttl = 180;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::ConductorUnstage) { /* A: drop a queued attachment */
                if (c.n >= 0 && (size_t)c.n < chat_attach.staged.size())
                    chat_attach.staged.erase(chat_attach.staged.begin() + (long)c.n);
            } else if (c.kind == hc::ui::UiCommand::Kind::ConductorNewChat) { /* P2: start a fresh conversation */
                if (svc.switch_conductor && svc.switch_conductor(std::string())) {
                    notice = "started a new conversation";
                    notice_ttl = 120;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::ConductorResumeChat) { /* P2: switch to a stored conversation */
                if (svc.switch_conductor && !c.a.empty() && svc.switch_conductor(c.a)) {
                    notice = "switched conversation";
                    notice_ttl = 120;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::ConductorRenameChat) { /* P3a: rename a conversation */
                if (!c.a.empty() && !c.b.empty()) {
                    if (svc.conductor && svc.conductor->session_id() == c.a) {
                        svc.conductor->set_session_title(c.b); /* the ACTIVE chat -> the conductor thread (single-owner) */
                        notice = "renamed conversation";
                        notice_ttl = 120;
                    } else if (svc.conductor_store) { /* an INACTIVE chat -> host-side load -> set -> free */
                        if (hc_session *se = hc_session_load(svc.conductor_store, c.a.c_str())) {
                            hc_session_set_title(se, c.b.c_str());
                            hc_session_free(se);
                            notice = "renamed conversation";
                            notice_ttl = 120;
                        }
                    }
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::ConductorDeleteChat) { /* P3b: delete a conversation */
                if (!c.a.empty() && svc.conductor_store) {
                    if (svc.conductor && svc.conductor->session_id() == c.a) {
                        notice = "can't delete the active conversation — switch away first"; /* refuse the active one */
                        notice_ttl = 180;
                    } else if (delete_conductor_session(svc.conductor_store, c.a)) {
                        notice = "deleted conversation";
                        notice_ttl = 120;
                    }
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::ForgetMemory) { /* operator prunes a memory */
                if (svc.mbroker && svc.mbroker->forget(c.a)) {
                    notice = "forgot a memory";
                    notice_ttl = 120;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::CreateProject) { /* W3 P3.2: mint a new project (no switch) */
                if (svc.projects && !c.a.empty()) {
                    hc_project np;
                    bool       ok = (hc_projects_create(svc.projects, c.a.c_str(), realtime_ms(), &np) == 0);
                    /* surface the MINTED id — two visually-similar names slug to distinct ids (e.g. my-project-2),
                     * so showing the id avoids a "wait, which project did I just make?" surprise (review F1). */
                    notice = ok ? ("created project '" + std::string(np.display) + "' (id " + np.id + ") — switch to enter it")
                                : "could not create the project";
                    notice_ttl = 180;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::SwitchProject) { /* W3 P3.2: re-exec into the project */
                if (svc.projects && !c.a.empty()) switch_to = c.a; /* main does set_active + execv after teardown */
            } else if (c.kind == hc::ui::UiCommand::Kind::RenameProject) { /* W3 P3.3: rename (display only) */
                if (svc.projects && !c.a.empty()) {
                    notice = hc_projects_rename(svc.projects, c.a.c_str(), c.b.c_str()) == 0 ? "project renamed"
                                                                                            : "rename failed";
                    notice_ttl = 150;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::DeleteProject) { /* W3 P3.3: tombstone (refuses active) */
                if (svc.projects && !c.a.empty()) {
                    /* hc_projects_delete tombstones + refuses the active project; the on-disk subtree lingers
                     * (reclaimed by the jailed remove that lands with Wave 4's sandbox primitives — honest defer). */
                    notice = hc_projects_delete(svc.projects, c.a.c_str()) == 0 ? "project deleted"
                                                                               : "delete failed (active or unknown)";
                    notice_ttl = 150;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::CreateFile) { /* W4 P4.1: jailed create (operator-direct) */
                if (svc.ws && !c.a.empty()) {
                    hc_sandbox_status st;
                    if (c.n == 1) {
                        st = hc_sandbox_mkdirs(svc.ws, c.a.c_str(), 0700);
                    } else { /* an empty regular file, O_EXCL so it never clobbers an existing one */
                        hc_sandbox_fd fd = HC_SANDBOX_FD_INVALID;
                        st = hc_sandbox_open_fd(svc.ws, c.a.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600, &fd);
                        if (st == HC_SANDBOX_OK && fd >= 0) close(fd);
                    }
                    notice = st == HC_SANDBOX_OK ? (std::string("created ") + c.a)
                                                 : (std::string("create failed: ") + hc_sandbox_strerror(st));
                    notice_ttl = 150;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::DeleteFile) { /* W4 P4.1: jailed unlink (confirm-gated UI) */
                if (svc.ws && !c.a.empty()) {
                    hc_sandbox_status st = hc_sandbox_unlink(svc.ws, c.a.c_str(), c.n == 1 ? 1 : 0);
                    notice = st == HC_SANDBOX_OK ? (std::string("deleted ") + c.a)
                                                 : (std::string("delete failed: ") + hc_sandbox_strerror(st));
                    notice_ttl = 150;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::RenameFile) { /* W4 P4.1: jailed rename/move */
                if (svc.ws && !c.a.empty() && !c.b.empty()) {
                    hc_sandbox_status st = hc_sandbox_rename(svc.ws, c.a.c_str(), c.b.c_str());
                    notice = st == HC_SANDBOX_OK ? (std::string("renamed ") + c.a + " -> " + c.b)
                                                 : (std::string("rename failed: ") + hc_sandbox_strerror(st));
                    notice_ttl = 150;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::OpenFile) { /* W4 P4.2: read the file into the Viewer */
                /* Also the [Reload] action of the W5 P5.2 change-watch: re-reading adopts the disk version and
                 * clears the "changed on disk" state (the watch then sees the hashes match). */
                std::string buf;
                if (read_open_file(svc.ws, c.a, buf, opened_size, opened_hash)) {
                    opened_bytes = std::make_shared<const std::string>(std::move(buf));
                    opened_path = c.a;
                    open_changed = false; /* W5 P5.2: the displayed version IS the disk version now */
                    open_diff = {};
                    watched_disk_hash = 0;
                    notice = "opened " + c.a;
                    notice_ttl = 120;
                } else {
                    opened_path.clear();
                    opened_bytes.reset();
                    opened_size = 0;
                    opened_hash = 0;
                    open_changed = false;
                    open_diff = {};
                    watched_disk_hash = 0;
                    notice = "could not open " + c.a;
                    notice_ttl = 150;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::SaveFile) { /* W5 P5.3: operator-direct jailed save */
                if (svc.ws && !c.a.empty()) {
                    hc_sandbox_status st = write_open_file_atomic(svc.ws, c.a, c.b);
                    if (st == HC_SANDBOX_OK) {
                        /* RE-READ through the same jailed path so the displayed version + hash match exactly
                         * what the watch will see — otherwise the just-saved file would false-flag as "changed
                         * on disk". (The editor blocks editing a TRUNCATED file, so no tail is lost on save.) */
                        std::string buf;
                        if (read_open_file(svc.ws, c.a, buf, opened_size, opened_hash)) {
                            opened_bytes = std::make_shared<const std::string>(std::move(buf));
                            opened_path = c.a;
                        }
                        open_changed = false;
                        open_diff = {};
                        watched_disk_hash = 0;
                        notice = "saved " + c.a;
                        notice_ttl = 120;
                    } else {
                        notice = std::string("save failed: ") + hc_sandbox_strerror(st);
                        notice_ttl = 150;
                    }
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::OpenSkill) { /* W6 P6.3: load a skill body for editing */
                std::string body;
                if (!c.a.empty() && hcapp::read_skill(svc.skills_root, c.a, body)) {
                    selected_skill = c.a;
                    selected_skill_body = std::make_shared<const std::string>(std::move(body));
                } else {
                    selected_skill.clear();
                    selected_skill_body.reset();
                    notice = "could not open skill " + c.a;
                    notice_ttl = 150;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::CreateSkill) { /* W6 P6.3: new skill from a template */
                if (!c.a.empty() && !svc.skills_root.empty()) {
                    std::string templ = "---\nname: " + c.a +
                                        "\ndescription: (one line — what this skill does + when to use it)\n---\n\n"
                                        "# " + c.a + "\n\n(write the skill instructions here)\n";
                    bool ok = hcapp::write_skill(svc.skills_root, c.a, templ);
                    notice = ok ? ("created skill " + c.a) : ("create failed (bad name or exists): " + c.a);
                    notice_ttl = 150;
                    if (ok) fill_skills(skills_cache, svc.skills_root); /* refresh the list now, not next poll */
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::SaveSkill) { /* W6 P6.3: operator-direct jailed save */
                if (!c.a.empty()) {
                    bool ok = hcapp::write_skill(svc.skills_root, c.a, c.b);
                    if (ok && selected_skill == c.a) /* keep the editor in sync with what was saved */
                        selected_skill_body = std::make_shared<const std::string>(c.b);
                    notice = ok ? ("saved skill " + c.a) : ("save failed: " + c.a);
                    notice_ttl = 120;
                    if (ok) fill_skills(skills_cache, svc.skills_root); /* a saved frontmatter desc updates the list */
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::DeleteSkill) { /* W6 P6.3: confirm-gated jailed delete */
                if (!c.a.empty()) {
                    bool ok = hcapp::delete_skill(svc.skills_root, c.a);
                    if (ok && selected_skill == c.a) { /* the open skill is gone — clear the editor */
                        selected_skill.clear();
                        selected_skill_body.reset();
                    }
                    notice = ok ? ("deleted skill " + c.a) : ("delete failed (non-empty or unknown): " + c.a);
                    notice_ttl = 150;
                    if (ok) fill_skills(skills_cache, svc.skills_root); /* drop the row now */
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::AudioPlay) { /* Music Player: play a file, or RESUME (empty a) */
                if (c.a.empty()) {
                    if (svc.player) hc_audio_player_play(svc.player); /* resume the loaded (paused) track */
                } else {
                    hc_sandbox *jail = (c.n == 1) ? svc.ws : svc.audio;
                    if (play_audio_track(svc.player, jail, c.a))
                        current_audio_name = (c.n == 0) ? c.a : std::string(); /* library track -> next/prev anchor */
                    else { notice = "could not play " + c.a; notice_ttl = 120; }
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::AudioPause) {
                if (svc.player) hc_audio_player_pause(svc.player);
            } else if (c.kind == hc::ui::UiCommand::Kind::AudioStop) {
                if (svc.player) hc_audio_player_stop(svc.player);
            } else if (c.kind == hc::ui::UiCommand::Kind::AudioSeek) {
                if (svc.player) hc_audio_player_seek_ms(svc.player, c.n < 0 ? 0 : (uint64_t)c.n);
            } else if (c.kind == hc::ui::UiCommand::Kind::AudioVolume) {
                int v = c.n < 0 ? 0 : (c.n > 100 ? 100 : c.n);
                if (svc.player) hc_audio_player_set_volume(svc.player, v / 100.0f);
                if (svc.settings) { /* live in-memory; persist only on the slider RELEASE (b == "save") */
                    svc.settings->settings.audio_volume = v;
                    if (c.b == "save") {
                        settings_validate(svc.settings->settings); /* validate-before-save, like every persist site */
                        settings_save(svc.settings->settings, svc.settings->path.c_str());
                    }
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::AudioNext ||
                       c.kind == hc::ui::UiCommand::Kind::AudioPrev) { /* step over the library (name-sorted) */
                if (svc.player && svc.audio && !audio_lib_cache.empty()) {
                    std::vector<std::string> names;
                    for (const auto &t : audio_lib_cache) names.push_back(t.name);
                    std::sort(names.begin(), names.end());
                    int idx = -1;
                    for (size_t i = 0; i < names.size(); i++)
                        if (names[i] == current_audio_name) { idx = (int)i; break; }
                    int step = (c.kind == hc::ui::UiCommand::Kind::AudioNext) ? 1 : -1;
                    int nxt = (idx < 0) ? 0 : ((idx + step + (int)names.size()) % (int)names.size());
                    if (play_audio_track(svc.player, svc.audio, names[(size_t)nxt])) current_audio_name = names[(size_t)nxt];
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::AudioToggle) { /* flip + persist a bool setting */
                if (svc.settings) {
                    if (c.a == "spectrum") svc.settings->settings.audio_spectrum = (c.n != 0);
                    else if (c.a == "mood") {
                        svc.settings->settings.conductor_mood_enabled = (c.n != 0);
                        /* turning the mood feature OFF also stops any conductor-started track: "off" == silent now */
                        if (c.n == 0 && svc.player) hc_audio_player_stop(svc.player);
                    }
                    settings_validate(svc.settings->settings); /* validate-before-save, like every persist site */
                    settings_save(svc.settings->settings, svc.settings->path.c_str());
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::SetConductorPersona) {
                /* Wave A: set the conductor VOICE slot, persisted immediately (live-owned; preserved across
                 * SaveSettings). The slot is sanitized at prompt assembly (validate_persona); this stores the raw
                 * operator text. Applies on the next conductor (re)build — New Chat / Resume / project switch. */
                if (c.a == "project") {
                    if (svc.project_dir.empty()) {
                        notice = "no persistent project — the per-project persona is unavailable here";
                    } else {
                        write_project_persona(svc.project_dir, c.b);
                        notice = c.b.empty() ? "project persona cleared (uses the global default) — applies to the next chat"
                                             : "project persona saved — applies to the next chat";
                    }
                    notice_ttl = 180;
                } else if (svc.settings) { /* the global default */
                    svc.settings->settings.conductor_persona = c.b;
                    settings_validate(svc.settings->settings); /* validate-before-save, like every persist site */
                    bool ok = settings_save(svc.settings->settings, svc.settings->path.c_str());
                    notice = !ok ? "persona save FAILED"
                                 : (c.b.empty() ? "global persona cleared (canonical voice) — applies to the next chat"
                                                : "global persona saved — applies to the next chat");
                    notice_ttl = 180;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::ToggleTool) {
                /* Flip a tool's enable state, persisted immediately. A System Tool toggle takes effect by being
                 * subtracted from the toolset of workers (re)spawned after (effective_role_tools_csv). A
                 * THIRD-PARTY toggle (Wave E) is LIVE: enabling writes the package's manifest.lock (the operator
                 * approved THESE bytes — the consent modal gated the click) + launches it now; disabling reaps it. */
                if (svc.settings) {
                    if (c.b == "thirdparty") {
                        if (c.n != 0) {
                            /* enable: re-check the host-private root + a valid id (F1), then write the lock
                             * (approve THESE bytes) BEFORE persisting enabled (F4 — a failed lock leaves the
                             * setting OFF, never a lockless "enabled"). The consent modal already gated this click. */
                            bool priv = !svc.tools_root.empty() && tool_id_ok(c.a)
                                        && hcapp::dir_is_host_private(svc.tools_root);
                            bool locked = priv && write_tool_lock(svc.tools_root, c.a);
                            if (!locked) {
                                notice = "tool '" + c.a +
                                         "': could not approve (no host-private install root / bad id / lock write failed) — not enabled";
                            } else {
                                svc.settings->settings.thirdparty_tools[c.a] = true;
                                settings_validate(svc.settings->settings);
                                settings_save(svc.settings->settings, svc.settings->path.c_str());
                                bool up = svc.toolhost && svc.settings->settings.thirdparty_tools_enabled
                                          && svc.toolhost->launch_one(c.a);
                                notice = up ? "tool '" + c.a + "' enabled + launched"
                                            : "tool '" + c.a + "' enabled (launches on next start)";
                            }
                        } else {
                            svc.settings->settings.thirdparty_tools[c.a] = false;
                            settings_validate(svc.settings->settings);
                            settings_save(svc.settings->settings, svc.settings->path.c_str());
                            if (svc.toolhost) svc.toolhost->reap_one(c.a);
                            notice = "tool '" + c.a + "' disabled + stopped";
                        }
                    } else {
                        svc.settings->settings.system_tools[c.a] = (c.n != 0);
                        settings_validate(svc.settings->settings);
                        settings_save(svc.settings->settings, svc.settings->path.c_str());
                        notice = "tool '" + c.a + (c.n ? "' enabled" : "' disabled") + " (applies to workers spawned after)";
                    }
                    notice_ttl = 180;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::DisableAllThirdParty) {
                /* the global kill-switch: n=1 arms it (all third-party off), n=0 clears it. Arming reaps any
                 * RUNNING tools immediately (the "stop everything now" affordance); clearing applies on the next
                 * launch (the live reap is one-way for this process — the persisted setting governs the relaunch). */
                if (svc.settings) {
                    svc.settings->settings.thirdparty_tools_enabled = (c.n == 0);
                    settings_validate(svc.settings->settings);
                    settings_save(svc.settings->settings, svc.settings->path.c_str());
                    if (c.n && svc.toolhost) svc.toolhost->disable_all_live(); /* SIGTERM/SIGKILL + revoke, now */
                    notice = c.n ? "third-party tools DISABLED (kill-switch armed; running tools stopped)"
                                 : "third-party tools re-enabled (applies on the next launch)";
                    notice_ttl = 180;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::SetConductorThirdParty) {
                /* D4c (opt-in): let the conductor use third-party tools. Persisted; applies on the NEXT conductor
                 * build (New Chat / Resume / project switch) — like the persona + the mood toggle. The conductor
                 * is the most-privileged agent, so this stays default-off + every sensitive call is operator-gated. */
                if (svc.settings) {
                    svc.settings->settings.thirdparty_tools_conductor = (c.n != 0);
                    settings_validate(svc.settings->settings);
                    settings_save(svc.settings->settings, svc.settings->path.c_str());
                    notice = c.n ? "conductor third-party tools ENABLED (applies to the next chat; calls are gated)"
                                 : "conductor third-party tools disabled (applies to the next chat)";
                    notice_ttl = 180;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::RemoveTool) {
                /* Wave E: uninstall a third-party package — stop it, delete its dir, clear its setting. Bounded to
                 * <tools_root>/<validated-id>/ (a single-component id; never escapes the install root). */
                if (svc.settings && !svc.tools_root.empty() && tool_id_ok(c.a)) {
                    if (svc.toolhost) svc.toolhost->reap_one(c.a);
                    rm_tree(svc.tools_root + "/" + c.a);
                    svc.settings->settings.thirdparty_tools.erase(c.a);
                    settings_validate(svc.settings->settings);
                    settings_save(svc.settings->settings, svc.settings->path.c_str());
                    notice = "tool '" + c.a + "' removed";
                    notice_ttl = 180;
                } else {
                    notice = "remove unavailable (no install root, or an invalid tool id)";
                    notice_ttl = 150;
                }
            } else if (c.kind == hc::ui::UiCommand::Kind::TestTool) {
                notice = "dry-run arrives in a later update"; /* honest deferral — see EXPANSIONS */
                notice_ttl = 150;
            } else { /* CreateAgenda / ToolVerdict / SetSecret / SaveSettings -> the command dispatcher */
                std::string status = dispatch_ui_command(c, orch_, sup, fleet, svc.gate, svc.artifacts,
                                                         svc.mbroker, pool, svc.settings, svc.roles);
                /* AddWorker/RemoveWorker refresh the bus known-fleet filters via the Fleet's on_change callback
                 * (installed by main, fired inside add/remove) — so the UI path AND the conductor-tool path stay
                 * in step without a per-call-site refresh here. */
                if (!status.empty()) {
                    notice = std::move(status);
                    notice_ttl = 180;
                }
            }
        }
        if (!switch_to.empty()) break; /* W3 P3.2: a project switch was requested -> return it for the re-exec */
        if (ui.render_frames(1) < 1 || ui.wants_quit()) break; /* window closed or File->Quit */
        sleep_ms(16);                                          /* ~60fps cap */
    }
    return switch_to; /* "" on a normal quit; a project id on a switch */
}

void run_headless_loop(Orchestrator &orch_, const HostServices &svc)
{
    std::fprintf(stderr, "host: no display — running headless\n");
    for (int waited = 0; waited < 120000 && !hc::orch::agenda_complete(orch_.snapshot()); waited += 100) {
        if (svc.gate) {
            std::vector<hc::host::PendingAuthView> pend;
            svc.gate->snapshot(pend);
            for (const auto &p : pend) {
                std::fprintf(stderr, "host: auto-denied %s request from %s (headless — no operator)\n",
                             p.tool.c_str(), p.agent.c_str());
                svc.gate->resolve(p.id, false);
            }
        }
        sleep_ms(100);
    }
    orch::Agenda fin = orch_.snapshot();
    std::printf("\nhost: agenda '%s' — %d%% (%s)\n", fin.title.c_str(), orch_.progress(),
                hc::orch::agenda_complete(fin) ? "all done" : "incomplete");
    for (const auto &t : fin.tasks)
        std::printf("\n[%s] %s  (%s, by %s)\n%s\n", t.id.c_str(), t.title.c_str(),
                    hc::orch::task_state_str(t.state), t.assignee.empty() ? "-" : t.assignee.c_str(),
                    t.result.c_str());
    if (svc.adapter) { /* streaming proof: the worker on_text deltas the "ui" adapter collected */
        std::vector<std::string> chat;
        svc.adapter->copy_into(chat);
        std::printf("\nhost: %zu live token deltas streamed to the console adapter\n", chat.size());
    }
    if (svc.store) { /* persistence proof: the per-task sessions the workers wrote */
        std::vector<hc::ui::SessionRow> sess;
        list_sessions(svc.store, sess);
        std::printf("host: %zu sessions persisted to the store\n", sess.size());
    }
}

} // namespace hcapp
