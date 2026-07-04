/* ui_app — UiApp: owns the Backend, applies the theme + font, and drives the per-frame draw of the
 * dockspace + panels from the current snapshot. See hc_ui.hpp for the full contract.
 * Owns:      the Backend (GL window + ImGui context), the held UiSnapshot, the DrawCtx (UI-internal state:
 *            accent + the host-facing command queue), the SpriteRegistry (all sprite-sheet GPU textures), and
 *            the OperatorTextureCache (GPU textures for operator-opened images) — both freed after the
 *            Backend's GPU sweep, see Impl's teardown note. One UiApp per UI thread.
 * Threading: GL/GLFW are single-threaded — construct and drive a UiApp from ONE thread only. */

#include "hc_ui.hpp"

#include "ui_backend.hpp"
#include "ui_conductor_chat.hpp"
#include "ui_editor.hpp"
#include "ui_mascot.hpp"
#include "ui_panels.hpp"
#include "ui_sprite_registry.hpp"
#include "ui_texcache.hpp"
#include "ui_theme.hpp"

#include "imgui.h" /* SetWindowFocus — the headless-capture Dashboard pin */

#include <utility>

namespace hc::ui {

struct UiApp::Impl {
    Backend       *backend = nullptr;
    UiSnapshot     snap;
    DrawCtx        ctx;     /* UI-internal mutable state: the runtime accent + the host-facing command sink */
    SpriteRegistry    sprites; /* owns ALL sprite-sheet GPU textures (the mascot + the conductor chat avatar/cue);
                                * borrowed by the sprite consumers below it. See ~Impl for the teardown ordering. */
    MascotView        mascot;    /* the opt-in mascot; BORROWS its sheets from `sprites` (owns no textures) */
    ConductorChatView chat_view; /* the conductor chat panel + its avatar/cue players; BORROWS from `sprites` */
    OperatorTextureCache texcache; /* W4 P4.2: GPU textures for operator-OPENED images (the Viewer). Like
                                    * `sprites` it owns ImTextureData and frees ONLY the CPU side in its dtor —
                                    * AFTER `delete backend` (the GPU sweep), calling no ImGui fn — so its
                                    * position among members is unconstrained (it borrows nothing). Its mid-run
                                    * destroy handshake is driven by new_frame() (see ui_texcache.cpp). */
    OperatorTextureCache chat_texcache; /* A: a SEPARATE instance for the conductor chat's inline images, so the
                                         * Viewer's single-consumer-per-frame cache stays uncontended (no cross
                                         * eviction). Same post-backend CPU-only teardown contract as `texcache`. */
    Editor               editor;   /* W5 P5.1: the IDE view's cached line model (plain strings — no GPU/ImGui
                                    * resources, so teardown order is unconstrained). */

