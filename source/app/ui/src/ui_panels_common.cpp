/* ui_panels (common cluster) — the dockspace + main menu infra and the shared state_color helper. See
 * ui_panels.hpp for the full contract and ui_panels_internal.hpp for the private cross-TU surface. */

#include "ui_panels.hpp"
#include "ui_panels_internal.hpp"

#include "ui_theme.hpp"

#include "imgui.h"
#include "imgui_internal.h" /* DockBuilder* — programmatic default layout */

#include <cstring> /* strcmp — the consent-modal phrase match */
#include <string>

namespace hc::ui {

/* A status color for an agent/task state string: accent for active, dim red for dead/failed, muted
 * for idle/pending, default for done. Keeps state legible without a rainbow. */
ImVec4 state_color(const std::string &st)
{
    if (st == "dead" || st == "failed") return err_v4();
    if (st == "ready" || st == "running") return accent_v4();
    if (st == "spawned" || st == "pending" || st == "assigned") return muted_v4();
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

namespace {
/* The std::string resize callback for input_multiline_str (no imgui_stdlib in-tree): grows the string as the
 * user types so the text is not capped by a fixed buffer. UserData is the &std::string, valid only for the
 * one InputTextMultiline call. Mirrors the canonical imgui_stdlib resize callback. */
int str_resize_cb(ImGuiInputTextCallbackData *data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto *str = static_cast<std::string *>(data->UserData);
        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = str->data();
    }
    return 0;
}
} // namespace

/* SHARED (declared in ui_panels_internal.hpp): the Roles editor (P2.3b) + the IDE editor (P5.3) both edit a
 * std::string in place. */
bool input_multiline_str(const char *label, std::string &str, const ImVec2 &size)
{
    return ImGui::InputTextMultiline(label, str.data(), str.capacity() + 1, size,
                                     ImGuiInputTextFlags_CallbackResize, str_resize_cb, &str);
}

bool consent_modal(const char *id, const char *disclaimer, const char *phrase, bool danger)
{
    bool confirmed = false;
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextColored(danger ? err_v4() : muted_v4(), "%s", disclaimer);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextColored(muted_v4(), "Type  %s  to confirm:", phrase);
        static char buf[64] = ""; /* REUSE CAVEAT: a single shared buffer, correct ONLY because ImGui runs one modal
                                   * at a time (BeginPopupModal is exclusive); cleared on confirm/cancel. Two nested
                                   * consent_modals would alias this — give them distinct buffers if that ever happens. */
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputText("##consent", buf, sizeof buf);
        const bool match = (std::strcmp(buf, phrase) == 0);
        ImGui::Spacing();
        ImGui::BeginDisabled(!match); /* Confirm is dead until the exact phrase is typed — never a one-click arm */
        if (ImGui::Button("Confirm")) {
            confirmed = true;
            buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return confirmed;
}

namespace {

/* Build + dock the default layout once (first frame / View->Reset Layout). Window names MUST match the
 * Begin() titles below. */
void setup_dock_layout(ImGuiID root)
{
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = root, left, right, c_bot, l_bot, r_bot;
    left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.21f, nullptr, &center);
    right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
    c_bot = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.32f, nullptr, &center);
    l_bot = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.50f, nullptr, &left);
    r_bot = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.58f, nullptr, &right);

    ImGui::DockBuilderDockWindow("Agenda", left);
    ImGui::DockBuilderDockWindow("Agenda Builder", l_bot);
    ImGui::DockBuilderDockWindow("Approvals", l_bot);
    ImGui::DockBuilderDockWindow("Fleet", right);
    ImGui::DockBuilderDockWindow("Sessions", r_bot);
    ImGui::DockBuilderDockWindow("Files", r_bot);
    ImGui::DockBuilderDockWindow("Music Player", r_bot); /* audio feature — opt-in, tabs with the side utilities */
    ImGui::DockBuilderDockWindow("Viewer", center); /* W4 P4.2: the opened-file viewer, in the main area */
    ImGui::DockBuilderDockWindow("Editor", center); /* W5 P5.1: the IDE view, tabbed in the main area */
    ImGui::DockBuilderDockWindow("Settings", r_bot);
    ImGui::DockBuilderDockWindow("Models", r_bot); /* W2: tabs alongside Settings */
    ImGui::DockBuilderDockWindow("Roles", r_bot);  /* P2.3b: the Worker Builder role editor, tabbed by Models */
    ImGui::DockBuilderDockWindow("Projects", r_bot); /* W3 P3.2: create + switch projects */
    ImGui::DockBuilderDockWindow("Skills", r_bot);   /* W6 P6.3: per-project skills authoring */
    ImGui::DockBuilderDockWindow("Tools", r_bot);    /* Custom Tooling: System Tools + third-party management */
    ImGui::DockBuilderDockWindow("Memory", r_bot);
    ImGui::DockBuilderDockWindow("Conductor", center); /* the front-door chat — the primary surface (P5) */
    ImGui::DockBuilderDockWindow("Log", center);
    ImGui::DockBuilderDockWindow("Console", center);
    ImGui::DockBuilderDockWindow("Reasoning", center);
    ImGui::DockBuilderDockWindow("Transcript", center);
    ImGui::DockBuilderDockWindow("Task", center);
    ImGui::DockBuilderDockWindow("Plan", center);
    ImGui::DockBuilderDockWindow("Dashboard", c_bot);
    ImGui::DockBuilderDockWindow("Activity", c_bot);
    ImGui::DockBuilderDockWindow("Terminal", c_bot);
    ImGui::DockBuilderFinish(root);
}

