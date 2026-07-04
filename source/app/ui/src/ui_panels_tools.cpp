/* ui_panels (tools cluster) — the Tools panel (Custom Tooling program, Wave C): a desktop-like split explorer
 * (modeled on the file browser) for the agent toolset. LEFT = a categorized tree (System Tools / Third-party /
 * Available); RIGHT = the selected tool's detail (kind, enable toggle, description, and — for third-party — its
 * declared manifest + disclaimer, enriched in Wave E). A top kill-switch disables ALL third-party tools at once.
 *
 * PURE RENDERER: reads UiSnapshot.tools (+ third_party_tools_disabled) and emits ToggleTool / RemoveTool /
 * DisableAllThirdParty. Enabling an un-approved third-party tool is type-to-confirm gated (consent_modal); Remove
 * is consent-gated too. (TestTool/dry-run is an honest deferral — a notice, not yet a facility; see EXPANSIONS.)
 * The host fills the rows + flips the settings + launches/reaps. Every host-supplied string
 * (name/description/manifest) is author/model-influenced and is rendered via "%s"/TextUnformatted — NEVER
 * interpreted. See ui_panels.hpp for the contract. */

#include "ui_panels.hpp"

#include "ui_panels_internal.hpp" /* consent_modal — the type-to-confirm gate for enable/remove (Wave E) */
#include "ui_theme.hpp"

#include "imgui.h"

#include <cstdio>
#include <string>
#include <vector>

