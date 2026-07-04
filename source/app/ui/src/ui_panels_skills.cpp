/* ui_panels (skills cluster) — the per-project Skills authoring panel (W6 P6.3). The operator lists/creates/
 * edits/deletes the SKILL.md skills that the worker prompt discloses + the load_skill tool reads. A PURE
 * renderer: it emits Create/Open/Save/DeleteSkill UiCommands the host executes JAILED (skill_store), and the
 * list refreshes from the host's ~2 Hz scan. The body editor reuses input_multiline_str — the same in-place
 * std::string editor widget as the Roles editor + the IDE. See ui_panels.hpp for the contract. */

#include "ui_panels.hpp"
#include "ui_panels_internal.hpp" /* input_multiline_str — the shared multiline editor widget */

#include "ui_theme.hpp"

#include "imgui.h"

#include <cfloat>
#include <string>

namespace hc::ui {

void draw_skills_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (!ImGui::Begin("Skills", open)) {
        ImGui::End();
        return;
    }
    /* in-place panel state (single UI thread). `edit_buf` is the body being edited; `loaded_skill` tracks which
     * skill it was seeded from so a new selection (or a host re-sync) re-seeds it. */
    static char        newname[64] = "";
    static std::string pending_delete;
    static std::string edit_buf;
    static std::string loaded_skill;

    PanelHeader("NEW SKILL");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##newskill", "skill name (e.g. pdf-extract)", newname, sizeof newname);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ create") && newname[0]) {
        ctx.commands.push_back({UiCommand::Kind::CreateSkill, std::string(newname), "", 0, {}});
        newname[0] = '\0';
    }
    ImGui::Separator();

    if (s.skills.empty()) {
        ImGui::TextColored(muted_v4(), "no skills yet — create one above (the fleet discloses them + load_skill reads them)");
    } else {
        /* O4: a stretch table (name / description / actions) replaces the fixed SameLine(170)/(400) offsets, which
         * pushed the edit+del buttons off-screen on a narrow panel. name+desc ellipsize (+ hover tooltip). */
        if (ImGui::BeginTable("skills", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableSetupColumn("desc", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("act", ImGuiTableColumnFlags_WidthFixed, 96.0f);
            for (const auto &sk : s.skills) {
                ImGui::PushID(sk.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, s.selected_skill == sk.name
                                                         ? accent_v4()
                                                         : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                EllipsisText(sk.name);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, muted_v4());
                EllipsisText(sk.description);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                if (pending_delete == sk.name) {
                    if (ImGui::SmallButton("Yes")) {
                        ctx.commands.push_back({UiCommand::Kind::DeleteSkill, sk.name, "", 0, {}});
                        pending_delete.clear();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("No")) pending_delete.clear();
                } else {
                    if (ImGui::SmallButton("edit"))
                        ctx.commands.push_back({UiCommand::Kind::OpenSkill, sk.name, "", 0, {}});
                    ImGui::SameLine();
                    if (ImGui::SmallButton("del")) pending_delete = sk.name;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    /* the SKILL.md editor for the selected skill (its body read by the host on OpenSkill). */
    if (!s.selected_skill.empty() && s.selected_skill_body) {
        ImGui::Separator();
        if (loaded_skill != s.selected_skill) { /* a fresh selection -> seed the buffer from the host's read */
            edit_buf = *s.selected_skill_body;
            loaded_skill = s.selected_skill;
        }
        ImGui::Text("editing %s/SKILL.md", s.selected_skill.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Save"))
            ctx.commands.push_back({UiCommand::Kind::SaveSkill, s.selected_skill, edit_buf, 0, {}});
        input_multiline_str("##skillbody", edit_buf, ImVec2(-FLT_MIN, -FLT_MIN));
    } else {
        loaded_skill.clear(); /* nothing open -> the next open re-seeds */
    }
    ImGui::End();
}

} // namespace hc::ui
