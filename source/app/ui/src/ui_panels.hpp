#ifndef HC_UI_PANELS_HPP
#define HC_UI_PANELS_HPP

/* ui_panels — the dockspace + the main menu bar + the panels (agent roster, task board + detail, agenda
 * builder, log, console/token-stream, dashboard, deep_reason chain, workspace file tree, session browser
 * + transcript, operator terminal, tool-authorization approvals, settings), rendered from a UiSnapshot
 * using the restrained-corporate theme helpers. INTERNAL to hc_ui — the host wires each panel's data
 * into the snapshot and dispatches the commands they emit.
 * Threading: single-threaded — UI/GL thread only. Lifetime: free functions over the read-only snapshot
 * + the UiApp-owned DrawCtx — a few keep function-static InputText buffers (ImGui's idiomatic in-place
 * widget state; UI-internal + single-threaded, so safe and intentionally not in the snapshot/DrawCtx). */

#include "hc_ui.hpp"

namespace hc::ui {

class OperatorTextureCache; /* ui_texcache.hpp — the Viewer borrows it to decode+show opened images (W4 P4.2) */
class Editor;               /* ui_editor.hpp — the IDE view borrows it for the cached line model (W5 P5.1)    */

/* Which panels are open (toggled from the View menu + each window's close box). All start visible EXCEPT
 * the opt-in mascot (WI-5), which is default-OFF — the operator turns it on from View -> Panels. */
struct PanelVis {
    bool fleet = true, agenda = true, task = true, builder = true, log = true, console = true,
         dashboard = true, dag = true, activity = true, reasoning = true, files = true,
         approvals = true, sessions = true, transcript = true, terminal = true, settings = true,
         memory = true, chat = true; /* chat = the Conductor front-door panel (Conductor P5) — default-ON */
    bool models = true; /* the Models catalog + per-role assignment panel (Worker Revamp W2) */
    bool roles = true;  /* the Worker Builder's role-template editor (prompt/tools/model per role; P2.3b) */
    bool projects = true; /* the Projects panel — create + switch projects (Wave 3 P3.2) */
    bool skills = true;   /* the per-project Skills authoring panel (Wave 6 P6.3) */
    bool tools = true;    /* the Tools panel — System Tools + third-party management (Custom Tooling) */
    bool viewer = true;   /* the file/media Viewer — renders the opened file (Wave 4 P4.2) */
    bool editor = true;   /* the IDE editor — gutter + syntax of the opened file (Wave 5 P5.1) */
    bool mascot = false; /* opt-in HyperCat mascot (WI-5) — default-OFF */
    bool music_player = false; /* the Music Player (audio feature) — opt-in, default-OFF */
    bool network = false;      /* the Network panel (P08.2 egress audit) — opt-in, default-OFF */
};

/* Per-frame UI context the interactive panels read/write: the host-facing command sink, the runtime
 * accent, panel visibility, and small menu/layout flags. Owned by UiApp across frames — the UI's own
 * mutable state, distinct from the read-only UiSnapshot (state IN) and the UiCommands (intent OUT).
 * Threading: single-threaded — UI/GL thread only. */
struct DrawCtx {
    Accent                 accent = Accent::White; /* current accent (the View menu / Settings change) */
    std::vector<UiCommand> commands;               /* host-facing commands emitted this frame          */
    std::string            selected_task;          /* task id the user clicked (for the detail view)   */
    PanelVis               show;                   /* per-panel visibility                             */
    bool                   want_quit = false;      /* File -> Quit (the host loop reads it)            */
    bool                   reset_layout = true;    /* rebuild the default dock layout next frame       */
    bool                   focus_builder = false;  /* File -> New Agenda focuses the builder           */
    std::string            focus_window;           /* headless capture only: focus this window's tab next frame */
    std::vector<TaskSpec>  draft_tasks;            /* the agenda builder's in-progress task list       */
    UiSettings             settings;               /* the Settings panel's editable draft (WI-2 E1)    */
    bool                   settings_primed = false; /* apply_settings has seeded the draft at least once */
    std::vector<UiRoleRow> roles_draft;            /* the Roles editor's in-progress role templates (P2.3b) */
    bool                   roles_primed = false;    /* the roles draft has been seeded from the snapshot once */
};

/* The fullscreen dock host + the main menu bar (File/Edit/View/Help). Applies the default dock layout on
 * the first frame and when ctx.reset_layout is set. Reads the snapshot for the menu (Open Session etc.). */
void draw_dockspace(const UiSnapshot &, DrawCtx &);

/* One task's position in the layered task-DAG layout (P10). `depth` = the longest dependency chain to a
 * root (the column); `row` = its slot within that column. `dag_layout` is a PURE function (tasks+deps ->
 * grid positions) — the renderer maps (depth,row) to pixels — so it is unit-tested without a GL context.
 * Cycle-tolerant: depths are bounded by the task count, so a dependency cycle lays out without looping. */
struct DagNode {
    size_t task;  /* index into the tasks vector */
    int    depth; /* column (longest dep chain to a root) */
    int    row;   /* slot within the column */
};
std::vector<DagNode> dag_layout(const std::vector<TaskRow> &tasks);

void draw_fleet_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_roles_panel(const UiSnapshot &, DrawCtx &, bool *open); /* the Worker Builder's role-template editor (P2.3b) */
void draw_projects_panel(const UiSnapshot &, DrawCtx &, bool *open); /* create + switch projects (W3 P3.2) */
void draw_skills_panel(const UiSnapshot &, DrawCtx &, bool *open);   /* author per-project skills (W6 P6.3) */
void draw_tools_panel(const UiSnapshot &, DrawCtx &, bool *open);    /* System Tools + third-party (Custom Tooling) */
void draw_music_player_panel(const UiSnapshot &, DrawCtx &, bool *open); /* the Music Player (audio feature, Phase C) */
void draw_network_panel(const UiSnapshot &, DrawCtx &, bool *open);      /* P08.2: the egress-audit Network panel */
void draw_toasts(const UiSnapshot &, DrawCtx &); /* B2: the transient notification stack (floats; not a panel) */
void draw_dag_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_timeline_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_tasks_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_log_panel(const UiSnapshot &, bool *open);
void draw_console_panel(const UiSnapshot &, bool *open);
void draw_dashboard_panel(const UiSnapshot &, bool *open);
void draw_reasoning_panel(const UiSnapshot &, bool *open);
void draw_files_panel(const UiSnapshot &, DrawCtx &, bool *open); /* W4 P4.1: create/delete/rename interactions */
void draw_viewer_panel(const UiSnapshot &, OperatorTextureCache &, bool *open); /* W4 P4.2: render the opened file (doc/media) */
void draw_editor_panel(const UiSnapshot &, Editor &, DrawCtx &, bool *open);    /* W5 P5.1/P5.2: IDE view + live diff */
void draw_sessions_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_transcript_panel(const UiSnapshot &, bool *open);
void draw_terminal_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_settings_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_models_panel(const UiSnapshot &, DrawCtx &, bool *open); /* W2: the Models catalog + role assignment */
void draw_agenda_builder_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_task_detail_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_approvals_panel(const UiSnapshot &, DrawCtx &, bool *open);
void draw_memory_panel(const UiSnapshot &, DrawCtx &, bool *open);

} // namespace hc::ui

#endif /* HC_UI_PANELS_HPP */
