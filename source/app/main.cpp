/* HyperCat host shell — the app entry that wires the modules together (doc 02): a host-private bus
 * broker, the agent supervisor, the orchestrator, the ImGui UI, and the host-side bus adapters in
 * app/host_bridge (UiAdapter — the workers' "tokens" stream -> the snapshot chat; AuthGate — the
 * workers' tool.authorize reqs -> the UI approval queue, and the verdict back). It is the intentional
 * host-WIRING point — it owns the top-level objects + the render loop and bridges UI commands to the
 * backend; each subsystem stays behind its own narrow API (no God Object). The host glue is factored
 * into free functions in the anon namespace — `build_snapshot`/`dispatch_ui_command` (the snapshot/
 * command bridges), `run_live_loop`/`run_headless_loop` (the two drive loops); the worker roster + spawn
 * moved to the `hcapp::Fleet` module (app/fleet) and `make_services` does the adapter/sandbox/store wiring —
 * so main() itself is just the
 * broker-auth bootstrap, those calls, the UI branch, and the strict teardown ordering. The file sits
 * near the size smell-test by design: it is the one place all this host wiring is composed.
 *
 * LIVE when OPENROUTER_API_KEY + HC_MODEL are set (workers run real hc_agent turns, streaming tokens,
 * a sandboxed workspace + a human-gated fs_write tool, and a deep_reason tool); otherwise workers echo
 * (the key-free gates pass). It starts with an EMPTY fleet (the operator adds workers in the Fleet panel)
 * and drives the visible UI as a BLANK SLATE — the operator builds + runs agendas in the Agenda Builder, drives
 * the terminal, approves tool requests, picks the accent, etc. `--screenshot <path> [--demo]` renders the
 * UI to a HIDDEN window then exits (headless verification). With no display it runs headless (printing
 * results + the streamed-delta + persisted-session counts, auto-denying tool requests). */

#include "exe_path.hpp" /* resolve agentd relative to this binary (relocatable bundle) */
#include "hc_artifacts.h"
#include "hc_bus.hpp"
#include "hc_fs.h"   /* read the passed --agenda / HC_MEMORY_SEED files (size-capped) */
#include "hc_json.h" /* parse them */
#include "hc_orch_model.hpp"
#include "hc_orchestrator.hpp"
#include "hc_pty.h"
#include "hc_sandbox.h"
#include "hc_store.h"
#include "hc_supervisor.hpp"
#include "hc_planner.hpp"
#include "hc_conductor.hpp" /* Conductor P5: the front-door agent type (held + stopped here) */
#include "hc_replan.hpp"
#include "hc_ui.hpp"
#include "host_bridge.hpp"
#include "host_conductor.hpp" /* Conductor P5: build_conductor — the conductor's construction + wiring */
#include "hc_roles.hpp"       /* Worker Revamp W2: the role table (per-role prompt overlay + toolset + model) */
#include "hc_fleet.hpp"       /* W2 P2.1: the worker-fleet manager (roster + spawn-arg builders, ex-main()) */
#include "host_services.hpp"

#include "hc_projects.h"      /* Wave 3: the project index (the active project + its subtree) */
#include "hc_secrets.h"       /* WI-2 E1: open/seed/close the process-local API-key store at startup */
#include "host_storage.hpp"
#include "host_time.hpp"       /* hcapp::realtime_ms — the project index timestamps */
#include "hc_toolhost.hpp"     /* Wave D: the GLOBAL third-party tool supervisor/router (a sibling of Supervisor) */
#include "project_session.hpp" /* Wave 3 P3.1: the per-project runtime bring-up + teardown */
#include "ws_util.hpp"

#include <dirent.h> /* Wave D: scan the install root for installed tool ids */

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ftw.h>      /* nftw — recursively remove the temp workspace tree on teardown */
#include <sys/stat.h> /* mkdir — provision the WAL/agendas dir (the fleet's workspace mkdir moved to app/fleet) */
#include <unistd.h>

namespace {

using hc::Broker;
using hc::Orchestrator;
using hc::Supervisor;
namespace orch = hc::orch;
using namespace hcapp; /* HostServices + build_snapshot + the list/loop helpers live in host_services */

void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

/* nftw visitor: remove one entry (remove() unlinks a file / rmdirs a dir). FTW_DEPTH visits children
 * before their parent, so a dir is empty by the time it is visited. Unused params are unnamed to keep
 * -Wunused-parameter quiet. */
int rm_entry(const char *path, const struct stat *, int, struct FTW *)
{
    remove(path);
    return 0;
}

/* Recursively remove `path` (the host-private temp dir: the socket + any per-agent workspace tree).
 * FTW_PHYS does not follow symlinks. Best-effort — teardown, so failures are non-fatal.
 * (Sibling remover: host_services.cpp's `rm_tree` does the Wave E tool-package Remove; same no-follow-symlink
 * invariant, lstat-based instead of nftw. If you change the symlink stance here, change it there too.) */
void rm_rf(const char *path) { nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS); }

