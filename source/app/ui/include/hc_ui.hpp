#ifndef HC_UI_HPP
#define HC_UI_HPP

/* hc_ui — the Dear ImGui docking shell + restrained-corporate theme (C++ host module, doc 10).
 *
 * Purpose:   render the workspace: a dockspace with the agent roster, the task/agenda board, and a
 *            log console, themed per doc 10 (near-black flat panels, 1px borders, single accent).
 *            It is a PURE RENDERER of a UiSnapshot — it does not touch the bus or the orchestrator
 *            directly; the host adapts live state (orchestrator agenda, agent pool, log stream)
 *            into a UiSnapshot and hands it in. That keeps the UI decoupled and testable headless.
 * Owns:      the window + GL context + ImGui state (via an internal Backend). One per UI thread.
 * Threading: GL/GLFW are single-threaded — construct and drive UiApp from ONE thread. The snapshot
 *            is copied in (set_snapshot), so the producer may live on another thread.
 * Lifetime:  create() returns null if no display/GL is available (a headless host) — callers skip.
 *            Caller owns the returned UiApp; delete it (the destructor tears down GL + the window).
 * Interactivity: the UI stays a pure renderer of the snapshot, but emits UiCommands OUTWARD (the
 *            inverse of the snapshot) for the host to execute — it never touches the bus/orchestrator
 *            itself. The accent is a runtime setting (set_accent + the settings panel's live picker);
 *            purely-visual state stays inside the UI. The host adapts state in + drains commands out.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hc::ui {

/* The six restrained-corporate accents (doc 10): white default + cyan/amber/emerald/violet/crimson.
 * Drives ONLY the primary slots (check mark, slider, selected-tab overline). Public so the host and
 * the settings panel can change it at runtime via UiApp::set_accent. */
enum class Accent { White, Cyan, Amber, Emerald, Violet, Crimson };

struct AgentRow {
    std::string id;
    std::string role;
    std::string state; /* "ready" | "spawned" | "dead" ... (host-supplied label) */
};

struct TaskRow {
    std::string              id;
    std::string              title;
    std::string              state;       /* "pending" | "assigned" | "running" | "done" | "failed" */
    std::string              assignee;
    std::string              description; /* the unit of work — shown in the task-detail view */
    std::string              result;      /* result payload on done, or error text on failed   */
    std::vector<std::string> deps;        /* prerequisite task ids — the DAG edges (P10)        */
};

/* One persisted session in the session browser (Phase D). The host adapts hc_store's listing into this;
 * clicking a row emits an OpenSession UiCommand keyed by `id`, and the host loads the transcript. */
struct SessionRow {
    std::string id;
    std::string title;
    std::string model;
    std::string updated;
    int         turns = 0;
};

/* One entry in the sandboxed-workspace file browser (Phase D). `path` is relative to the workspace root
 * (e.g. "agent_A/reverse.txt"); the host builds it by listing the per-context hc_sandbox jail, so it can
 * never escape the workspace. Display-only. */
struct FileEntry {
    std::string path;
    long        size = 0; /* bytes, for a regular file; 0 for a directory */
    bool        is_dir = false;
};

/* One content-addressed artifact a task produced (P02). The host records an approved fs_write into the
 * CAS + provenance log and adapts the recent rows into the snapshot; the task-detail panel filters by
 * `task`. `label` is the (model-supplied) write path — untrusted, rendered verbatim. */
struct ArtifactRow {
    std::string id;      /* sha256 content id (shown truncated)         */
    std::string task;    /* the producing task id (the panel filters)   */
    std::string agent;   /* the producing agent                         */
    std::string label;   /* the file path / handle (untrusted)          */
    std::string created; /* ISO-8601 UTC                                */
    long        size = 0;
};

/* One semantic-memory record for the Memory panel (P01): the text + its scope + provenance. The id is
 * the ForgetMemory handle. Text + source are untrusted (model/operator-authored) — rendered via "%s". */
struct MemoryRow {
    std::string id;
    std::string scope;  /* "agent:A" | "shared" | "agenda:<id>" */
    std::string text;
    std::string source; /* provenance: "operator" | "session:..." | "distill:..." */
};

/* Why the Memory panel has nothing to show. Without this the panel falls back to "no memories yet",
 * which is wrong in two of these three states -- and dangerously wrong in the third, where a store that
 * is REFUSING to serve records it actually holds looks identical to one that is simply empty. */
struct MemoryStatus {
    enum class State {
        Online,       /* the recall broker is running; the rows are the store's real contents      */
        NoEmbedModel, /* nothing configured -> no broker at all, so memory cannot be read OR written */
        ModelClash,   /* the store's vectors came from another embedding model; it refuses to serve */
    };
    State       state = State::NoEmbedModel;
    std::string store_model;                 /* embedding model recorded in (or assumed for) the store */
    std::string config_model;                /* what is configured now (Settings > Provider)           */
    bool        store_model_assumed = false; /* store_model was ADOPTED, never witnessed in use         */
    std::string store_path;                  /* the directory to delete to start over                  */
};

