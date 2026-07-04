/* ui_panels (roles) — the Worker Builder's ROLE-TEMPLATE editor (P2.3b). INTERNAL to hc_ui.
 *
 * A worker ROLE is a template: a system-prompt overlay (appended after the base prompt), a tool subset, and a
 * model. The operator edits these in a draft (ctx.roles_draft) seeded once from the snapshot's role_defs; per
 * role, "Save role" emits EditRole (the host upserts + persists to roles.json + applies to that role's NEXT
 * spawn), "Remove role" emits RemoveRole, and "+ add role" appends a fresh draft row. A pure renderer of
 * UiSnapshot.role_defs + the editable draft — it emits UiCommands and never touches the host or the bus.
 *
 * Tool selection invariant: a role with an EMPTY toolset means "all tools" in the role table, so unchecking
 * every box would silently GRANT everything. We forbid that by keeping >=1 box checked (the last is disabled)
 * and by storing tools EXPLICITLY (empty is expanded to the full set on seed), so the convention is never
 * tripped by accident. Unknown/over-long fields are bounded host-side on save (roletable_validate). UI/GL
 * thread only. */

#include "ui_panels.hpp"
#include "ui_panels_internal.hpp" /* input_multiline_str — the shared std::string InputTextMultiline helper */

#include "ui_theme.hpp" /* PanelHeader, muted_v4 */

#include "imgui.h"

#include <cstddef>
#include <string>
#include <vector>

namespace hc::ui {

namespace {

bool has_tool(const std::vector<std::string> &tools, const char *name)
{
    for (const auto &t : tools)
        if (t == name) return true;
    return false;
}

/* Expand the role-table "empty = all tools" convention into an explicit list (over the host's tool catalog),
 * so the checkboxes map 1:1 and the operator cannot accidentally produce an empty (= all) set. A fresh role
 * likewise starts with all tools. */
void expand_tools(std::vector<std::string> &tools, const std::vector<std::string> &catalog)
{
    if (!tools.empty()) return;
    tools = catalog;
}

/* Render the tool checkboxes (3 per row) over the host's tool catalog, keeping >=1 checked. Mutates `tools`. */
void draw_tool_checkboxes(std::vector<std::string> &tools, const std::vector<std::string> &catalog)
{
    const size_t n = catalog.size();
    int          checked = 0;
    for (const auto &nm : catalog)
        if (has_tool(tools, nm.c_str())) checked++;
    for (size_t t = 0; t < n; t++) {
        const char *nm = catalog[t].c_str();
        bool        on = has_tool(tools, nm);
        const bool  lock_last = on && checked <= 1; /* the last enabled tool can't be unchecked (empty = all) */
        if (lock_last) ImGui::BeginDisabled();
        if (ImGui::Checkbox(nm, &on)) {
            if (on) {
                if (!has_tool(tools, nm)) tools.push_back(nm);
            } else {
                for (size_t k = 0; k < tools.size(); k++)
                    if (tools[k] == nm) {
                        tools.erase(tools.begin() + k);
                        break;
                    }
            }
        }
        if (lock_last) ImGui::EndDisabled();
        if ((t % 3) != 2 && t + 1 < n) ImGui::SameLine(); /* 3 per row */
    }
}

} // namespace

void draw_roles_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (!ImGui::Begin("Roles", open)) {
        ImGui::End();
        return;
    }

    if (!ctx.roles_primed) { /* seed the editable draft once from the saved templates (like the Settings draft) */
        ctx.roles_draft = s.role_defs;
        for (auto &r : ctx.roles_draft) expand_tools(r.tools, s.tool_catalog);
        ctx.roles_primed = true;
    }

    PanelHeader("WORKER ROLES");
    ImGui::TextColored(muted_v4(), "templates each worker instantiates — a prompt overlay, a tool subset, a model");
    ImGui::TextColored(muted_v4(), "edits apply to NEW workers of that role (existing workers keep their identity)");

    int remove_at = -1;
    for (size_t i = 0; i < ctx.roles_draft.size(); i++) {
        UiRoleRow &r = ctx.roles_draft[i];
        ImGui::PushID((int)i);
        if (i == 0) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver); /* the first role opens by default */
        if (ImGui::CollapsingHeader(r.role.empty() ? "(unnamed role)" : r.role.c_str())) {
            ImGui::TextColored(muted_v4(), "system-prompt overlay (appended after the base prompt)");
            input_multiline_str("##overlay", r.prompt_overlay, ImVec2(-1.0f, 90.0f));

            ImGui::TextColored(muted_v4(), "tools the role may use (at least one):");
            draw_tool_checkboxes(r.tools, s.tool_catalog);

            ImGui::TextColored(muted_v4(), "model:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(280.0f);
            const char *preview = r.model.empty() ? "(global)" : r.model.c_str();
            if (ImGui::BeginCombo("##rolemodel", preview)) {
                if (ImGui::Selectable("(global)", r.model.empty())) r.model.clear();
                for (const auto &me : s.settings.models) {
                    if (me.id.empty()) continue;
                    const std::string lbl = me.note.empty() ? me.id : (me.id + "  — " + me.note);
                    if (ImGui::Selectable(lbl.c_str(), me.id == r.model)) r.model = me.id;
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            if (ImGui::Button("Save role") && !r.role.empty())
                ctx.commands.push_back({UiCommand::Kind::EditRole, "", "", 0, {}, {}, r});
            ImGui::SameLine();
            if (ImGui::Button("Remove role")) remove_at = (int)i;
        }
        ImGui::PopID();
    }
    if (remove_at >= 0) {
        const std::string nm = ctx.roles_draft[(size_t)remove_at].role;
        /* only ask the host to delete a role it actually has saved; a never-saved draft row just drops locally */
        bool saved = false;
        for (const auto &rd : s.role_defs)
            if (rd.role == nm) { saved = true; break; }
        if (saved && !nm.empty()) ctx.commands.push_back({UiCommand::Kind::RemoveRole, nm, "", 0, {}});
        ctx.roles_draft.erase(ctx.roles_draft.begin() + remove_at);
    }
    if (ctx.roles_draft.empty())
        ImGui::TextColored(muted_v4(), "(no roles — add one below; a worker with no matching role gets the base "
                                       "prompt + all tools)");

    ImGui::Spacing();
    PanelHeader("ADD ROLE");
    static char newname[64] = "";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##newrole", "role name (e.g. reviewer)", newname, sizeof newname);
    ImGui::SameLine();
    if (ImGui::Button("+ add role") && newname[0]) {
        UiRoleRow nr;
        nr.role = newname;
        expand_tools(nr.tools, s.tool_catalog); /* a new role starts with full capability (all tools) */
        ctx.roles_draft.push_back(std::move(nr));
        newname[0] = '\0';
    }
    if (s.tool_catalog.empty())
        ImGui::TextColored(muted_v4(), "(tool catalog unavailable — open this panel with the host running)");
    ImGui::TextColored(muted_v4(), "the new role appears above — set it up, then Save role to persist it");

    ImGui::End();
}

} // namespace hc::ui
