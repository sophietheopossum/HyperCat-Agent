#ifndef HC_HOST_SERVICES_HPP
#define HC_HOST_SERVICES_HPP

/* host_services — the host's per-frame UI adapter, extracted from app/main.cpp so each file has one job:
 * this is the snapshot/command BRIDGE + the two drive loops (build a UiSnapshot from live orchestrator +
 * supervisor + bus-adapter + store state each frame, and dispatch the UiCommands the UI emits back to the
 * backend); main.cpp keeps the bootstrap + lifecycle (broker auth, fleet spawn, service open, teardown).
 *
 * Owns:      nothing — operates on the borrowed Orchestrator/Supervisor/HostServices main owns.
 * Threading: the host/UI thread drives these; the bus adapters keep their own reader threads behind
 *            their APIs. The collectors here (TimelineLog) are single-threaded host state, internal to
 *            the .cpp. See app/host_bridge for the bus-side adapters, Docs/Plan_HyperCat/02. */

#include "hc_roles.hpp"    /* hcapp::RoleTable — the editable worker role templates (Worker Builder, P2.3b) */
#include "hc_settings.hpp" /* hcapp::Settings / EnvOverrides — the persisted settings (WI-2 E1) */
#include "hc_ui.hpp"

#include "hc_artifacts.h" /* opaque C handles used as HostServices pointer members */
#include "hc_audio.h"     /* hc_audio_player — the GLOBAL Music Player engine handle */
#include "hc_memory.h"
#include "hc_pty.h"
#include "hc_sandbox.h"
#include "hc_store.h"

struct hc_llm;      /* forward — open_chat_llm returns one; the real header stays in the .cpp / consumers */
struct hc_http;     /* forward — the chat client's transport (freed via close_chat_llm)                   */
struct hc_secrets;  /* forward — the process-local key store (WI-2 E1); the real header lives in the .cpp */
struct hc_projects; /* forward — the project index (W3 P3.2; the real header lives in the .cpp / main)    */

#include <map>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace hc {
class Orchestrator;
class Supervisor;
namespace host {
class UiAdapter;
class AuthGate;
class MemoryBroker;
class CapabilityAuthority; /* P09: mints + verifies capability tokens (beside AuthGate) */
} // namespace host
namespace conductor {
class Conductor; /* Conductor P5 — the front-door agent the host wires + the loops bridge into the UI */
} // namespace conductor
} // namespace hc