/* A pending tool-authorization request awaiting a human verdict (Phase C). A worker's tool blocks on a
 * tool.authorize bus req; the host's AuthGate queues it and surfaces it here; the approvals panel shows
 * `summary` — untrusted, model-influenced text rendered verbatim, NEVER interpreted — with allow/deny,
 * which emits a ToolVerdict UiCommand keyed by `id`. */
struct PendingAuth {
    std::string id;      /* opaque host-assigned request id (the ToolVerdict key)        */
    std::string agent;   /* the requesting agent id (the broker-stamped `from`)          */
    std::string tool;    /* the tool name (e.g. "fs_write")                              */
    std::string summary; /* human-readable, length-bounded description of the action     */
    long        age_ms = 0; /* B1: how long it has been pending (PATIENT — never auto-expires); the panel shows "pending Nm" */
};

/* B2: a transient, non-blocking notification card (a stack of these floats over the UI). The host builds the vector
 * each frame; the UI is a pure renderer. ApprovalPending is clickable (opens + fronts the Approvals panel) and is
 * keyed to its pending-request id so it appears with the prompt and self-clears when the request is resolved/
 * dismissed. AutoApproved/Warning are for B3/B4 (a quiet auto-approve trace; a sticky armed-mode warning). Text is
 * untrusted (model-influenced) -> rendered via "%s". */
struct Toast {
    enum class Kind { Info, ApprovalPending, AutoApproved, Warning };
    std::string id;              /* stable key (the pending-request id for ApprovalPending; else host-minted) */
    std::string text;           /* bounded, untrusted-safe                                                   */
    Kind        kind = Kind::Info;
};

/* Per-agent token-usage roll-up (P12), summed host-side from the workers' `turn.usage` pubs. */
struct UsageStat {
    std::string agent; /* the agent id this roll-up is for */
    long        input_tokens = 0, output_tokens = 0;
    int         calls = 0;
};

/* P08.2: one recorded egress decision (the Network panel) — the broker-stamped agent + the requested peer +
 * the verdict ("allow" / "deny-ip-class" / "deny-host-list"). Display-only; host-collected from egress.event. */
struct EgressRow {
    std::string    agent;
    std::string    host;
    std::string    ip;
    std::string    verdict;
    unsigned short port = 0;
    uint64_t       at_ms = 0;
};

/* One span on the activity timeline (P10): a half-open [start_ms, end_ms) interval on one agent's lane.
 * Host-accumulated from orchestrator/supervisor state transitions. Shaped to be P12-span-compatible — a
 * P12 trace span (an LLM/tool/turn op) populates this same row, so the timeline IS the trace's visual
 * form. `end_ms == 0` means still open (drawn to `now_ms`). `label` is host-composed (a task title may
 * appear in a tooltip — untrusted, rendered via "%s"). */
struct TimelineSpan {
    enum class Kind { Task, Tool, Reason, Wait, Crash };
    std::string lane;       /* the agent id (the swimlane); "" for a system lane */
    std::string label;      /* short label ("reverse a string", "fs_write", ...)  */
    std::string task_id;    /* the task this belongs to (click -> select); "" if none */
    uint64_t    start_ms = 0;
    uint64_t    end_ms = 0; /* 0 == still open (draw to now_ms) */
    Kind        kind = Kind::Task;
};

/* One line of a unified diff (P11): `kind` is ' ' (context), '-' (removed) or '+' (added); `text` is the
 * line content (untrusted model/agent bytes — rendered verbatim, never interpreted). */
struct DiffLine {
    char        kind = ' ';
    std::string text;
};

/* One diff hunk (a contiguous changed region + its surrounding context), with 1-based line anchors for
 * the "@@ -old,+new @@" header. */
struct DiffHunk {
    int                   old_start = 0, new_start = 0;
    std::vector<DiffLine> lines;
};

/* A pending fs_write rendered as a reviewable diff (P11) — keyed by the SAME id as its PendingAuth, so
 * the existing Allow/Deny (ToolVerdict) decides it. `too_large`/binary falls back to the blob summary. */
struct PendingDiff {
    std::string           id;     /* == the PendingAuth id (the ToolVerdict key) */
    std::string           agent;  /* the requesting agent                        */
    std::string           path;   /* the file being written (untrusted)          */
    bool                  is_new = false;    /* create vs modify                  */
    bool                  too_large = false; /* diff suppressed -> blob fallback  */
    std::vector<DiffHunk> hunks;
    int                   added = 0, removed = 0; /* line counts for the header   */
};

/* WI-3: this host process's resource stats + short rolling history for the Dashboard sparklines. Filled by
 * the host's throttled (~2 Hz) sampler; present=false offline-or-before-the-first-sample. */
struct SysStat {
    bool               present = false;
    double             cpu_pct = 0.0;
    uint64_t           rss_bytes = 0, uptime_ms = 0, wall_ms = 0;
    std::vector<float> cpu_history;    /* recent CPU% (oldest..newest) */
    std::vector<float> rss_mb_history; /* recent RSS in MB             */
};