/* W2 P2.1: the worker-fleet manager — the spawn-arg builders (agent_args / role_spawn_args), the per-role
 * model resolution (model_override / resolve_role_model), and the roster + provisioning (was provision_fleet)
 * — moved to the new app/fleet module (hc_fleet.hpp). main() now creates a hcapp::Fleet and adds the workers;
 * resolve_role_model is hcapp::resolve_role_model (the planner + conductor model resolution below call it). */

/* Load + run an agenda from a JSON file (for `--screenshot --agenda <file>` and manual testing) — so a
 * test agenda is PASSED as data, never hardcoded here (edit the file, not this source). Schema:
 *   {"id":"..","title":"..","goal":"..","tasks":[
 *      {"id":"t1","capability":"dev","title":"..","description":"..","deps":["t0"]}, ...]}
 * Best-effort: an unreadable / malformed / empty file logs and runs nothing. */
void run_agenda_file(Orchestrator &orch, const std::vector<std::pair<std::string, std::string>> &pool,
                     const char *path, const hc::planner::Decomposer &replan_fn = {},
                     int replan_budget = 0)
{
    size_t flen = 0;
    char  *data = hc_fs_read_file(path, 1u << 20, &flen); /* 1 MiB cap */
    if (!data) {
        std::fprintf(stderr, "host: agenda file '%s' is unreadable\n", path);
        return;
    }
    hc_json *root = hc_json_parse(data, flen);
    free(data);
    if (!root) {
        std::fprintf(stderr, "host: agenda file '%s' is not valid JSON\n", path);
        return;
    }
    orch::Agenda ag;
    ag.id = hc_json_get_str(root, "id", "agenda");
    ag.title = hc_json_get_str(root, "title", "agenda");
    ag.goal = hc_json_get_str(root, "goal", "");
    const hc_json *tasks = hc_json_get(root, "tasks");
    size_t         n = tasks ? hc_json_arr_len(tasks) : 0;
    for (size_t i = 0; i < n; i++) {
        const hc_json *t = hc_json_arr_at(tasks, i);
        if (!t) continue;
        orch::Task task;
        task.id = hc_json_get_str(t, "id", "");
        task.capability = hc_json_get_str(t, "capability", "dev");
        task.title = hc_json_get_str(t, "title", "");
        task.description = hc_json_get_str(t, "description", task.title.c_str());
        task.artifact_path = hc_json_get_str(t, "artifact_path", ""); /* W1.3: the deliverable target */
        const hc_json *deps = hc_json_get(t, "deps");
        size_t         dn = deps ? hc_json_arr_len(deps) : 0;
        for (size_t j = 0; j < dn; j++) {
            const char *d = hc_json_as_str(hc_json_arr_at(deps, j), "");
            if (d[0]) task.deps.push_back(d);
        }
        if (!task.id.empty() && !task.title.empty()) ag.tasks.push_back(std::move(task));
    }
    hc_json_free(root);
    if (ag.tasks.empty() && ag.goal.empty()) {
        std::fprintf(stderr, "host: agenda file '%s' had no tasks and no goal\n", path);
    } else if (replan_fn && replan_budget > 0) {
        /* P05b: run with bounded verify->replan — a Failed agenda is re-planned (from the failure reasons)
         * and re-run until it succeeds, or the budget / no-progress guard escalates. Blocking (the headless
         * path), so the outcome is known here and logged for the operator. */
        hc::planner::ReplanOptions opt;
        opt.replan_budget = replan_budget;
        hc::planner::ReplanOutcome o = hc::planner::run_with_replan(orch, ag, pool, replan_fn, opt);
        std::fprintf(stderr, "host: agenda settled — %s after %d repair round(s): %s\n",
                     o.status == hc::planner::ReplanOutcome::Status::Done ? "DONE" : "ESCALATED", o.rounds,
                     o.detail.c_str());
    } else {
        orch.run_agenda(ag, pool); /* empty tasks + a goal => the planner decomposes it (P05a) */
    }
}


} // namespace

