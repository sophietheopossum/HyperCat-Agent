/* ui_panels (projects) — the Projects panel (Wave 3 P3.2). INTERNAL to hc_ui.
 *
 * Lists the projects (the active one marked with the single accent dot), and lets the operator CREATE a project
 * (emits CreateProject — the host mints the id + the sealed subtree) and SWITCH to one (emits SwitchProject — the
 * host re-execs into it, giving HARD isolation by a fresh address space). Switch is CONFIRM-gated inline because a
 * re-exec reloads HyperCat. A pure renderer of UiSnapshot.projects — it emits UiCommands and never touches the
 * host or the bus. UI/GL thread only. */

#include "ui_panels.hpp"

#include "ui_theme.hpp" /* PanelHeader, muted_v4, accent_v4 */

#include "imgui.h"

#include <cstdio>
#include <string>

namespace hc::ui {

void draw_projects_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (!ImGui::Begin("Projects", open)) {
        ImGui::End();
        return;
    }

    PanelHeader("PROJECTS");
    ImGui::TextColored(muted_v4(), "each project is a sealed sandbox — its own memory, sessions, workspaces");
    ImGui::TextColored(muted_v4(), "switching RELOADS HyperCat into the chosen project (hard isolation)");
    ImGui::Spacing();

    static std::string pending_switch; /* the id awaiting the inline switch confirmation  */
    static std::string pending_delete; /* the id awaiting the inline delete confirmation  */
    static std::string renaming;       /* the id being renamed (its edit row is shown)    */
    static char        rename_buf[128] = "";

    if (s.projects.empty()) ImGui::TextColored(muted_v4(), "(no projects yet — create one below)");
    /* O5: a stretch table (name / id / actions) replaces the fixed SameLine(200)/(330) offsets (which pushed the
     * action buttons off-screen on a narrow panel). name+id ellipsize (+ tooltip); the inline rename editor is
     * rendered below the table (keyed on the id), so the action column always fits. */
    if (!s.projects.empty() &&
        ImGui::BeginTable("projects", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("act", ImGuiTableColumnFlags_WidthFixed, 190.0f); /* fits Switch+Rename+Delete */
        for (const auto &p : s.projects) {
            ImGui::PushID(p.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(p.active ? accent_v4() : muted_v4(), "%s", p.active ? "* " : "  "); /* accent dot */
            ImGui::SameLine(0.0f, 0.0f);
            EllipsisText(p.display.empty() ? p.id : p.display);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, muted_v4());
            EllipsisText(p.id);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (pending_switch == p.id) { /* switch confirm (a re-exec) takes over the action cell */
                ImGui::TextColored(muted_v4(), "reload?");
                ImGui::SameLine();
                if (ImGui::SmallButton("Yes")) {
                    ctx.commands.push_back({UiCommand::Kind::SwitchProject, p.id, "", 0, {}});
                    pending_switch.clear();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("No")) pending_switch.clear();
            } else if (pending_delete == p.id) { /* delete confirm */
                ImGui::TextColored(muted_v4(), "delete?");
                ImGui::SameLine();
                if (ImGui::SmallButton("Yes")) {
                    ctx.commands.push_back({UiCommand::Kind::DeleteProject, p.id, "", 0, {}});
                    pending_delete.clear();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("No")) pending_delete.clear();
            } else {
                if (p.active) ImGui::TextColored(muted_v4(), "(active)");
                else if (ImGui::SmallButton("Switch")) pending_switch = p.id;
                ImGui::SameLine();
                if (ImGui::SmallButton("Rename")) { /* open the inline rename row below the table */
                    renaming = p.id;
                    std::snprintf(rename_buf, sizeof rename_buf, "%s",
                                  p.display.empty() ? p.id.c_str() : p.display.c_str());
                }
                if (!p.active) { /* delete is UI-only + confirm-gated; the ACTIVE project can't be deleted */
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) pending_delete = p.id;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!renaming.empty()) { /* the inline rename editor, below the table (keyed on the id only) */
        ImGui::PushID(renaming.c_str());
        ImGui::TextColored(muted_v4(), "rename %s:", renaming.c_str());
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##rename", "new name", rename_buf, sizeof rename_buf);
        ImGui::SameLine();
        if (ImGui::SmallButton("Save") && rename_buf[0]) {
            ctx.commands.push_back({UiCommand::Kind::RenameProject, renaming, std::string(rename_buf), 0, {}});
            renaming.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) renaming.clear();
        ImGui::PopID();
    }

    ImGui::Spacing();
    PanelHeader("NEW PROJECT");
    static char newname[128] = "";
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##newproject", "project name", newname, sizeof newname);
    ImGui::SameLine();
    if (ImGui::Button("+ create") && newname[0]) {
        ctx.commands.push_back({UiCommand::Kind::CreateProject, std::string(newname), "", 0, {}});
        newname[0] = '\0';
    }
    ImGui::TextColored(muted_v4(), "a new project starts empty (no workers, its own memory) — switch to enter it");

    ImGui::End();
}

} // namespace hc::ui