/* The UI-side mirror of the operator settings (WI-2 E1) — a POD the host maps to/from its own
 * hcapp::Settings. It carries ONLY what the Settings panel renders or edits: NEVER the API key bytes
 * (those flow one-way via UiCommand::SetSecret into hc_secrets), and NEVER hcapp::Settings itself (the UI
 * stays decoupled from the host value type). The `ov_*` flags mark a field an explicit environment variable
 * overrides (the panel renders it disabled with an "env" note); `key_present` reflects whether hc_secrets
 * holds the provider key; `export_key_to_env` is the security-flagged opt-in (default-OFF) that re-exposes
 * the key to worker environments. */
/* One available model for the Models panel (the UI mirror of hcapp::ModelEntry — no host types leak here). */
struct UiModelEntry {
    std::string id;
    std::string note;
};

struct UiSettings {
    /* ui — applied LIVE */
    Accent accent = Accent::White;
    int    poll_hz = 2;
    bool   mascot = false;
    /* provider — applied on restart (the panel tags it) */
    std::string model, base_url, embed_model, reasoning_effort;
    /* paths — applied on restart */
    std::string data_dir;
    bool        ephemeral = false;
    /* limits — injected into the worker/orchestrator env at (re)spawn */
    int llm_call_total_ms = 120000, llm_connect_ms = 10000, deep_reason_budget = 4,
        task_deadline_ms = 300000;
    /* security */
    std::vector<std::string> egress_allow;             /* the E2 editor's list (numeric IPs)        */
    std::vector<std::string> exec_allow;               /* the exec allowlist (absolute paths; W4)   */
    bool                     key_present = false;        /* hc_secrets holds the provider key          */
    bool                     keychain_available = false; /* an OS keychain is reachable (persist works) */
    bool                     export_key_to_env = false;  /* SECURITY: re-expose key to worker env (OFF) */
    bool                     notify_approvals = false;   /* desktop notification per approval (OFF)      */
    bool                     notify_available = false;   /* this BUILD can notify (gio present; Linux)   */
    /* automation (B3/B4) — delegated-approval opt-ins; BOTH default OFF, the human gate stays the floor */
    bool                     auto_approve_contained = false; /* auto-approve sandbox-contained writes only */
    bool                     allow_all_approvals = false;    /* auto-approve EVERYTHING (armed via consent modal) */
    /* models (Worker Revamp W2) — the available-models catalog + the per-role assignment, for the Models panel.
     * `model` above stays the global/fallback; both empty => today's single-model behaviour. */
    std::vector<UiModelEntry>                        models;      /* the operator's catalog ({id,note}) */
    std::vector<std::pair<std::string, std::string>> role_models; /* role -> model id (the assignment grid) */
    /* role -> the OpenRouter provider-routing block (inner object, canonical JSON), for the same grid. An
     * absent role means "inherit HC_OPENROUTER_PROVIDER", which unset means free routing. */
    std::vector<std::pair<std::string, std::string>> role_providers;
    /* env-override locks — a set env var WINS over the file; the field renders disabled with a note */
    bool ov_model = false, ov_base_url = false, ov_embed_model = false, ov_reasoning_effort = false,
         ov_data_dir = false,
         ov_ephemeral = false, ov_llm_call_total_ms = false, ov_llm_connect_ms = false,
         ov_deep_reason_budget = false, ov_task_deadline_ms = false, ov_egress_allow = false;
    /* conductor personality (Wave A) — the operator-editable VOICE slot. The persona is LIVE-OWNED: the
     * Settings panel edits the slot and emits SetConductorPersona (it does NOT ride in SaveSettings, which
     * preserves it). `conductor_persona` is the GLOBAL default; `conductor_persona_project` the per-project
     * override (editable only when conductor_persona_per_project — a persistent project). The spine_* /
     * default strings are DISPLAY-ONLY (the locked-vs-editable preview); presets are {label,slot-text} the
     * picker applies. Identity and the conduct floor are NOT editable here — the host re-asserts them around
     * the slot, so this can only restyle her voice. */
    std::string conductor_persona;         /* global default slot ("" => the canonical HyperCat voice) */
    std::string conductor_persona_project; /* per-project override slot ("" => fall back to the global)  */
    bool        conductor_persona_per_project = false; /* a persistent project => per-project editing offered */
    std::string conductor_spine_identity;  /* the LOCKED identity preamble (preview; display-only)        */
    std::string conductor_persona_default; /* the canonical default voice (preview / "reset" reference)   */
    std::string conductor_spine_floor;     /* the LOCKED conduct + operational floor (preview)            */
    std::vector<std::pair<std::string, std::string>> persona_presets; /* {label, slot-text} for the picker */
};

/* The conductor's chat surface mirror (Conductor P5). The UI stays decoupled from hc_conductor's types —
 * the host maps a ConductorView into these PODs, exactly as it maps orchestrator/host_bridge state in. */
/* An operator-attached IMAGE bound to a conductor message for INLINE display (A). The conductor is text-only and
 * never sees these (vision is parked) — they render for the operator. `bytes` are the ENCODED image (PNG/JPEG/QOI),
 * SHARED read-only; `hash` is the content key for the chat texture cache; `qoi` selects the decoder. */