int main(int argc, char **argv)
{
    /* dev affordance: `--screenshot <path> [--agenda <file>]` renders the real app to a HIDDEN window +
     * dumps a PPM, then exits — no window appears on screen (headless UI verification). `--agenda <file>`
     * runs a PASSED agenda (JSON) so a test scenario is data, not hardcoded source. `--ephemeral` (or
     * HC_EPHEMERAL) keeps ALL data under the throwaway temp dir (the old behavior) — for CI / clean-slate
     * testing / screenshots; otherwise the stores persist under ~/.local/share/hypercat. */
    const char *screenshot = nullptr;
    const char *agenda_file = nullptr;
    bool        want_ephemeral = getenv("HC_EPHEMERAL") != nullptr;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--agenda") == 0 && i + 1 < argc) agenda_file = argv[++i];
        else if (std::strcmp(argv[i], "--ephemeral") == 0) want_ephemeral = true;
    }

    /* host-private 0700 socket dir (the supervisor enforces this) */
    char dir[] = "/tmp/hypercat_XXXXXX";
    if (!mkdtemp(dir)) {
        std::perror("mkdtemp");
        return 1;
    }
    std::string sock = std::string(dir) + "/bus.sock";

    /* Resolve where the DURABLE stores live: persistent (HC_DATA_DIR > $XDG_DATA_HOME/hypercat >
     * ~/.local/share/hypercat, single-host locked) by default — so sessions, artifacts, the agent
     * workspace, and the MEMORY store survive a run and accumulate across runs. --ephemeral / a
     * lock-contended fallback puts them back under the throwaway temp dir. The socket stays in `dir`. */
    hcapp::StorageRoots roots = hcapp::open_host_storage(want_ephemeral, dir);
    if (!want_ephemeral && roots.ephemeral)
        std::fprintf(stderr,
                     "host: persistent storage unavailable (another instance is running, the data dir "
                     "isn't host-private 0700, or no HOME) — using EPHEMERAL storage under %s\n",
                     dir);
    else if (!roots.ephemeral)
        std::fprintf(stderr, "host: data dir %s (persistent)\n", roots.data_dir.c_str());

    /* Wave 3 — per-project isolation: (1) one-time migrate a pre-projects install's stores into projects/default/
     * (persistent only, before the index is created); (2) open the project index; (3) resolve the ACTIVE project
     * (the persisted pointer, else auto-create + activate "default"). All per-project storage re-roots under
     * <data_dir>/projects/<id>/. P3.2: the index is kept OPEN (svc.projects) so the Projects panel lists + the
     * SwitchProject command re-execs into another project; it is closed in teardown (after any set_active). */
    if (!roots.ephemeral) hcapp::migrate_legacy_to_default(roots.data_dir);
    std::string  project_dir;
    hc_projects *projects = hc_projects_open(roots.data_dir.c_str());
    if (projects) {
        char id[HC_PROJECT_ID_CAP];
        if (hc_projects_get_active(projects, id, sizeof id) != 0) {
            /* none active: PREFER an existing "default" (e.g. the one a legacy migration created + open adopted —
             * minting a new one would dedup to "default-2" and orphan the migrated data), else mint it. */
            hc_project def;
            if (hc_projects_get(projects, "default", &def) == 0)
                hc_projects_set_active(projects, "default", realtime_ms());
            else if (hc_projects_create(projects, "default", realtime_ms(), &def) == 0)
                hc_projects_set_active(projects, def.id, realtime_ms());
        }
        char dirbuf[1200];
        if (hc_projects_get_active(projects, id, sizeof id) == 0 &&
            hc_projects_dir(projects, id, dirbuf, sizeof dirbuf) == 0) {
            project_dir = dirbuf;
            std::fprintf(stderr, "host: active project '%s' -> %s\n", id, project_dir.c_str());
        }
    }
    if (project_dir.empty()) { /* index unavailable -> degrade to the data dir itself (one implicit project) */
        project_dir = roots.data_dir;
        std::fprintf(stderr, "host: project index unavailable — using the data dir as a single project\n");
    }

    /* WI-2 E1: load the operator settings (under the data dir), let an explicit env WIN over the file,
     * validate, then INJECT the effective provider/limit/egress values into the environment so the existing
     * getenv-based fleet/planner/memory bootstrap below transparently honors the settings (no rewrite of
     * that code). The API key lives ONLY in the process-local key store (seeded from the launch env), NEVER
     * in the settings file. settings_state outlives the render loop; main closes the key store in teardown. */
    hcapp::SettingsState settings_state;
    settings_state.secrets = hc_secrets_open();
    if (settings_state.secrets) {
        hc_secrets_load_env(settings_state.secrets, "OPENROUTER_API_KEY", "OPENROUTER_API_KEY");
        /* ENV WINS. Only when no env key was set do we consult the persistent OS keychain; if it holds one, pull
         * it into the in-memory store AND export it into THIS process env (overwrite=0, so an operator export is
         * never clobbered) so the existing getenv-based host LLM paths + the worker posix_spawn(environ)
         * inheritance keep working UNCHANGED. Runs on the boot thread, before any spawn/render. */
        if (!hc_secrets_has(settings_state.secrets, "OPENROUTER_API_KEY") && hc_secrets_keychain_available()
            && hc_secrets_load_keychain(settings_state.secrets, "OPENROUTER_API_KEY") == HC_SECRETS_OK) {
            char keybuf[8192]; /* generous — any realistic provider key (even a long JWT) fits; the in-memory
                                * store holds the authoritative copy regardless, so a miss only skips the env mirror */
            if (hc_secrets_get(settings_state.secrets, "OPENROUTER_API_KEY", keybuf, sizeof keybuf)
                == HC_SECRETS_OK) {
                setenv("OPENROUTER_API_KEY", keybuf, 0); /* 0: never clobber an operator's own export */
                hc_secrets_zero(keybuf, sizeof keybuf);  /* scrub the transient host copy */
                std::fprintf(stderr, "host: provider key loaded from the OS keychain (no env key was set)\n");
            }
        }
    }
    settings_state.path = roots.data_dir + "/settings.json";
    hcapp::settings_load(settings_state.path.c_str(), settings_state.settings); /* missing file -> defaults */
    settings_state.settings = hcapp::settings_merge_env(settings_state.settings, settings_state.ov);
    hcapp::settings_validate(settings_state.settings);
    /* SECURITY (B4): disarm the session-scoped approval escape hatch so a persisted / hand-edited
     * allow_all_approvals=true cannot silently re-arm the gate. This startup path is the SOLE settings-load boundary —
     * a SwitchProject re-execs through it (see :206) — so this covers every launch AND every project switch; the
     * operator re-arms in-session via the type-to-confirm consent modal (the only arm path). auto_approve_contained
     * deliberately persists; only the unbounded hatch is session-scoped (see settings_clear_session_arming). */
    const bool persisted_allow_all = settings_state.settings.allow_all_approvals;
    hcapp::settings_clear_session_arming(settings_state.settings);
    if (persisted_allow_all) /* visible, never silent: tell the operator their stale armed flag was disarmed */
        std::fprintf(stderr, "host: allow_all_approvals was persisted ON but is DISARMED for this session — "
                             "re-arm via Settings -> Automation (type-to-confirm) to enable it.\n");
    hcapp::inject_settings_env(settings_state.settings);

    Broker *broker = Broker::start(sock.c_str(), -1);
    if (!broker) {
        std::fprintf(stderr, "host: broker failed to start\n");
        return 1;
    }
    /* Authorize the host's own in-process bus ids to THIS process BEFORE anything connects, so a
     * same-uid peer cannot squat "host" (worker check-ins carry tokens) or "orchestrator" (which
     * assigns tasks). The supervisor binds each worker id to its pid as it spawns. */
    broker->authorize_id("host", (long)getpid());
    broker->authorize_id("orchestrator", (long)getpid());
    broker->authorize_id("ui", (long)getpid());           /* the host's UI token-stream adapter id   */
    broker->authorize_id("authgate", (long)getpid());     /* the host's tool-authorization gate id   */
    broker->authorize_id("memorybroker", (long)getpid()); /* the host's semantic-recall service (P01) */
    broker->authorize_id("capabilities", (long)getpid());  /* the host's capability authority (P09)     */
    broker->authorize_id("toolhost", (long)getpid());      /* the host's third-party tool router (Wave D) */
    broker->authorize_id("conductor", (long)getpid());     /* D4c: the conductor's own third-party tool.invoke client */
    /* The host's own ids never check in with a token, so self-confirm them for routing — the
     * orchestrator's task.assign is withheld until "orchestrator" is routing-confirmed. Keeping all
     * the host id-authorizations together here makes the broker's auth surface auditable in one read. */
    broker->confirm_id("host");
    broker->confirm_id("orchestrator");
    broker->confirm_id("ui");
    broker->confirm_id("authgate");
    broker->confirm_id("memorybroker");
    broker->confirm_id("capabilities");
    broker->confirm_id("conductor"); /* D4c: self-confirmed like the other host ids; build_conductor connects it
                                      * only when conductor third-party access is opted in (else it never binds) */
    broker->confirm_id("toolhost"); /* the ToolHost connects as this id (a worker tool.list to an absent toolhost
                                     * gets a prompt "no such endpoint" err -> the worker proxy is a clean no-op). */
    /* Resolve agentd next to THIS host binary so a relocated bundle finds its worker; AGENTD_PATH (the build-tree
     * path) is the dev fallback. /proc/self/exe-based, no env/argv/CWD influence (app/exe_path.hpp). */
    std::string agentd_path = hc::resolve_sibling_exe("agentd", AGENTD_PATH);
    std::fprintf(stderr, "host: worker binary = %s\n", agentd_path.c_str()); /* relocatable-bundle diagnostic */
    Supervisor *sup = Supervisor::create(sock, agentd_path.c_str(), broker);
    /* P14: durable agendas. Persistent mode journals each agenda under a 0700 host-private WAL dir (outside
     * every agent sandbox) so a host crash is recoverable; ephemeral mode passes "" (no journaling — there
     * is nothing to recover across a throwaway run). The dir's 0700 parent (the validated data dir) protects
     * this subdir. */
    if (!sup) {
        std::fprintf(stderr, "host: supervisor failed to start\n");
        broker->stop();
        delete broker;
        return 1;
    }

    /* Wave D: the GLOBAL third-party tool supervisor/router — a sibling of the Supervisor that launches
     * operator-installed tool binaries as CONFINED sibling processes and routes their bus invokes (a worker's
     * tool.invoke -> "toolhost" -> the tool -> back). LINUX-ONLY (confinement = Landlock + seccomp): on
     * non-Linux, third-party tools are fail-closed DISABLED (the "toolhost" id stays unbound, so a worker's
     * tool.list gets a prompt "no such endpoint" err — a clean no-op). Persistent storage only (an ephemeral
     * run has no install root). It launches the per-tool-ENABLED subset under <data>/tools/<id>/ unless the
     * global kill-switch is armed; the manifest.lock supply-chain pin gates each launch (see hc_toolhost). */
    std::unique_ptr<hcapp::ToolHost> toolhost;