namespace hc::ui {

namespace {

const char *kind_str(ToolRow::Kind k)
{
    switch (k) {
    case ToolRow::Kind::System: return "system";
    case ToolRow::Kind::ThirdParty: return "third-party";
    default: return "available";
    }
}

/* LEFT pane — the categorized tool tree (mirrors the file browser's draw_tree_node). Selection drives the
 * detail pane via the function-static `selected`. */
void draw_tools_tree(const UiSnapshot &s, std::string &selected)
{
    auto section = [&](const char *label, ToolRow::Kind kind) {
        int count = 0;
        for (const auto &t : s.tools)
            if (t.kind == kind) count++;
        char hdr[64];
        std::snprintf(hdr, sizeof hdr, "%s (%d)", label, count);
        if (!ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) return;
        if (count == 0)
            ImGui::TextColored(muted_v4(), "%s", kind == ToolRow::Kind::ThirdParty ? "none installed yet" : "none");
        for (const auto &t : s.tools) {
            if (t.kind != kind) continue;
            ImGui::PushID(t.name.c_str());
            ImGuiTreeNodeFlags lf = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                    ImGuiTreeNodeFlags_SpanAvailWidth;
            if (selected == t.name) lf |= ImGuiTreeNodeFlags_Selected;
            ImGui::TreeNodeEx(t.name.c_str(), lf, "%s%s", t.name.c_str(), t.enabled ? "" : "  (off)");
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected = t.name;
            ImGui::PopID();
        }
        ImGui::TreePop();
    };
    section("System Tools", ToolRow::Kind::System);
    section("Third-party", ToolRow::Kind::ThirdParty);
    bool any_avail = false;
    for (const auto &t : s.tools)
        if (t.kind == ToolRow::Kind::Available) any_avail = true;
    if (any_avail) section("Available", ToolRow::Kind::Available);
}

/* join a string list for a one-line PERMISSIONS cell. */
std::string join_csv(const std::vector<std::string> &v)
{
    std::string out;
    for (const auto &x : v) {
        if (!out.empty()) out += ", ";
        out += x;
    }
    return out;
}

/* one row of the PERMISSIONS table: a muted label + an ellipsized (hover-tooltip) value. */
void perm_row(const char *label, const std::string &value)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(muted_v4(), "%s", label);
    ImGui::TableNextColumn();
    EllipsisText(value.empty() ? "—" : value);
}

/* RIGHT pane — the selected tool's detail. */
void draw_tool_detail(const UiSnapshot &s, DrawCtx &ctx, const std::string &selected)
{
    const ToolRow *t = nullptr;
    for (const auto &r : s.tools)
        if (r.name == selected) {
            t = &r;
            break;
        }
    if (!t) {
        ImGui::TextColored(muted_v4(), "select a tool on the left");
        return;
    }

    /* name + kind tag + live state */
    ImGui::TextUnformatted(t->name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(muted_v4(), "[%s]", kind_str(t->kind));
    ImGui::SameLine();
    ImGui::TextColored(t->enabled ? accent_v4() : muted_v4(), "%s", t->enabled ? "enabled" : "disabled");

    /* enable toggle (a third-party tool is force-disabled while the kill-switch is armed). Enabling a NOT-yet-
     * approved third-party tool routes through a type-to-confirm consent modal first (it runs operator-installed
     * code); a System tool or an already-approved third-party tool toggles directly. */
    const bool thirdparty = t->kind == ToolRow::Kind::ThirdParty;
    const bool killed = s.third_party_tools_disabled && thirdparty;
    bool       en = t->enabled;
    ImGui::BeginDisabled(killed);
    if (ImGui::Checkbox("enabled", &en)) {
        if (en && thirdparty && !t->disclaimed)
            ImGui::OpenPopup("consent_enable_tool"); /* first enable of an unapproved tool: type-to-confirm */
        else
            ctx.commands.push_back(
                {UiCommand::Kind::ToggleTool, t->name, thirdparty ? "thirdparty" : "system", en ? 1 : 0, {}, {}});
    }
    ImGui::EndDisabled();
    if (killed) {
        ImGui::SameLine();
        ImGui::TextColored(muted_v4(), "(disabled by the kill-switch)");
    }
    if (consent_modal("consent_enable_tool",
                      "Enabling this tool runs operator-installed code in a confined sandbox with the permissions "
                      "it declares below. HyperCat has not vetted what it does within those permissions. Enable it "
                      "only if you trust its source.",
                      t->name.c_str(), /*danger=*/true))
        ctx.commands.push_back({UiCommand::Kind::ToggleTool, t->name, "thirdparty", 1, {}, {}});

    ImGui::Spacing();
    PanelHeader("DESCRIPTION");
    WrappedText("%s", t->description.c_str());

    if (t->kind == ToolRow::Kind::System) {
        ImGui::Spacing();
        WrappedTextUnformatted("Always installed — a built-in, security-reviewed System Tool. Toggling it off "
                               "removes it from the toolset of workers spawned afterward; it cannot be uninstalled.");
        return;
    }

    /* third-party: the exposed functions, the declared PERMISSIONS, a loud DISCLAIMER, and Remove. */
    if (!t->spec_json.empty()) {
        ImGui::Spacing();
        PanelHeader("FUNCTIONS");
        WrappedText("%s", t->spec_json.c_str());
    }

    ImGui::Spacing();
    PanelHeader("PERMISSIONS");
    if (ImGui::BeginTable("tool_perms", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch, 0.32f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 0.68f);
        perm_row("filesystem", t->manifest.fs_scopes.empty() ? "none" : join_csv(t->manifest.fs_scopes));
        perm_row("network", t->manifest.egress_hosts.empty()
                                ? "none"
                                : join_csv(t->manifest.egress_hosts) + "  (unrestricted egress)");
        if (!t->manifest.exec_paths.empty()) perm_row("exec", join_csv(t->manifest.exec_paths));
        perm_row("limits", t->manifest.resource_limits);
        perm_row("source", t->manifest.source);
        perm_row("version", t->manifest.version);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    PanelHeader("DISCLAIMER");
    ImGui::PushStyleColor(ImGuiCol_Text, err_v4());
    WrappedTextUnformatted("Operator-installed third-party code. It runs confined to the permissions above, but "
                           "HyperCat does not vet its behaviour, and a network grant is UNRESTRICTED at the kernel "
                           "floor (each network call is gated by you at run time). Remove it if you no longer trust it.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (ImGui::Button("Remove tool")) ImGui::OpenPopup("consent_remove_tool");
    if (consent_modal("consent_remove_tool",
                      "Removing deletes this tool's package from disk and stops it immediately. This cannot be undone.",
                      t->name.c_str(), /*danger=*/true))
        ctx.commands.push_back({UiCommand::Kind::RemoveTool, t->name, {}, 0, {}, {}});
}

} // namespace

void draw_tools_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (!ImGui::Begin("Tools", open)) {
        ImGui::End();
        return;
    }

    static std::string selected; /* the tool name shown in the detail pane */

    PanelHeader("TOOLS");
    {
        bool killed = s.third_party_tools_disabled;
        if (ImGui::Checkbox("disable all third-party tools", &killed))
            ctx.commands.push_back({UiCommand::Kind::DisableAllThirdParty, "", "", killed ? 1 : 0, {}, {}});
        if (s.third_party_tools_disabled) {
            ImGui::SameLine();
            ImGui::TextColored(err_v4(), "(armed)");
        }
        /* D4c (opt-in, default OFF): let the conductor — the front-door chat agent — call third-party tools too.
         * Workers always get them by role; this exposes them to the conductor itself. Applies on the next chat
         * (New Chat / Resume / project switch); every sensitive call is still operator-gated. */
        bool cond_tp = s.third_party_conductor;
        if (ImGui::Checkbox("conductor may use third-party tools (opt-in)", &cond_tp))
            ctx.commands.push_back({UiCommand::Kind::SetConductorThirdParty, "", "", cond_tp ? 1 : 0, {}, {}});
    }

    if (ImGui::BeginTable("tools_split", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch, 0.66f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("tools_tree")) draw_tools_tree(s, selected);
        ImGui::EndChild();
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("tools_detail")) draw_tool_detail(s, ctx, selected);
        ImGui::EndChild();
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace hc::ui