void draw_menu(const UiSnapshot &s, DrawCtx &ctx)
{
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Agenda", "Ctrl+N")) {
            ctx.show.builder = true;
            ctx.focus_builder = true;
        }
        if (ImGui::BeginMenu("Open Session", !s.sessions.empty())) {
            for (const auto &se : s.sessions) {
                std::string lbl = se.title + "  (" + se.updated + ")";
                if (ImGui::MenuItem(lbl.c_str())) {
                    ctx.show.transcript = true;
                    ctx.commands.push_back({UiCommand::Kind::OpenSession, se.id, "", 0, {}});
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) ctx.want_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Copy transcript", nullptr, false, !s.transcript.empty())) {
            std::string all;
            for (const auto &l : s.transcript) all += l + "\n";
            ImGui::SetClipboardText(all.c_str());
        }
        if (ImGui::MenuItem("Copy reasoning", nullptr, false, !s.reasoning.empty()))
            ImGui::SetClipboardText(s.reasoning.c_str());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        /* The 17 panel toggles grouped by function (was a flat wall of 17). Pure reshuffle — each item
         * binds the same &ctx.show.X bool and the DockBuilder window-title strings are never touched, so
         * docking cannot break. Layout (accent + reset) moves to its own submenu. */
        if (ImGui::BeginMenu("Panels")) {
            ImGui::SeparatorText("Core");
            ImGui::MenuItem("Conductor (chat)", nullptr, &ctx.show.chat);
            ImGui::MenuItem("Fleet", nullptr, &ctx.show.fleet);
            ImGui::MenuItem("Roles (worker builder)", nullptr, &ctx.show.roles); /* P2.3b: role-template editor */
            ImGui::MenuItem("Projects", nullptr, &ctx.show.projects); /* W3 P3.2: create + switch projects */
            ImGui::MenuItem("Skills", nullptr, &ctx.show.skills);     /* W6 P6.3: author per-project skills */
            ImGui::MenuItem("Tools", nullptr, &ctx.show.tools);       /* Custom Tooling: System + third-party */
            ImGui::MenuItem("Agenda", nullptr, &ctx.show.agenda);
            ImGui::MenuItem("Task detail", nullptr, &ctx.show.task);
            ImGui::MenuItem("Activity (timeline)", nullptr, &ctx.show.activity);
            ImGui::MenuItem("Plan (task graph)", nullptr, &ctx.show.dag);
            ImGui::SeparatorText("Work");
            ImGui::MenuItem("Agenda Builder", nullptr, &ctx.show.builder);
            ImGui::MenuItem("Approvals", nullptr, &ctx.show.approvals);
            ImGui::MenuItem("Terminal", nullptr, &ctx.show.terminal);
            ImGui::SeparatorText("Analysis");
            ImGui::MenuItem("Dashboard", nullptr, &ctx.show.dashboard);
            ImGui::MenuItem("Reasoning", nullptr, &ctx.show.reasoning);
            ImGui::MenuItem("Memory", nullptr, &ctx.show.memory);
            ImGui::MenuItem("Transcript", nullptr, &ctx.show.transcript);
            ImGui::SeparatorText("System");
            ImGui::MenuItem("Log", nullptr, &ctx.show.log);
            ImGui::MenuItem("Console", nullptr, &ctx.show.console);
            ImGui::MenuItem("Files", nullptr, &ctx.show.files);
            ImGui::MenuItem("Viewer", nullptr, &ctx.show.viewer); /* W4 P4.2: the opened-file viewer */
            ImGui::MenuItem("Editor (IDE)", nullptr, &ctx.show.editor); /* W5 P5.1: gutter + syntax view */
            ImGui::MenuItem("Sessions", nullptr, &ctx.show.sessions);
            ImGui::MenuItem("Settings", nullptr, &ctx.show.settings);
            ImGui::MenuItem("Models", nullptr, &ctx.show.models); /* W2: catalog + per-role assignment */
            ImGui::MenuItem("Music Player", nullptr, &ctx.show.music_player); /* audio feature — opt-in, default-OFF */
            ImGui::MenuItem("Network (egress)", nullptr, &ctx.show.network); /* P08.2 egress audit — opt-in, default-OFF */
            ImGui::SeparatorText("Persona");
            ImGui::MenuItem("HyperCat Mascot", nullptr, &ctx.show.mascot); /* opt-in, default-OFF (WI-5) */
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layout")) {
            if (ImGui::BeginMenu("Accent")) {
                for (const AccentOpt &o : k_accents)
                    if (ImGui::MenuItem(o.name, nullptr, ctx.accent == o.val) && ctx.accent != o.val) {
                        ctx.accent = o.val;
                        apply_theme(o.val);
                    }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Reset layout")) {
                ctx.reset_layout = true;
                ctx.show = PanelVis{}; /* restore visibility too, so a panel closed via its X comes back */
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About HyperCat")) ImGui::OpenPopup("About HyperCat");
        ImGui::EndMenu();
    }
    /* the transient host status (e.g. "agenda started (3 tasks)") — every operator action lands here
     * so nothing is silently dropped. Set by the host on the next snapshot; cleared when it expires. */
    if (!s.notice.empty()) {
        ImGui::TextColored(accent_v4(), "  %s", s.notice.c_str());
    }
    /* the live provider/model, right-aligned in the menu bar — always-visible context */
    {
        std::string ctxlbl = (s.provider.empty() ? "offline" : s.provider) +
                             (s.model.empty() ? "" : ("  " + s.model));
        float w = ImGui::CalcTextSize(ctxlbl.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - w - ImGui::GetStyle().FramePadding.x * 2.0f);
        ImGui::TextColored(muted_v4(), "%s", ctxlbl.c_str());
    }
    ImGui::EndMenuBar();
}

} // namespace

void draw_dockspace(const UiSnapshot &s, DrawCtx &ctx)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##hypercat_dockhost", nullptr, flags);
    ImGui::PopStyleVar(3);

    draw_menu(s, ctx);

    /* global keyboard shortcuts (advertised in the menu) */
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) {
        ctx.show.builder = true;
        ctx.focus_builder = true;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) ctx.want_quit = true;

    ImGuiID dockspace_id = ImGui::GetID("hypercat_dockspace");
    if (ctx.reset_layout) {
        setup_dock_layout(dockspace_id);
        ctx.reset_layout = false;
    }
    ImGui::DockSpace(dockspace_id);

    /* the About popup (opened from Help) */
    if (ImGui::BeginPopupModal("About HyperCat", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("HyperCat");
        ImGui::TextColored(muted_v4(), "a standalone multi-agent workspace (C/C++)");
        ImGui::TextColored(muted_v4(), "v0.1 — restrained corporate");
        ImGui::Spacing();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace hc::ui