struct ConductorAttachment {
    std::string                        name;        /* defanged basename (the caption)             */
    std::shared_ptr<const std::string> bytes;       /* encoded image bytes, shared                 */
    uint64_t                           hash = 0;     /* content fingerprint -> chat texcache key    */
    bool                               qoi = false;  /* true = QOI decoder; false = PNG/JPEG (Auto) */
};
struct ConductorMsg {
    std::string                      role;        /* "user" | "assistant" */
    std::string                      text;        /* the turn content (assistant markdown-rendered; both untrusted -> "%s") */
    uint64_t                         at_ms = 0;   /* wall-clock ms when the turn settled (0 = unknown/resumed); HH:MM (D) */
    std::vector<ConductorAttachment> attachments{}; /* A: operator-attached images shown inline (usually empty; DMI so
                                                     * the existing {role,text[,at_ms]} brace-inits don't trip -Wmissing-field) */
};
/* A chip in the staged-attachment tray (A): one file queued for the NEXT conductor message. */
struct ConductorStagedChip {
    std::string name;             /* defanged basename */
    bool        is_image = false; /* hint: image (shown inline once sent) vs a readable file */
};
struct ConductorGoalRow {
    std::string id;
    std::string title;
    std::string status;      /* "active" | "paused" | "done" | "failed" | "abandoned" | "discussing" */
    int         agendas = 0; /* count of attached agendas */
};
/* One row in the conductor's conversations picker (P3b). The host fills these from the dedicated conductor session
 * store (hc_store_list); the chat panel renders the list, highlights the active one, and emits New/Resume/Rename/
 * Delete. title is operator-typed (untrusted -> "%s"); updated is an ISO-8601 timestamp string. */
struct ConductorConversationRow {
    std::string id;
    std::string title;
    std::string updated;
    int         turns = 0;
};

/* The UI mirror of one worker ROLE template (hcapp::RoleDef) for the Worker Builder's role editor (P2.3b).
 * `tools` holds the agentd tool-ids that are ENABLED; an EMPTY list means "all tools" (the role-table
 * convention, so an unconfigured role keeps full capability). `model` "" = inherit the global/role-model.
 * The host maps this <-> RoleDef: unknown tool names are dropped (never granted) and the overlay is bounded
 * on save. The UI stays decoupled from hcapp::RoleTable — only this POD crosses the seam. */
struct UiRoleRow {
    std::string              role;
    std::string              prompt_overlay;
    std::string              model;
    std::vector<std::string> tools;
};

/* One project in the Projects panel (Wave 3 P3.2). `id` is the traversal-safe project id; `display` the human
 * name; `active` marks the currently-loaded project. Switching to a non-active project re-execs the host. */
struct ProjectRow {
    std::string id;
    std::string display;
    bool        active = false;
};

/* One per-project skill in the Skills panel (W6 P6.3). `name` is the traversal-safe directory name (the
 * load_skill key + the edit/delete key); `description` is its SKILL.md frontmatter description. */
struct SkillRow {
    std::string name;
    std::string description;
};

/* One tool surfaced in the Tools panel (Custom Tooling program). System tools are the built-in worker tools
 * (always installed; toggle enable/disable). ThirdParty tools are operator-installed user binaries (toggle +
 * remove; they carry a declared manifest + a disclaimer). Available = not-yet-installed (a future Browse). All
 * host-supplied strings (name/description/spec_json/manifest) are author/model-influenced -> rendered via
 * "%s"/TextUnformatted, NEVER interpreted. The host fills these; the panel is a pure renderer. (Wave C ships the
 * System view; the third-party producer + manifest/dry-run detail land in Waves D/E.) */
struct ToolManifest {                         /* a third-party tool's DECLARED permissions (display-only) */
    std::vector<std::string> fs_scopes;       /* workspace paths/globs it may touch                       */
    std::vector<std::string> egress_hosts;    /* network peers (host[:port]) it may reach                 */
    std::vector<std::string> exec_paths;      /* executables it may run                                   */
    std::string              resource_limits; /* host-formatted summary ("cpu 2s, mem 128MiB, 1 proc")    */
    std::string              source;          /* provenance ("bundled" | "path:...")                      */
    std::string              version;
};
struct ToolRow {
    enum class Kind { System, ThirdParty, Available };
    std::string  name;               /* the function name the model calls (the toggle/remove/test key) */
    Kind         kind = Kind::System;
    bool         enabled = false;    /* currently active for the fleet                                 */
    bool         disclaimed = false; /* the operator already accepted this third-party tool's risk     */
    std::string  description;        /* one-line summary (untrusted -> "%s")                           */
    std::string  spec_json;          /* the tool's params spec, pretty-printed by the host ("" = none)  */
    ToolManifest manifest;           /* third-party only (System leaves it empty)                      */
    std::string  last_result;        /* the most recent TestTool dry-run result (Wave E)               */
    std::string  last_error;         /* "" on success; the failure reason otherwise (Wave E)           */
};