#if defined(__linux__)
    if (!roots.ephemeral) {
        const std::string  tools_root = roots.data_dir + "/tools";
        const auto        &tset = settings_state.settings;
        std::vector<std::string> enabled_ids; /* the installed packages the operator has switched ON (missing => OFF) */
        /* Discover the enabled packages ONLY from a host-private 0700 root (mirrors the audio library): a
         * non-private tools dir (a same-uid pre-create / symlink) is refused rather than walked. A missing dir
         * is the normal "nothing installed yet" case — silent; an existing-but-not-private one is a tampering
         * signal we log. The package <id> = the dir name (one path component, traversal-safe, like skill_store). */
        struct stat tstat;
        if (hcapp::dir_is_host_private(tools_root)) {
            if (DIR *d = opendir(tools_root.c_str())) {
                for (struct dirent *e; (e = readdir(d));) {
                    if (e->d_name[0] == '.') continue; /* skip ., .., dotfiles (each name is one path component) */
                    auto it = tset.thirdparty_tools.find(e->d_name);
                    if (it != tset.thirdparty_tools.end() && it->second) enabled_ids.emplace_back(e->d_name);
                }
                closedir(d);
            }
        } else if (stat(tools_root.c_str(), &tstat) == 0) {
            std::fprintf(stderr, "host: tools dir %s is not host-private 0700 — third-party tools disabled\n",
                         tools_root.c_str());
        }
        /* Resolve the managed-runtime launcher (hc_tool_launch) next to THIS host binary, exactly like agentd —
         * /proc/self/exe-based, no env/argv/CWD influence; HC_TOOL_LAUNCH_PATH is the dev-tree fallback. A managed
         * (non-C) tool is jailed+exec'd by it; if it can't be resolved, managed tools fail-closed (native run). */
        std::string launcher_path = hc::resolve_sibling_exe("hc_tool_launch", HC_TOOL_LAUNCH_PATH);
        toolhost = hcapp::ToolHost::start(sock, broker, tools_root, launcher_path,
                                          /*kill_switch_on=*/!tset.thirdparty_tools_enabled, enabled_ids);
        if (!toolhost) {
            std::fprintf(stderr, "host: ToolHost failed to start — third-party tools unavailable this run\n");
            broker->revoke_id("toolhost"); /* no client bound -> let a worker's tool.list fast-fail (no stall) */
        } else {
            std::fprintf(stderr, "host: ToolHost up (%zu enabled tool package(s)%s)\n", enabled_ids.size(),
                         tset.thirdparty_tools_enabled ? "" : "; kill-switch ARMED");
            /* D4c: the conductor is built ONCE at startup and registers proxies only for CONFIRMED tools (the
             * functions() set). Tools confirm asynchronously a few ms after launch, so without this the startup
             * conductor would race the check-in and miss them (only a later New Chat would pick them up). When the
             * operator has opted the conductor in AND tools were launched, briefly wait (bounded) for them to
             * confirm so the startup conductor reliably sees them. Workers don't need this (they spawn on demand,
             * well after check-in). Stragglers in a large fleet are caught by the next conductor (re)build. */
            if (tset.thirdparty_tools_conductor && !enabled_ids.empty())
                for (int i = 0; i < 200 && !toolhost->any_ready(); i++) {
                    struct timespec ts = {0, 10 * 1000 * 1000}; /* 10ms slices, up to ~2s */
                    nanosleep(&ts, nullptr);
                }
        }
    } else {
        broker->revoke_id("toolhost"); /* ephemeral: no install root, so no router — fast-fail worker tool.list */
    }
