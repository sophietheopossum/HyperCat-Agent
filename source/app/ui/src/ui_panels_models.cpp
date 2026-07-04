/* ui_panels (models) — the Models catalog + per-role assignment panel (Worker Revamp W2). INTERNAL to hc_ui.
 *
 * The operator designates the AVAILABLE models (an id + an optional note) in a draft catalog, [Apply]'d via
 * SaveSettings, then assigns which model each ROLE runs via a combo that emits AssignRoleModel (live-persisted,
 * mirroring the egress editor — the assignment shows from the SAVED state, never the unsaved draft). The
 * global/fallback model stays in Settings; an unassigned role inherits it. A pure renderer of UiSnapshot.settings
 * + the editable ctx.settings draft; it emits UiCommands and never touches the host. UI/GL thread only. */

#include "ui_panels.hpp"

#include "ui_theme.hpp" /* PanelHeader, muted_v4 */

#include "imgui.h"

#include <cfloat> /* FLT_MIN — the ImGui "fill the cell width" idiom for the role-assign combo */
#include <cstdio>
#include <set>
#include <string>

namespace hc::ui {

namespace {

/* a char-buffer InputText over a std::string (no imgui_stdlib in-tree). Returns true on an edit. */
bool input_str(const char *label, std::string &field)
{
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s", field.c_str());
    if (ImGui::InputText(label, buf, sizeof buf)) {
        field = buf;
        return true;
    }
    return false;
}

} // namespace

void draw_models_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (!ImGui::Begin("Models", open)) {
        ImGui::End();
        return;
    }
    if (!ctx.settings_primed) { /* defensively seed the draft (the host normally seeds it via apply_settings) */
        ctx.settings = s.settings;
        ctx.settings_primed = true;
    }
    UiSettings &d = ctx.settings;

    /* --- the available-models catalog (edited in the draft; [Apply] persists it via SaveSettings) --- */
    PanelHeader("AVAILABLE MODELS");
    ImGui::TextColored(muted_v4(), "the models the fleet may use — an id + an optional note");
    int remove_at = -1;
    for (size_t i = 0; i < d.models.size(); i++) {
        ImGui::PushID((int)i);
        ImGui::SetNextItemWidth(260.0f);
        input_str("##id", d.models[i].id);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        input_str("##note", d.models[i].note);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove_at = (int)i;
        ImGui::PopID();
    }
    if (remove_at >= 0) d.models.erase(d.models.begin() + remove_at);
    if (d.models.empty()) ImGui::TextColored(muted_v4(), "(none yet — add a model below)");

    static char id_add[256] = "", note_add[256] = "";
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##id_add", "model id (e.g. anthropic/claude)", id_add, sizeof id_add);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##note_add", "note (optional)", note_add, sizeof note_add);
    ImGui::SameLine();
    if (ImGui::Button("+ add") && id_add[0]) {
        d.models.push_back({std::string(id_add), std::string(note_add)});
        id_add[0] = note_add[0] = '\0';
    }
    if (ImGui::Button("Apply catalog"))
        ctx.commands.push_back({UiCommand::Kind::SaveSettings, "", "", 0, {}, d});

    /* --- per-role assignment (LIVE: emits AssignRoleModel; shown from the SAVED catalog + assignment) --- */
    ImGui::Spacing();
    PanelHeader("ROLE ASSIGNMENTS");
    ImGui::TextColored(muted_v4(), "which model each role runs — an unassigned role uses the global model");

    std::set<std::string> roles; /* the distinct fleet roles + the reserved conductor/planner (W2.4 consumes) */
    for (const auto &a : s.agents)
        if (!a.role.empty()) roles.insert(a.role);
    roles.insert("conductor");
    roles.insert("planner");

    auto assigned = [&](const std::string &role) -> std::string {
        for (const auto &kv : s.settings.role_models)
            if (kv.first == role) return kv.second;
        return "";
    };
    /* O6: a 2-col stretch table (role / model) replaces the fixed SameLine(170) + 280px combo, which overflowed
     * a narrow panel. The role ellipsizes (+ tooltip); the combo fills its cell. */
    if (ImGui::BeginTable("roleassign", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("role", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("model", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        for (const auto &role : roles) {
            const std::string cur = assigned(role);
            ImGui::PushID(role.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            EllipsisText(role);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const char *preview = cur.empty() ? "(global)" : cur.c_str();
            if (ImGui::BeginCombo("##assign", preview)) {
                if (ImGui::Selectable("(global)", cur.empty()))
                    ctx.commands.push_back({UiCommand::Kind::AssignRoleModel, role, "", 0, {}, {}});
                for (const auto &me : s.settings.models) {
                    if (me.id.empty()) continue;
                    const std::string lbl = me.note.empty() ? me.id : (me.id + "  — " + me.note);
                    if (ImGui::Selectable(lbl.c_str(), me.id == cur))
                        ctx.commands.push_back({UiCommand::Kind::AssignRoleModel, role, me.id, 0, {}, {}});
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (s.settings.models.empty())
        ImGui::TextColored(muted_v4(), "add + Apply a model above before assigning it to a role");

    PanelHeader("GLOBAL / FALLBACK MODEL");
    if (s.settings.model.empty())
        ImGui::TextColored(muted_v4(), "unset — set it in Settings → PROVIDER → model");
    else
        ImGui::TextColored(muted_v4(), "Settings → PROVIDER → model:  %s", s.settings.model.c_str());

    ImGui::End();
}

} // namespace hc::ui