/* One track in the Music Player's global audio library. `name` is the on-disk file name (the play key, relative
 * to the audio jail); `title`/`artist` are the file's metadata (control-byte-stripped at the source, rendered
 * via "%s" — never a format string). Display-only. (Music Player feature.) */
struct AudioTrack {
    std::string name;
    std::string title;
    std::string artist;
    long        duration_ms = 0;
    std::string fmt; /* "WAV"/"MP3"/"FLAC"/"OGG" */
};

struct UiSnapshot {
    std::string              agenda_title;
    int                      agenda_progress = 0; /* 0..100 */
    std::vector<AgentRow>    agents;
    std::vector<TaskRow>     tasks;
    std::vector<std::string> logs;
    std::vector<std::string> chat;     /* live token/console stream (worker on_text deltas; lossy)     */
    std::string              provider; /* settings display: the live LLM provider ("offline" if none) */
    std::string              model;    /* settings display: the configured model                      */
    std::vector<std::string> roles;    /* capabilities the live fleet offers — the agenda-builder cap combo */
    std::vector<UiRoleRow>   role_defs; /* the editable role TEMPLATES (the Worker Builder editor seeds from these; P2.3b) */
    std::vector<std::string> tool_catalog; /* every worker tool-id a role may grant — the editor's checkbox set (P2.3b) */
    std::vector<ProjectRow>  projects;     /* the Projects panel list (most-recently-touched first; W3 P3.2) */
    std::string              active_project; /* the active project's display name (panel header + window title) */
    std::vector<SkillRow>    skills;        /* the per-project Skills panel list (W6 P6.3) */
    std::string              selected_skill; /* the skill name open in the Skills editor ("" = none) (W6 P6.3) */
    std::shared_ptr<const std::string> selected_skill_body; /* its SKILL.md content (read once on OpenSkill; SHARED) */
    std::vector<ToolRow>     tools;                   /* the Tools panel rows (System + third-party + available) */
    bool                     third_party_tools_disabled = false; /* the global kill-switch state (host-owned)     */
    bool                     third_party_conductor = false; /* D4c: the conductor may use third-party tools (opt-in) */
    std::vector<PendingAuth> pending_auth; /* tool-auth requests awaiting a human allow/deny verdict   */
    std::vector<Toast>       toasts;       /* B2: transient notification cards (approval-pending, etc.) */
    std::string              reasoning;    /* the latest deep_reason 5-stage chain (untrusted text)   */
    std::vector<FileEntry>   files;        /* the sandboxed workspace's contents (file browser; full ws-rel paths) */
    bool                     files_truncated = false; /* the recursive listing hit its depth/count cap — the
                                                       * browser shows an honest "listing truncated" note */
    /* Music Player (audio feature) — the now-playing engine status + the global library (throttled scan). */
    std::vector<AudioTrack>  audio_library;          /* the <data_dir>/audio/ library (double-click to play) */
    bool                     audio_present = false;  /* the OpenAL engine exists (a device opened) — else silent */
    bool                     audio_enabled = false;  /* the conductor "set the mood" tool is enabled (settings) */
    bool                     audio_spectrum_on = true; /* show the spectrum analyser (settings)                 */
    std::string              audio_title;            /* now-playing title (or the file name when untagged)      */
    std::string              audio_artist;           /* now-playing artist (may be empty)                       */
    int                      audio_state = 0;        /* 0 stopped / 1 playing / 2 paused                        */
    long                     audio_pos_ms = 0;       /* audible playback position                               */
    long                     audio_dur_ms = 0;       /* track length (0 = unknown)                              */
    int                      audio_volume = 70;      /* 0..100                                                  */
    std::vector<float>       audio_spectrum;         /* 0..1 magnitude bins (the visualiser)                    */
    std::vector<SessionRow>  sessions;     /* persisted task transcripts (session browser)            */
    std::string              viewed_session; /* the session id whose transcript is loaded, if any     */
    std::vector<std::string> transcript;   /* the viewed session's messages ("role: content"; lossy) */
    std::string              terminal;     /* the operator terminal's output buffer (raw, bounded)    */
    /* W4 P4.2: the file the operator OPENED in the browser (host reads it once on OpenFile; the Viewer panel
     * classifies via ui_docview + renders). */
    std::string              open_path;    /* the opened file's workspace path ("" = nothing open)     */
    /* The opened file's raw content (bounded read; the Viewer classifies + decodes it). A SHARED, read-once
     * immutable buffer so the per-frame snapshot rebuild carries only a refcount bump, not a deep copy of up
     * to a few MiB (an image): the bytes never change after OpenFile reads them. Null when nothing is open. */
    std::shared_ptr<const std::string> open_bytes;
    long                     open_size = 0; /* the file's TRUE byte size (may exceed open_bytes if capped) */
    uint64_t                 open_hash = 0; /* content fingerprint of open_bytes (host-computed once on open):
                                             * the Viewer's texture-cache key, and the W5 live-diff change signal */
    bool                     open_disk_changed = false; /* W5 P5.2: the open file changed on disk since it was
                                                         * displayed (an agent rewrote it) — the editor banners it */
    PendingDiff              open_diff;     /* W5 P5.2: the displayed->disk diff (reuses the P11 diff renderer;
                                             * too_large for an image — Reload only) */
    std::string               notice;      /* transient host status (e.g. "agenda started") for the UI */
    std::vector<ArtifactRow>  artifacts;   /* recent content-addressed outputs (task-detail filters)  */
    std::vector<TimelineSpan> timeline;    /* host-accumulated activity spans (the swimlane view, P10) */
    uint64_t                  now_ms = 0;  /* the host's monotonic "now", so open spans draw to the edge */
    std::vector<PendingDiff>  pending_diff; /* fs_write diffs awaiting a verdict (P11; keyed like pending_auth) */
    long                      tokens_in = 0, tokens_out = 0; /* agenda token totals (P12)        */
    double                    cost_usd = -1;                 /* total cost; -1 -> "—" (no prices) */
    std::vector<UsageStat>    usage_by_agent;                /* the Dashboard usage breakdown     */
    std::vector<EgressRow>    egress;                        /* P08.2: recent egress decisions (Network panel) */
    std::vector<MemoryRow>    memory;                        /* the fleet's semantic memory (P01) */
    MemoryStatus              memory_status;                 /* why `memory` is empty, when it is */
    SysStat                   sysstat;                       /* host process CPU/RSS/uptime (WI-3) */
    UiSettings                settings;                      /* the operator settings panel state (WI-2) */
    /* The conductor chat surface (Conductor P5) — the host fills these from Conductor::snapshot() each frame. */
    bool                          conductor_online = false;  /* the conductor is wired + live (else "offline") */
    std::vector<ConductorMsg>     conductor_chat;            /* the settled conversation (bounded tail)        */
    std::string                   conductor_streaming;       /* the in-progress assistant text ("" when idle)  */
    bool                          conductor_busy = false;    /* a turn is running                              */
    std::string                   conductor_active_tool;     /* the most recent tool call this turn (S2 cue)   */
    std::string                   conductor_notice;          /* transient (budget reached / resume summary)    */
    std::vector<ConductorGoalRow> conductor_goals;           /* the conductor's goals                          */
    /* P3b conversations picker: the dedicated-store conversation list (throttled fill) + the active id (per-frame). */
    std::vector<ConductorConversationRow> conductor_conversations; /* all stored conversations (newest-first)     */
    std::string                           conductor_current_session_id; /* the active conversation (highlight)    */
    std::vector<ConductorStagedChip>      conductor_staged;             /* A: files queued for the next message   */
};