namespace hcapp {

using Pool = std::vector<std::pair<std::string, std::string>>;

class Fleet;    /* fwd (app/fleet) — run_live_loop/dispatch_ui_command borrow the LIVE fleet for the roster + add/remove */
class ToolHost; /* fwd (app/toolhost) — the GLOBAL third-party tool supervisor/router; main owns it, the bridge borrows */

/* WI-2 E1: the host-owned settings state main constructs at startup and the services bridge into the UI.
 * Holds the effective (file + env-merged) settings, which fields an explicit env overrode, the file path,
 * and the process-local key store. The API key is in `secrets` ONLY — never a Settings field, never on
 * disk. main owns the state + opens/closes `secrets`; HostServices borrows a pointer to it. */
struct SettingsState {
    Settings     settings;          /* effective (env-merged + validated) */
    EnvOverrides ov;                /* which fields an explicit env var locked */
    hc_secrets  *secrets = nullptr; /* the COMPANION key store for the keyless settings file: the provider
                                     * API key lives here ONLY (host owns + zeroizes on close; never serialized
                                     * to settings.json, never in a snapshot) */
    std::string  path;              /* <data_dir>/settings.json */
};

/* The host-owned WORKER ROLE TABLE + its file path (P2.3b — the Worker Builder). main constructs it at
 * startup (built-in defaults, then overlaid by roles.json if present) and Fleet BORROWS &table (reading it
 * fresh in role_spawn_args, so an edit applies to the role's next spawn). dispatch_ui_command's EditRole/
 * RemoveRole mutate `table` + persist to `path`. The table holds NO secret — it is the operator's prompt/
 * tool/model templates only; the API key never appears here (it stays in SettingsState::secrets).
 *
 * Threading: `mu` guards `table`. P2.4 lets the CONDUCTOR thread spawn workers (Fleet::add_worker ->
 * role_spawn_args reads the table), while EditRole mutates it on the HOST thread — so both the spawn-time
 * read (Fleet locks &mu, passed to Fleet::create) and the EditRole write take `mu`. Without it a runtime role
 * edit would race a concurrent conductor spawn (a torn read of the role strings). RoleState is never copied
 * (the std::mutex member makes that explicit); main owns it, Fleet + dispatch borrow into it. */
struct RoleState {
    RoleTable   table;
    std::string path; /* <data_dir>/roles.json */
    std::mutex  mu;   /* guards `table` across the host-thread editor + the conductor-thread spawn read */
};

/* The host-side services the render loops bridge into the UI: the bus adapters (token/reasoning stream +
 * the tool-auth gate), the read-only jail + the session/artifact stores the browsers list, and (visible
 * UI only) the operator pty. Any may be null; main owns + opens + closes them. */
struct HostServices {
    hc::host::UiAdapter    *adapter = nullptr;
    hc::host::AuthGate     *gate = nullptr;
    hc::host::CapabilityAuthority *capauth = nullptr; /* P09: the capability-token authority (beside `gate`) */
    hc_sandbox             *ws = nullptr;        /* the workspace jail — operator-direct read AND write (the file
                                                  * browser open/create/delete/rename/save + audio play, all jailed) */
    hc_store               *store = nullptr;     /* the session store (session browser)               */
    hc_artifacts           *artifacts = nullptr; /* content-addressed output store + provenance (P02) */
    hc_memory              *memory = nullptr;    /* the semantic-memory store (P01)                   */
    hc::host::MemoryBroker *mbroker = nullptr;   /* the recall bus service (owns the embed client)    */
    hc::ui::MemoryStatus    mem_status;          /* WHY memory is or is not available — the Memory
                                                  * panel must not render a refusing store as an empty
                                                  * one. Set once, where the store is opened.        */
    hc_pty                 *pty = nullptr;       /* the operator terminal's shell (visible UI only)   */
    SettingsState          *settings = nullptr;  /* the operator settings + key store (WI-2 E1)       */
    RoleState              *roles = nullptr;     /* the editable worker role templates (Worker Builder, P2.3b) */
    hc_projects            *projects = nullptr;  /* the project index — Projects panel lists + switches (W3 P3.2);
                                                  * BORROWED: main owns it + closes it after the loop returns */
    hc::conductor::Conductor *conductor = nullptr; /* the front-door agent (Conductor P5); main owns it */
    /* P2 conductor conversations: the runtime conversation-SWITCH seam. ProjectSession installs it (a lambda over
     * its switch_conductor_conversation) so the dispatch loop can start/resume a conversation WITHOUT reaching into
     * ProjectSession. id == "" => a fresh chat; a session id => resume it. Returns true on success. Null when the
     * conductor runs without persistence (ephemeral). Called ONLY on the host/dispatch thread. */
    std::function<bool(const std::string &)> switch_conductor;
    hc_store *conductor_store = nullptr; /* P3a/P3b: the conductor's DEDICATED conversation store (BORROWED; the
                                          * session owns + closes it). The conversation picker LISTS it; an INACTIVE
                                          * rename/delete operates on it host-side; the ACTIVE one routes through the
                                          * conductor (single-owner of its live hc_session). Null => no persistence. */
    std::string             skills_root;         /* the per-project skills/ dir PATH — the Skills panel CRUD +
                                                  * fill_skills scan it (jailed per-op via skill_store); W6 P6.3 */
    std::string             project_dir;         /* the per-project root PATH (Wave A) — the persona editor reads/
                                                  * writes <project_dir>/conductor_persona here. "" when ephemeral
                                                  * (no persistent project => the per-project persona is offline). */
    bool                    shared_workspace = false; /* W3: the fleet shares one ws dir (the diff-read path follows) */
    hc_audio_player        *player = nullptr;        /* the GLOBAL OpenAL playback engine (Music Player); main owns + frees */
    hc_sandbox             *audio = nullptr;         /* the GLOBAL audio library jail (sibling of projects/); main owns + closes */
    ToolHost               *toolhost = nullptr;      /* the GLOBAL third-party tool supervisor/router (Wave D); BORROWED —
                                                      * main owns + reaps it. Null => no third-party tools (ephemeral, the
                                                      * kill-switch armed, or launch failed). The Tools panel + the
                                                      * DisableAllThirdParty kill-switch reach it through here. */
    std::string             tools_root;              /* <data>/tools — the third-party install root (Wave E). The Tools
                                                      * panel scans it for installed packages; enable writes its
                                                      * manifest.lock + launches; Remove deletes the package dir. "" when
                                                      * ephemeral / non-Linux (third-party tools are off). */
    /* the conductor mood-tool gate is read LIVE from settings->settings.conductor_mood_enabled (not cached here)
     * so an operator Save takes effect immediately — see fill_audio_status + the Phase-D mood tool. */
};

/* Map the host settings state -> the UI-side POD (accent string<->enum, the ov_* env locks, key_present
 * derived from the key store). The UI never sees hcapp::Settings or the key bytes. */
hc::ui::UiSettings to_ui_settings(const SettingsState &);

/* setenv the EFFECTIVE provider/limit/egress values so the existing getenv-based fleet/planner/memory
 * bootstrap transparently uses the settings (env already won via merge_env; this is idempotent for those).
 * data_dir/ephemeral are NOT injected (the settings file lives under data_dir — it can't relocate the
 * bootstrap that already resolved it). Never touches the API key. */
void inject_settings_env(const Settings &);

/* Build the UI render model from the live orchestrator agenda + supervisor liveness (one frame). `roles` (the
 * host role table, may be null) populates the Worker Builder's editable role templates (P2.3b). */
hc::ui::UiSnapshot build_snapshot(hc::Orchestrator &, hc::Supervisor &, const Pool &, const RoleTable *roles);

/* Browser listings — also used by main's --screenshot capture branch. `list_workspace` walks the jail
 * RECURSIVELY (depth + count bounded) so the file browser can show a real tree; `*truncated` (optional) is set
 * when a cap trips so the UI can flag a partial listing. */
void list_workspace(hc_sandbox *, std::vector<hc::ui::FileEntry> &, bool *truncated = nullptr);
void list_sessions(hc_store *, std::vector<hc::ui::SessionRow> &);
/* P3b: the conductor conversations picker list over the DEDICATED conductor store (newest-first). */
void list_conductor_conversations(hc_store *, std::vector<hc::ui::ConductorConversationRow> &);
void list_artifacts(hc_artifacts *, std::vector<hc::ui::ArtifactRow> &);

/* P12: fill per-agent token usage + saturating totals into the snapshot from the adapter — shared by the
 * live loop and the --screenshot capture branch so the two paths never drift. No-op if adapter is null. */
void fill_usage(hc::ui::UiSnapshot &, hc::host::UiAdapter *);

/* P08.2: fill the snapshot's Network panel rows from the adapter's collected egress.event decisions. Shared by
 * the live loop + the capture branch (so they never drift). No-op if the adapter is null. */
void fill_egress(hc::ui::UiSnapshot &, hc::host::UiAdapter *);

/* P01: fill the snapshot's Memory panel rows from the recall broker's store (most-recent-first, bounded),
 * plus the status that explains an EMPTY panel (a refusing store must never read as an empty one).
 * Shared by the live loop + the capture branch. A null broker fills the status and no rows.
 *
 * Takes the two things it uses, not the whole HostServices: a filler that receives the service aggregate
 * can silently start reaching for anything in it, which is how a service struct turns into a God Object.
 * The narrow signature also keeps this callable from a test with no host at all. */
void fill_memory(hc::ui::UiSnapshot &, hc::host::MemoryBroker *, const hc::ui::MemoryStatus &);

/* W3 P3.2: fill the snapshot's Projects panel list (+ the active project's display name) from the project index.
 * No-op if the index is null (ephemeral degraded path). Host thread only (hc_projects is not thread-safe). */
void fill_projects(hc::ui::UiSnapshot &, hc_projects *);
void fill_skills(std::vector<hc::ui::SkillRow> &, const std::string &skills_root); /* W6 P6.3 (clears + fills) */

/* Music Player: the now-playing engine status (cheap — every frame) + the THROTTLED library scan (lazy-probed,
 * cached by name — like files/skills). The capture branch + the live loop share both. */
void fill_audio_status(hc::ui::UiSnapshot &, hc_audio_player *player, bool enabled, bool spectrum_on);
void scan_audio_library(std::vector<hc::ui::AudioTrack> &, hc_sandbox *audio,
                        std::map<std::string, hc::ui::AudioTrack> &cache);
/* Jailed read of `name` from `jail` + load + play (the UI transport + the conductor set_mood tool share it). */
bool play_audio_track(hc_audio_player *player, hc_sandbox *jail, const std::string &name);

/* (fill_conductor is host_services.cpp-internal — it takes the file-local ChatAttachState, and only the live
 * loop calls it; it is intentionally NOT declared here. The screenshot/capture branch does not render chat.) */

/* P6: the memory CONSOLIDATION pass (compaction + distillation) over every stored session — a HOST pass
 * (NOT an agent process), so the operator triggers it (opt-in: HC_CONSOLIDATE). A chat LLM summarizes
 * long sessions beside the raw transcript and distills durable, provenance-tagged facts into the memory
 * store via the broker's human-trusted write. Returns the number of facts written. No-op without
 * store + broker + a chat model + key. The const char* args are BORROWED (valid for the call only). */
int consolidate_sessions(const HostServices &, const char *base_url, const char *model, const char *key);

/* Open a host-side CHAT hc_llm (+ its hc_http) — the build_llm pattern factored to one place (used by the
 * consolidation pass and the P05 planner). Returns the llm + the http via *out_http (free BOTH with
 * close_chat_llm), or nullptr (freeing nothing, refcount balanced) on failure / a missing model+key. */
hc_llm *open_chat_llm(const char *base_url, const char *model, const char *key, hc_http **out_http);
void    close_chat_llm(hc_llm *, hc_http *); /* frees both + balances the global-init refcount */

/* The distinct roles in `pool`, first-seen order — the capability set offered to a decomposer (the planner
 * and the conductor each derive the same list, so it lives here once). */
std::vector<std::string> distinct_capabilities(const Pool &pool);

/* W1.3: true iff `agent` produced a NON-EMPTY regular file at `path` in its workspace jail (under `ws`). The
 * host fills the orchestrator's deliverable verifier with this so a Done result for a file-producing task is
 * confirmed on disk before it settles. "../escape" is refused by the jail. `shared` (W3) selects the jail: the
 * agent's own per-agent dir (false) or the one fleet-shared dir (true) — matching where the worker wrote. */
bool deliverable_present(hc_sandbox *ws, const std::string &agent, const std::string &path, bool shared = false);

/* The two drive loops. live: the visible UI + command dispatch (returns when the window closes);
 * headless: auto-deny tool requests + run the agenda to completion, printing proofs. */
/* P2.3: push the LIVE fleet roster into the bus known-fleet filters (UiAdapter/AuthGate/MemoryBroker), so ONLY
 * a currently-live worker's pubs / tool-auth / memory are honored — a removed or never-spawned id is dropped.
 * Called on every add/remove worker (the filter must track the live fleet, not a static superset). */
void sync_fleet_filters(const HostServices &, const std::vector<std::string> &ids);

/* run_live_loop returns the project id to SWITCH to (W3 P3.2), or "" for a normal quit. main acts on a non-empty
 * return: set it active + re-exec the host into it (hard isolation). */
std::string run_live_loop(hc::ui::UiApp &, hc::Orchestrator &, hc::Supervisor &, Fleet &, bool live,
                          const char *model, const HostServices &);
void run_headless_loop(hc::Orchestrator &, const HostServices &);

} // namespace hcapp

#endif /* HC_HOST_SERVICES_HPP */