#else
    broker->revoke_id("toolhost"); /* non-Linux: confinement unavailable -> third-party tools fail-closed disabled */
#endif

    /* P3.1: roles.json stays GLOBAL (the per-project fleet/roles split is P3.2). The host owns role_state +
     * settings_state; the project session BORROWS them. P2.3b: operator-editable, the Fleet reads it fresh at
     * each spawn (an EditRole applies to new workers). */
    hcapp::RoleState role_state;
    role_state.table = roletable_builtin_defaults();
    role_state.path = roots.data_dir + "/roles.json";
    roletable_load(role_state.path.c_str(), role_state.table); /* overlay edits if present; else defaults stand */

    /* Music Player: the GLOBAL audio library + the OpenAL playback engine — ONE per process (a re-exec switch
     * recreates them). The library `<data_dir>/audio/` is a SIBLING of projects/ (workers NEVER see it — it is
     * outside their workspace jail; re-validated host-private against a same-uid pre-create/symlink). The engine
     * is OUTPUT-only + null-device-graceful (no device => the Music Player is simply silent). */
    hc_sandbox      *audio_jail = nullptr;
    hc_audio_player *audio_player = nullptr;
    if (!roots.ephemeral) {
        std::string audio_dir = roots.data_dir + "/audio";
        if ((mkdir(audio_dir.c_str(), 0700) == 0 || errno == EEXIST) && hcapp::dir_is_host_private(audio_dir))
            audio_jail = hc_sandbox_open(audio_dir.c_str(), nullptr);
        else
            std::fprintf(stderr, "host: audio library disabled (the audio dir is not host-private)\n");
    }
    audio_player = hc_audio_player_new(); /* NULL on no backend — the UI + conductor degrade to silent */
    if (!audio_player) std::fprintf(stderr, "host: no audio backend — the Music Player is silent\n");
    else hc_audio_player_set_volume(audio_player, settings_state.settings.audio_volume / 100.0f); /* persisted volume */

    /* P3.1: bring up the PER-PROJECT runtime — the orchestrator + de-seeded fleet + host services + memory +
     * conductor, all rooted under the ACTIVE project's subtree — in ONE object whose dtor reproduces the
     * documented teardown order. main keeps the GLOBAL layer (broker/supervisor/settings/roles/audio) + the UI
     * branching; the WAL dir + orchestrator now live inside the session. */
    /* Pass the ToolHost + its install root INTO open so the session sets svc.toolhost BEFORE it builds the
     * startup conductor (D4c: the conductor's opt-in third-party seam reads svc.toolhost at build time). */
    std::string tools_root_for_svc = toolhost ? (roots.data_dir + "/tools") : std::string();
    hcapp::ProjectSession *session = hcapp::ProjectSession::open(
        project_dir, roots.ephemeral, sup, &settings_state, &role_state, audio_player, audio_jail, sock,
        toolhost.get(), tools_root_for_svc);
    if (!session) {
        if (audio_player) hc_audio_player_free(audio_player);
        if (audio_jail) hc_sandbox_close(audio_jail);
        toolhost.reset(); /* reap tool processes + revoke their ids while the broker is still alive */
        sup->shutdown();
        delete sup;
        broker->stop();
        delete broker;
        return 1;
    }

    /* P3.1: thin aliases onto the session's per-project objects, so the recovery + the UI/headless/screenshot
     * branches below read exactly as before. The session OWNS these; the aliases are valid until `delete session`
     * (the branches all return before teardown). role_state + settings_state stay GLOBAL (main-owned). */
    HostServices                  &svc = session->services(); /* svc.toolhost + svc.tools_root were set inside open() */
    hcapp::FleetEnv                info = session->env();
    hc::Orchestrator              *orch_ = &session->orch();
    hcapp::Fleet                  *fleet_mgr = &session->fleet();
    const hc::planner::Decomposer &plan_fn = session->plan_fn();
    int                            replan_budget = session->replan_budget();
    std::string                    wal_dir = session->wal_dir();
    svc.projects = projects; /* P3.2: the live loop lists + switches projects via the index handle main holds */
    std::string switch_to;   /* P3.2: set by run_live_loop -> set_active + re-exec into it after teardown */

    /* DEV/TEST headless affordance: the fleet is DE-SEEDED (W2) — a fresh project has NO workers, so a headless
     * `--agenda` run has nothing to assign to. HC_STARTER_FLEET=<roles csv> (e.g. "dev,qa,research,ops") spawns
     * those workers up front so live-validation works without the UI/conductor. Default-OFF + loud — a stand-in
     * for the operator/conductor adding workers (mirrors HC_AUTO_APPROVE); never needed interactively. */
    if (const char *sf = getenv("HC_STARTER_FLEET"); sf && *sf) {
        std::fprintf(stderr, "host: *** HC_STARTER_FLEET='%s' — spawning a starter fleet (dev/headless only) ***\n", sf);
        std::string roles = sf;
        size_t      i = 0;
        while (i < roles.size()) {
            size_t      j = roles.find(',', i);
            std::string role = roles.substr(i, j == std::string::npos ? std::string::npos : j - i);
            i = (j == std::string::npos) ? roles.size() : j + 1;
            if (!role.empty()) {
                std::string id = fleet_mgr->add_worker(role);
                std::fprintf(stderr, "host:   + %s [%s]\n", id.empty() ? "(failed)" : id.c_str(), role.c_str());
            }
        }
        fleet_mgr->wait_ready(5000);
    }

    /* P14: recover agendas a previous run left incomplete (a host crash). Consent-gated — HC_RESUME re-runs
     * them over the live pool (Done tasks are NOT re-run; in-flight ones re-dispatch); otherwise we just
     * report what is recoverable and leave it on disk for the operator to decide. Persistent mode only. */
    if (!wal_dir.empty()) {
        std::vector<orch::Agenda> incomplete = Orchestrator::recover_incomplete(wal_dir);
        if (!incomplete.empty() && getenv("HC_RESUME")) {
            std::fprintf(stderr, "host: resuming %zu incomplete agenda(s) from a previous run\n",
                         incomplete.size());
            for (auto &ag : incomplete) {
                if (!orch_->run_agenda(ag, fleet_mgr->pool())) continue; /* the LIVE roster (empty if de-seeded) */
                Orchestrator::Verdict rv;
                do {
                    rv = orch_->wait_until_done(20000); /* the orchestrator guarantees an agenda settles */
                } while (rv == Orchestrator::Verdict::Running);
                std::fprintf(stderr, "host: resumed agenda '%s' -> %s\n", ag.id.c_str(),
                             rv == Orchestrator::Verdict::Done ? "DONE" : "FAILED");
            }
        } else if (!incomplete.empty()) {
            std::fprintf(stderr,
                         "host: %zu incomplete agenda(s) recoverable from a previous run "
                         "(set HC_RESUME to resume them)\n",
                         incomplete.size());
        }
    }

    if (screenshot) {
        /* dev: render the real app to a HIDDEN window + capture, then exit. With --demo, run a sample
         * agenda first (auto-deny any tool requests) so the shot is populated. */
        if (agenda_file) run_agenda_file(*orch_, fleet_mgr->pool(), agenda_file, plan_fn, replan_budget);
        hc::ui::UiApp *cap = hc::ui::UiApp::create("HyperCat", /*visible=*/false);
        if (cap) {
            cap->apply_settings(hcapp::to_ui_settings(settings_state)); /* WI-2 E1: reflect settings in the shot */
            /* Live LLM turns need real round-trip time before the workers can report usage; the offline
             * echo fleet settles almost instantly. Give live runs a generous window so the captured frame
             * reflects completed turns (P12 dashboard validation), without slowing offline screenshots. */
            const int settle_frames = info.live ? 300 : 60;
            for (int f = 0; f < settle_frames; f++) { /* let an agenda settle + auto-deny tool prompts */
                cap->pin_dashboard_tab(); /* re-arm each frame: the capture targets the P12 USAGE section */
                if (svc.gate) {
                    std::vector<hc::host::PendingAuthView> pend;
                    svc.gate->snapshot(pend);
                    for (const auto &p : pend) svc.gate->resolve(p.id, false);
                }
                /* read role_state.table UNLOCKED: the --screenshot capture branch never starts the conductor
                 * thread (the only off-host-thread role-table reader), so this single-threaded read can't race. */
                hc::ui::UiSnapshot s = build_snapshot(*orch_, *sup, fleet_mgr->pool(), &role_state.table);
                s.provider = info.live ? "openrouter" : "offline";
                s.model = info.live ? info.model : "";
                if (svc.ws) list_workspace(svc.ws, s.files);
                if (svc.store) list_sessions(svc.store, s.sessions);
                if (svc.artifacts) list_artifacts(svc.artifacts, s.artifacts);
                fill_usage(s, svc.adapter);  /* P12: token usage from the workers' turn.usage pubs */
                fill_memory(s, svc.mbroker, svc.mem_status); /* P01: the Memory panel rows + why they are absent */
                fill_projects(s, svc.projects); /* W3 P3.2: the Projects panel list */
                fill_skills(s.skills, svc.skills_root); /* W6 P6.3: the Skills panel list (capture path) */
                static std::map<std::string, hc::ui::AudioTrack> audio_cap_cache; /* probe-once across frames */
                fill_audio_status(s, svc.player, settings_state.settings.conductor_mood_enabled,
                                  settings_state.settings.audio_spectrum);
                if (svc.audio) scan_audio_library(s.audio_library, svc.audio, audio_cap_cache); /* Music Player */
                cap->set_snapshot(std::move(s));
                cap->render_frames(1);
                sleep_ms(40);
            }
            if (getenv("HC_CONSOLIDATE")) { /* P6: operator-triggered consolidation after the agenda settled */
                int w = consolidate_sessions(svc, getenv("HC_BASE_URL"), info.model,
                                             getenv("OPENROUTER_API_KEY"));
                std::fprintf(stderr, "host: consolidation distilled %d fact(s) into memory\n", w);
                hc::ui::UiSnapshot s = build_snapshot(*orch_, *sup, fleet_mgr->pool(), &role_state.table); /* refresh so the panel shows them */
                s.provider = info.live ? "openrouter" : "offline";
                s.model = info.live ? info.model : "";
                fill_memory(s, svc.mbroker, svc.mem_status);
                cap->set_snapshot(std::move(s));
                cap->render_frames(1);
            }
            cap->pin_dashboard_tab(); /* keep it selected through screenshot()'s own settle frames */
            cap->screenshot(screenshot, 6);
            delete cap;
            std::fprintf(stderr, "host: screenshot -> %s\n", screenshot);
            if (svc.adapter) { /* dev diagnostic: the real token totals behind the Dashboard (P12 proof) */
                std::vector<hc::host::UiAdapter::UsageView> uv;
                svc.adapter->copy_usage(uv);
                for (const auto &u : uv)
                    std::fprintf(stderr, "host: usage %s in=%ld out=%ld calls=%d\n", u.agent.c_str(),
                                 u.input_tokens, u.output_tokens, u.calls);
                /* totals via the same saturating path as the Dashboard (never a raw += on a token field) */
                hc::ui::UiSnapshot tot;
                fill_usage(tot, svc.adapter);
                std::fprintf(stderr, "host: usage TOTAL in=%ld out=%ld (%zu agents reporting)\n",
                             tot.tokens_in, tot.tokens_out, uv.size());
            }
        }
    } else if (hc::ui::UiApp *ui = hc::ui::UiApp::create("HyperCat", /*visible=*/true)) {
        ui->apply_settings(hcapp::to_ui_settings(settings_state)); /* WI-2 E1: seed accent/mascot + the draft */
        /* the OPERATOR terminal — a shell, spawned ONLY with a visible UI. (NB the gate is "a GL window
         * was created", not "a human is present" — a virtual display like Xvfb would also spawn an idle
         * shell; harmless since stdin is operator-only + it is reaped on teardown, but noted.) It starts
         * in the workspace (if any), gets a SCRUBBED env (no API key reaches it — see hc_pty), and is fed
         * ONLY by the human's keystrokes in the terminal panel (no agent/bus path to its stdin). */
        const char *const sh[] = {"/bin/sh", nullptr};
        svc.pty = hc_pty_spawn(sh, info.ws_root.empty() ? nullptr : info.ws_root.c_str());
        switch_to = run_live_loop(*ui, *orch_, *sup, *fleet_mgr, info.live, info.model, svc);
        delete ui;
    } else {
        run_headless_loop(*orch_, svc);
    }

    /* Teardown (P3.1): `delete session` runs the per-project dtor — the documented T1..T12 order (conductor stop;
     * observer unbind + orchestrator-driver join; conductor + planner/conductor llm frees; pty/sandbox/store/
     * artifacts closes; mbroker stop before the memory close; the bus adapters; the fleet reap last via the still-
     * live supervisor). It MUST run BEFORE the supervisor + broker it borrows. Then the GLOBAL layer: the key
     * store, the supervisor, the broker, the single-host lock, the temp dir. The svc/orch_/fleet_mgr aliases above
     * dangle after this delete — none is used past here. */
    delete session;
    /* the GLOBAL Music Player engine (joins its stream thread + releases the audio device) + the audio jail —
     * freed AFTER the session that borrowed them, BEFORE a re-exec so the next process reopens the device clean. */
    if (audio_player) hc_audio_player_free(audio_player);
    if (audio_jail) hc_sandbox_close(audio_jail);
    if (settings_state.secrets) hc_secrets_close(settings_state.secrets); /* WI-2 E1: zeroizes the API key */
    sup->shutdown();
    delete sup;
    /* Wave D: reap the third-party tool processes (SIGTERM/SIGKILL + revoke their bus ids) AFTER the fleet is
     * gone (no worker is mid-invoke) and the supervisor is down, but BEFORE the broker stops — the dtor revokes
     * ids through the still-live broker. */
    toolhost.reset();
    broker->stop();
    delete broker;

    /* P3.2: a project SWITCH requested in the UI — persist the new active project WHILE the single-host lock is
     * still held, then release everything and RE-EXEC this binary so the chosen project loads in a FRESH address
     * space (hard isolation by construction; the API key rides the inherited env, and the new process re-resolves
     * the now-active project from the index). set_active validates the id is live — only re-exec on success. */
    bool reexec = false;
    if (projects) {
        if (!switch_to.empty()) {
            if (hc_projects_set_active(projects, switch_to.c_str(), realtime_ms()) == 0)
                reexec = true;
            else /* the target was deleted/tombstoned between the panel listing it and the loop returning */
                std::fprintf(stderr, "host: project switch to '%s' failed (no longer a live project)\n",
                             switch_to.c_str());
        }
        hc_projects_close(projects);
    }
    if (roots.lock_fd >= 0) close(roots.lock_fd); /* release the single-host lock BEFORE the re-exec re-acquires it */
    /* ALWAYS remove the mkdtemp socket dir. In ephemeral mode roots.data_dir == dir, so the stores under
     * it are cleaned too; in persistent mode the data dir is elsewhere and survives untouched. */
    rm_rf(dir);
    /* `!screenshot` is belt-and-suspenders (review F3): switch_to is only ever set in the visible-UI branch, never
     * the --screenshot/headless paths, so reexec can't be true here — but guarding locally means a future scripted-
     * switch affordance can't accidentally re-exec a `--screenshot`-launched process into a loop. */
    if (reexec && !screenshot) { /* same argv + inherited env; the new run loads `switch_to` as the active project */
        execv("/proc/self/exe", argv);
        std::perror("host: execv (project switch) failed"); /* reached only if execv itself fails */
        return 1;
    }
    return 0;
}