/* One user-defined task in an agenda the operator builds in the UI (the inverse of the read-only
 * TaskRow). The host turns these into orchestrator tasks. `deps` holds the ids of prerequisite tasks. */
struct TaskSpec {
    std::string              id;          /* UI-assigned (t1, t2, ...) */
    std::string              title;
    std::string              capability;  /* "dev" | "qa" | "research" | ... — routes to a matching agent */
    std::string              description; /* the unit of work (becomes the model prompt when live)        */
    std::vector<std::string> deps;        /* ids of tasks that must finish first                          */
    std::string              artifact_path; /* the file the worker must produce ("" = no file deliverable; W1.3) */
    bool                     verify = false; /* require an independent sibling self-check before accept (P04) */
    int                      verifiers = 1;  /* siblings to ask (the budget) when verify is on               */
    int                      quorum = 1;     /* upholds needed to accept                                     */
};

/* A command the UI emits OUTWARD for the host to execute — the inverse of the snapshot. The UI never
 * touches the bus/orchestrator; it accumulates these from widget interactions and the host drains them
 * each frame (UiApp::drain_commands) and dispatches (e.g. CreateAgenda -> orchestrator->run_agenda).
 * Purely-visual state (the accent, open modals) stays inside the UI and is NOT a command. */
struct UiCommand {
    enum class Kind {
        CreateAgenda, /* a: title, b: goal, tasks: the user's tasks — submit a new agenda */
        ToolVerdict,  /* a: request id, n: 1=allow / 0=deny — a tool-auth verdict          */
        ToolDismiss,  /* a: request id — DEFER it (B1): clears the prompt without a verdict, NOT a denial   */
        ToolGrantScoped, /* a: request id, n: budget — approve an fs_write AND mint a SCOPED capability for N
                          * prompt-free writes under the file's directory (P09.3; host derives the prefix)      */
        OpenSession,  /* a: session id — load a past session's transcript                 */
        TermInput,    /* a: a line typed in the terminal — sent to the shell              */
        ForgetMemory, /* a: memory id — operator prunes a record from the store (P01)     */
        SetSecret,    /* a: the provider API key (TRANSIENT — host stores in hc_secrets + zeroizes; never disk) */
        ForgetSecret, /* clear the persisted provider key from the OS keychain + the in-memory store */
        SaveSettings, /* settings: the edited UiSettings — host maps -> hcapp::Settings, validates, persists   */
        EditAllowlist, /* a: a numeric IP, b: "add"|"remove" — the egress allowlist (E2; host re-validates +
                        * persists immediately). Add is confirm-gated in the UI; remove narrows (no confirm). */
        EditExecAllowlist, /* a: an absolute binary path, b: "add"|"remove" — the exec allowlist (W4; host
                            * re-validates [absolute + exists] + persists immediately; mirrors EditAllowlist). */
        ConductorSay,  /* a: the line the operator typed in the conductor chat panel -> Conductor::say(a) */
        ConductorStopTurn, /* interrupt the in-flight conductor turn -> Conductor::cancel_turn() (keeps the session) */
        ConductorAttach,   /* a: ws-relative path, n: 0 = jailed workspace file — queue it for the next chat message (A) */
        ConductorUnstage,  /* n: index into conductor_staged — drop a queued attachment (A) */
        ConductorNewChat,    /* start a FRESH conductor conversation (-> svc.switch_conductor("")) (P2)            */
        ConductorResumeChat, /* a: session id — switch the conductor to that stored conversation (P2)              */
        ConductorRenameChat, /* a: session id, b: new title — rename a conversation (P3a; active via the conductor) */
        ConductorDeleteChat, /* a: session id — delete a conversation (P3b; the ACTIVE one is refused, confirm-gated) */
        AssignRoleModel, /* a: role, b: model id ("" clears -> the role inherits the global model). The host
                          * updates role_models live + persists immediately (W2; mirrors EditAllowlist). */
        AssignRoleProvider, /* a: role, b: the OpenRouter routing block as an INNER JSON object ("" clears ->
                             * the role inherits HC_OPENROUTER_PROVIDER). Live-persisted like AssignRoleModel;
                             * the host REJECTS a non-object with a notice rather than dropping it silently. */
        AddWorker,       /* a: role — spawn a new worker of that role at the next free id (W2 P2.3)          */
        RemoveWorker,    /* a: worker id — reap it + revoke its bus routing, drop it from the fleet (W2 P2.3) */
        EditRole,        /* role_edit: the edited role TEMPLATE — host UPSERTS into the role table, validates +
                          * persists to roles.json; applies to the role's NEXT spawn (W2 P2.3b)              */
        RemoveRole,      /* a: role name — drop a role template from the table + persist (W2 P2.3b)          */
        SwitchProject,   /* a: project id — set it active + RE-EXEC the host into it (W3 P3.2; hard isolation) */
        CreateProject,   /* a: display name — the host mints the id + creates the subtree (appears in the list) */
        RenameProject,   /* a: project id, b: new display name — rename (the id is immutable) (W3 P3.3)         */
        DeleteProject,   /* a: project id — tombstone it (UI-only, confirm-gated; refuses the active one) (P3.3) */
        CreateFile,      /* a: workspace-relative path, n: 1=dir/0=empty file — jailed create (W4 P4.1)        */
        DeleteFile,      /* a: workspace-relative path, n: 1=dir — jailed unlink, confirm-gated (W4 P4.1)       */
        RenameFile,      /* a: old ws-relative path, b: new ws-relative path — jailed rename (W4 P4.1)          */
        OpenFile,        /* a: workspace-relative path — host reads + classifies it into the Viewer (W4 P4.2)   */
        SaveFile,        /* a: ws-relative path, b: the full new content — operator-direct jailed ATOMIC write
                          * (temp+fsync+rename); the host re-syncs the displayed version (W5 P5.3)              */
        OpenSkill,        /* a: skill name — host reads skills/<name>/SKILL.md into the Skills editor (W6 P6.3) */
        CreateSkill,      /* a: skill name — host mkdirs skills/<name>/ + writes a template SKILL.md (W6 P6.3)  */
        SaveSkill,        /* a: skill name, b: the full SKILL.md content — operator-direct jailed save (W6 P6.3) */
        DeleteSkill,      /* a: skill name — jailed remove of skills/<name>/, confirm-gated (W6 P6.3)           */
        /* Music Player (audio feature) — the host routes these to the global hc_audio_player + the audio jail. */
        AudioPlay,        /* a: file name ("" = resume the paused track), n: source (0 = audio library / 1 = workspace) */
        AudioPause,       /* pause playback                                                                     */
        AudioStop,        /* stop + rewind                                                                      */
        AudioSeek,        /* n: position in ms                                                                  */
        AudioVolume,      /* n: 0..100; b == "save" also persists settings.audio_volume (else live-only)         */
        AudioNext,        /* play the next track in the library (relative to now-playing)                        */
        AudioPrev,        /* play the previous library track                                                     */
        AudioToggle,      /* a: "spectrum" | "mood", n: 0/1 — flip the bool setting + persist                    */
        /* conductor personality (Wave A): set the VOICE slot. a: scope ("global" | "project"), b: the slot text
         * ("" => clear -> the canonical voice / the global default). LIVE-OWNED: persisted immediately + preserved
         * across SaveSettings. The host sanitizes it at prompt assembly; it can never alter identity or the floor.
         * Applies on the next conductor (re)build (New Chat / Resume / project switch). */
        SetConductorPersona,
        /* Tools panel (Custom Tooling). ToggleTool: a=tool name, b="system"|"thirdparty", n=1 enable / 0 disable.
         * DisableAllThirdParty: n=1 arm the kill-switch (all third-party off) / 0 clear. RemoveTool (a=tool id)
         * + TestTool (a=tool name, b=sample args json -> result back via ToolRow.last_result) are wired in Waves
         * D/E; in Wave C they are accepted no-ops (no third-party tool exists yet). */
        ToggleTool,
        RemoveTool,
        TestTool,
        DisableAllThirdParty,
        SetConductorThirdParty, /* D4c: n=1 let the conductor use third-party tools / 0 off (opt-in, next chat) */
    } kind;
    std::string           a;
    std::string           b;
    int                   n = 0;
    std::vector<TaskSpec> tasks;      /* CreateAgenda only: the user-built task list */
    UiSettings            settings{}; /* SaveSettings only: the edited settings (DMI so the existing
                                       * positional brace-inits that omit it don't trip -Wmissing-field) */
    UiRoleRow             role_edit{}; /* EditRole only: the edited role template (DMI — last member, so the
                                        * existing positional brace-inits that omit it still compile) */
};