    /* Teardown ordering (the ui_sprite contract: free a SpriteSheet's CPU side only AFTER the Backend's GPU
     * shutdown sweep, and never touch a dead ImGui context). Two facts make it hold:
     *   1. `delete backend` runs HERE, in the destructor body, BEFORE any member destructor — so the GPU sweep
     *      happens first.
     *   2. Members destruct in REVERSE declaration order, so the borrowers (`chat_view`, then `mascot`) are
     *      destroyed before `sprites` (the owner) — no dangling borrow — and `sprites` frees its textures after
     *      the sweep. (The borrowers own no textures, so their relative order does not matter.)
     * If `backend` ever becomes an owning smart pointer (no explicit `delete` here), declare it AFTER `sprites`
     * so it is destroyed FIRST and the GPU sweep still precedes ~SpriteRegistry. */
    ~Impl() { delete backend; }
};

UiApp::UiApp() : p_(new Impl) {}

UiApp *UiApp::create(const char *title, bool visible)
{
    Backend *b = Backend::create(title, 1280, 800, visible);
    if (!b) return nullptr; /* no display/GL — caller skips */
    UiApp *app = new UiApp();
    app->p_->backend = b;
    load_fonts();              /* before the first frame, while the atlas is unbuilt */
    apply_theme(Accent::White);
    return app;
}

UiApp::~UiApp()
{
    delete p_; /* ~Impl tears down the Backend (ImGui + GL + window) before the sprite members — see Impl */
}

void UiApp::set_snapshot(const UiSnapshot &s) { p_->snap = s; }
void UiApp::set_snapshot(UiSnapshot &&s) { p_->snap = std::move(s); }

void UiApp::set_accent(Accent a)
{
    p_->ctx.accent = a;
    apply_theme(a);
}

Accent UiApp::accent() const { return p_->ctx.accent; }

std::vector<UiCommand> UiApp::drain_commands()
{
    std::vector<UiCommand> out = std::move(p_->ctx.commands);
    p_->ctx.commands.clear(); /* required: std::move leaves the source in a valid-but-unspecified state */
    return out;
}

std::vector<std::string> UiApp::drain_dropped_paths()
{
    return p_->backend ? p_->backend->drain_dropped_paths() : std::vector<std::string>{};
}

bool UiApp::wants_quit() const { return p_->ctx.want_quit; }

void UiApp::pin_window(const char *window) { p_->ctx.focus_window = window ? window : ""; } /* capture: focus a tab */
void UiApp::pin_dashboard_tab() { pin_window("Dashboard"); } /* headless capture: select Dashboard (P12) */

void UiApp::show_mascot(bool on) { p_->ctx.show.mascot = on; } /* enable the opt-in mascot (WI-5) */
void UiApp::show_music_player(bool on) { p_->ctx.show.music_player = on; } /* enable the opt-in Music Player */
void UiApp::show_network(bool on) { p_->ctx.show.network = on; }           /* enable the opt-in Network panel */

void UiApp::apply_settings(const UiSettings &s)
{
    set_accent(s.accent);          /* LIVE: sets ctx.accent + re-themes */
    p_->ctx.show.mascot = s.mascot; /* LIVE */
    p_->ctx.settings = s;          /* seed the Settings panel's editable draft */
    p_->ctx.settings_primed = true;
}

namespace {
void draw_all(const UiSnapshot &s, DrawCtx &ctx, MascotView &mascot, ConductorChatView &chat,
              SpriteRegistry &sprites, OperatorTextureCache &texcache, OperatorTextureCache &chat_texcache,
              Editor &editor)
{
    texcache.new_frame();      /* W4 P4.2: drive the operator-image texture destroy handshake before any draw */
    chat_texcache.new_frame(); /* A: the conductor chat's separate inline-image cache (same per-frame handshake) */
    draw_dockspace(s, ctx); /* the dock host + the main menu bar (reads visibility/accent/quit in ctx) */
    if (ctx.show.fleet) draw_fleet_panel(s, ctx, &ctx.show.fleet);
    if (ctx.show.dag) draw_dag_panel(s, ctx, &ctx.show.dag);
    if (ctx.show.activity) draw_timeline_panel(s, ctx, &ctx.show.activity);
    if (ctx.show.agenda) draw_tasks_panel(s, ctx, &ctx.show.agenda);
    if (ctx.show.task) draw_task_detail_panel(s, ctx, &ctx.show.task);
    if (ctx.show.builder) draw_agenda_builder_panel(s, ctx, &ctx.show.builder);
    if (ctx.show.approvals) draw_approvals_panel(s, ctx, &ctx.show.approvals);
    if (ctx.show.memory) draw_memory_panel(s, ctx, &ctx.show.memory);
    if (ctx.show.dashboard) draw_dashboard_panel(s, &ctx.show.dashboard);
    if (ctx.show.log) draw_log_panel(s, &ctx.show.log);
    if (ctx.show.console) draw_console_panel(s, &ctx.show.console);
    if (ctx.show.reasoning) draw_reasoning_panel(s, &ctx.show.reasoning);
    if (ctx.show.transcript) draw_transcript_panel(s, &ctx.show.transcript);
    if (ctx.show.files) draw_files_panel(s, ctx, &ctx.show.files);
    if (ctx.show.music_player) draw_music_player_panel(s, ctx, &ctx.show.music_player); /* audio feature (Phase C) */
    if (ctx.show.network) draw_network_panel(s, ctx, &ctx.show.network); /* P08.2: egress audit (opt-in) */
    if (ctx.show.viewer) draw_viewer_panel(s, texcache, &ctx.show.viewer); /* W4 P4.2: the opened-file viewer */
    if (ctx.show.editor) draw_editor_panel(s, editor, ctx, &ctx.show.editor); /* W5 P5.1/P5.2: IDE view + live diff */
    if (ctx.show.sessions) draw_sessions_panel(s, ctx, &ctx.show.sessions);
    if (ctx.show.terminal) draw_terminal_panel(s, ctx, &ctx.show.terminal);
    if (ctx.show.settings) draw_settings_panel(s, ctx, &ctx.show.settings);
    if (ctx.show.models) draw_models_panel(s, ctx, &ctx.show.models); /* W2: the Models catalog + role assignment */
    if (ctx.show.roles) draw_roles_panel(s, ctx, &ctx.show.roles);    /* P2.3b: the Worker Builder role editor */
    if (ctx.show.projects) draw_projects_panel(s, ctx, &ctx.show.projects); /* W3 P3.2: create + switch projects */
    if (ctx.show.skills) draw_skills_panel(s, ctx, &ctx.show.skills);       /* W6 P6.3: author per-project skills */
    if (ctx.show.tools) draw_tools_panel(s, ctx, &ctx.show.tools);          /* Custom Tooling: System + third-party */
    /* The Conductor front door (P5) — the primary surface (default-ON). Its avatar + tool cue borrow from the
     * registry, so ensure_loaded() before drawing (lazy + idempotent — the catalog decodes once a sprite
     * consumer is shown, then is shared across consumers). */
    if (ctx.show.chat) {
        sprites.ensure_loaded();
        chat.draw(s, ctx, sprites, chat_texcache, &ctx.show.chat);
    }
    /* The opt-in mascot (default-OFF; ctx.show.mascot from View -> Panels). Drawn last so it floats above the
     * panels. */
    if (ctx.show.mascot) {
        sprites.ensure_loaded();
        mascot.draw(s, sprites, &ctx.show.mascot);
    }
    /* B2: the toast stack — drawn last so it floats above the panels, BEFORE the focus_window block so a toast click
     * (which sets ctx.show.approvals + ctx.focus_window) fronts the Approvals panel this same frame. */
    draw_toasts(s, ctx);
    /* Headless capture: focus a named window's tab in its dock node so the shot shows it (a panel sharing a
     * node is otherwise not the default selection). One-shot — consumed here so a stray call on a VISIBLE
     * window self-corrects after one frame instead of latching the operator's tab choice; the capture path
     * re-arms it each frame. */
    if (!ctx.focus_window.empty()) {
        ImGui::SetWindowFocus(ctx.focus_window.c_str());
        ctx.focus_window.clear();
    }
}
} // namespace

void UiApp::run()
{
    while (p_->backend->begin_frame()) {
        draw_all(p_->snap, p_->ctx, p_->mascot, p_->chat_view, p_->sprites, p_->texcache, p_->chat_texcache, p_->editor);
        p_->backend->end_frame();
    }
}

int UiApp::render_frames(int n)
{
    int done = 0;
    for (int i = 0; i < n; i++) {
        if (!p_->backend->begin_frame()) break;
        draw_all(p_->snap, p_->ctx, p_->mascot, p_->chat_view, p_->sprites, p_->texcache, p_->chat_texcache, p_->editor);
        p_->backend->end_frame();
        done++;
    }
    return done;
}

bool UiApp::screenshot(const char *ppm_path, int settle_frames)
{
    if (!p_->backend) return false;
    for (int i = 0; i < settle_frames; i++) { /* let ImGui's layout/docking settle before capturing */
        if (!p_->backend->begin_frame()) return false;
        draw_all(p_->snap, p_->ctx, p_->mascot, p_->chat_view, p_->sprites, p_->texcache, p_->chat_texcache, p_->editor);
        p_->backend->end_frame();
    }
    if (!p_->backend->begin_frame()) return false;
    draw_all(p_->snap, p_->ctx, p_->mascot, p_->chat_view, p_->sprites, p_->texcache, p_->chat_texcache, p_->editor);
    p_->backend->end_frame(ppm_path); /* capture the final frame's back buffer */
    return true;
}

} // namespace hc::ui
