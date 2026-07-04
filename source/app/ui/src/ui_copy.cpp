/* ui_copy — right-click "Copy" menus for the read-only text views (see ui_copy.hpp). INTERNAL. */

#include "ui_copy.hpp"

#include "imgui.h"

namespace hc::ui {

void copy_block_menu(const char *id, const std::string &block, const std::string &all)
{
    /* BeginPopupContextItem keys the popup to the LAST submitted item (after EndGroup: the whole message
     * group's rect) on a right-mouse release. Passing an explicit `id` is REQUIRED here: a text/group item
     * carries no widget ID of its own, so the popup needs a name (uniqueness comes from the caller's PushID). */
    if (ImGui::BeginPopupContextItem(id)) {
        if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(block.c_str());
        if (ImGui::MenuItem("Copy all")) ImGui::SetClipboardText(all.c_str());
        ImGui::EndPopup();
    }
}

void copy_region_menu(const char *id, const std::string &all, bool only_empty_space)
{
    ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonRight;
    /* NoOpenOverItems lets this whole-panel menu coexist with per-block menus: a right-click over a message
     * opens that message's menu, this one only fills the gaps. Single-render panels (log/terminal/reasoning)
     * pass false so a right-click anywhere — including over the text — still offers "Copy all". */
    if (only_empty_space) flags |= ImGuiPopupFlags_NoOpenOverItems;
    if (ImGui::BeginPopupContextWindow(id, flags)) {
        if (ImGui::MenuItem("Copy all")) ImGui::SetClipboardText(all.c_str());
        ImGui::EndPopup();
    }
}

bool copy_button(const char *id, const std::string &text)
{
    /* A namespaced SmallButton with a fixed visible label ("copy"); `id` keeps it unique under the caller's PushID
     * so sibling code blocks don't collide. The code travels as literal bytes — never a format string. */
    ImGui::PushID(id);
    const bool clicked = ImGui::SmallButton("copy");
    if (clicked) ImGui::SetClipboardText(text.c_str());
    ImGui::PopID();
    return clicked;
}

} // namespace hc::ui