class UiApp {
public:
    /* Create the window (visible) or a HIDDEN window (visible==false, for a headless smoke). Null
     * if no display/GL. Caller owns the returned pointer. */
    static UiApp *create(const char *title, bool visible);
    ~UiApp();

    /* Hand the UI the data to render. The UI keeps its own copy, so the caller may reuse/destroy its
     * snapshot afterwards. Prefer the rvalue overload in a per-frame loop: it MOVES the snapshot in
     * (no second deep copy of the logs/chat/transcript/terminal — the single largest per-frame cost
     * when those grow), since the host rebuilds a fresh snapshot each frame and discards it anyway. */
    void set_snapshot(const UiSnapshot &snap);
    void set_snapshot(UiSnapshot &&snap);

    /* Runtime accent — re-applies the theme. The settings panel's live picker and the host use it. */
    void   set_accent(Accent a);
    Accent accent() const;

    /* Drain the host-facing commands the UI emitted since the last call (the inverse of the snapshot):
     * the host calls this each frame and dispatches them. The UI itself never touches the backend.
     * Ownership: the returned vector is the caller's; the UI's internal queue is left empty. Call at
     * most once per frame (each call clears what it returns). */
    std::vector<UiCommand> drain_commands();

    /* A.4: absolute OS paths the operator dragged onto the window since the last call (passthrough to the
     * Backend's GLFW drop queue). UNTRUSTED, NON-JAILED — the host stages them via its hardened os-file read.
     * Empty on a headless backend. Call at most once per frame (each call clears the queue). */
    std::vector<std::string> drain_dropped_paths();

