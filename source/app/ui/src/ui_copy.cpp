/* ui_copy — right-click "Copy" menus for the read-only text views (see ui_copy.hpp). INTERNAL. */

#include "ui_copy.hpp"

#include "imgui.h"
#include "imgui_internal.h" /* GetInputTextState — the live selection is not in the public API */

namespace hc::ui {

void copy_block_menu(const char *id, const std::string &block, const std::string &all,
                     const char *block_label)
{
    /* BeginPopupContextItem keys the popup to the LAST submitted item (after EndGroup: the whole message
     * group's rect) on a right-mouse release. Passing an explicit `id` is REQUIRED here: a text/group item
     * carries no widget ID of its own, so the popup needs a name (uniqueness comes from the caller's PushID). */
    if (ImGui::BeginPopupContextItem(id)) {
        if (ImGui::MenuItem(block_label)) ImGui::SetClipboardText(block.c_str());
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

std::string selectable_text(const char *id, const std::string &text)
{
    const float wrap = ImGui::GetContentRegionAvail().x;
    if (wrap <= 1.0f || text.empty()) { /* nothing sensible to lay out */
        ImGui::TextUnformatted(text.c_str());
        return {};
    }
    /* Measure the WRAPPED height, then give it a couple of pixels of slack. Erring generous is deliberate
     * and asymmetric: a box one pixel too SHORT becomes scrollable, and a scrollable child swallows the
     * mouse wheel so the chat stops scrolling under the cursor. Too tall costs a hairline of space. */
    const ImVec2 sz = ImGui::CalcTextSize(text.c_str(), text.c_str() + text.size(), false, wrap);
    const float  h = sz.y + 2.0f;

    /* Off-screen turns get a spacer of the SAME height instead of the widget. InputTextMultiline is a
     * child window -- its own layout, clip rect and ID scope -- and this panel has no list clipper, so it
     * walks every turn every frame; in a long conversation that is a child window per turn per frame for
     * text nobody is looking at. The height is the one we just measured and FramePadding is zeroed below,
     * so the item occupies exactly what the real widget would and the scrollbar does not move. Selection
     * is not lost by this: you cannot select what is not on screen, and scrolling it back into view
     * restores the real widget before it can be clicked. */
    if (!ImGui::IsRectVisible(ImVec2(wrap, h))) {
        ImGui::Dummy(ImVec2(wrap, h));
        return {};
    }

    /* Dress the edit box down until it reads as plain text. */
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    /* const_cast is sound ONLY because of ImGuiInputTextFlags_ReadOnly — see the header note. */
    ImGui::InputTextMultiline(id, const_cast<char *>(text.c_str()), text.size() + 1, ImVec2(wrap, h),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    /* Remember the selection while we still can. A right-click deactivates the input and clears the live
     * selection BEFORE any context menu opens, so reading it at menu time always finds nothing -- which is
     * why "Copy" used to silently fall back to the whole message. Offsets are stored rather than the text:
     * state->TextSrc is documented as null outside the InputText() call for read-only fields.
     *
     * ONE slot, not one per message, and that is the correctness argument rather than a saving. ImGui has
     * a single active input at a time, so a single selection is all that can exist; per-message storage
     * could hold several at once and did -- select in one turn, click into another, right-click the first,
     * and it copied an invisible selection the reader had long since moved on from. Whoever selects last
     * owns the slot; every other message reports nothing and its menu correctly offers the whole turn. */
    static ImGuiID s_owner = 0;
    static int     s_a = 0, s_b = 0;
    const ImGuiID  self = ImGui::GetItemID();
    if (ImGuiInputTextState *sti = ImGui::GetInputTextState(self)) {
        if (sti->HasSelection()) {
            s_owner = self;
            s_a = sti->GetSelectionStart();
            s_b = sti->GetSelectionEnd();
        } else if (s_owner == self) {
            s_owner = 0; /* deselected in place — the menu goes back to offering the whole turn */
        }
    }
    if (s_owner != self) return {};
    int a = s_a, b = s_b;
    if (a > b) { const int t = a; a = b; b = t; } /* stb reports the anchor first when dragging backwards */
    if (a < 0 || b > (int)text.size() || b <= a) return {};
    return text.substr((std::size_t)a, (std::size_t)(b - a));
}

} // namespace hc::ui