    /* True once the user picked File -> Quit; the host loop should break + tear down. */
    bool wants_quit() const;

    /* Mark the window as wanting attention — paired with the approval notification, for the case where
     * the operator is looking at another window entirely. Harmless when the compositor ignores it. */
    void request_attention();

    /* Headless capture only: focus a named window's tab so a screenshot shows it (it may share a dock node
     * with others and not be the default selection). One-shot — re-arm before each screenshot(). No effect on
     * the interactive app beyond that single focus. `pin_dashboard_tab` is the P12 dashboard shorthand. */
    void pin_window(const char *window);
    void pin_dashboard_tab();

    /* Enable/disable the opt-in HyperCat mascot (WI-5), the default-OFF persona window. The host calls this
     * to honor a persisted preference (the settings module) or a programmatic toggle; the operator also
     * toggles it from View -> Panels. Safe before the first frame. */
    void show_mascot(bool on);
    void show_music_player(bool on); /* enable the opt-in Music Player panel (audio feature) */
    void show_network(bool on);      /* enable the opt-in Network (egress-audit) panel (P08.2) */

    /* Seed the UI from the host's persisted settings (WI-2 E1): applies the LIVE fields immediately (accent
     * re-themes, poll cadence, mascot, panel visibility) and stashes `s` as the Settings panel's editable
     * draft. The host calls this at startup after loading + env-merging the settings; the panel then edits
     * the draft and emits SaveSettings/SetSecret. Safe before the first frame. */
    void apply_settings(const UiSettings &s);

    /* Run the event/render loop until the window is closed. */
    void run();

    /* Render exactly `n` frames then return (for the headless smoke / a host that drives its own
     * loop). Returns the number of frames actually rendered (fewer if the window closed). */
    int render_frames(int n);

    /* Render `settle_frames` (so the docking/layout settles), then capture the final frame to
     * `ppm_path` as a binary PPM. For headless screenshots (sighted UI iteration). false if the
     * window closed mid-capture or no backend. */
    bool screenshot(const char *ppm_path, int settle_frames = 4);

private:
    UiApp();
    struct Impl;
    Impl *p_;
};

} // namespace hc::ui

#endif /* HC_UI_HPP */
