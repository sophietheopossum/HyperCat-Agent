/* ui_toasts — B2: the transient notification stack (see hc_ui.hpp `Toast`). A NON-BLOCKING overlay pinned to the
 * bottom-right corner; an ApprovalPending toast is clickable and opens + fronts the Approvals panel so a patient
 * approval is unmissable. A PURE renderer of the snapshot's `toasts` — the host owns their lifecycle (an approval
 * toast mirrors its pending request; it appears with the prompt and self-clears when resolved/dismissed). The click
 * only flips UI-local panel state (show + focus), never a host command. UI/GL thread only. */

#include "ui_panels.hpp" /* DrawCtx, draw_toasts decl */
#include "ui_theme.hpp"  /* accent_v4 / muted_v4 / err_v4 */

#include "imgui.h"

namespace hc::ui {

void draw_toasts(const UiSnapshot &s, DrawCtx &ctx)
{
    if (s.toasts.empty()) return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float          pad = 12.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, vp->WorkPos.y + vp->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f)); /* bottom-right pivot */
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##toasts", nullptr, flags)) {
        constexpr std::size_t kMax = 6; /* show only the most recent few — the rest are in the panels */
        const std::size_t     start = s.toasts.size() > kMax ? s.toasts.size() - kMax : 0;
        for (std::size_t i = start; i < s.toasts.size(); i++) {
            const Toast &t = s.toasts[i];
            ImGui::PushID(static_cast<int>(i)); /* stable id so untrusted text can't collide / "##"-escape the id */
            const ImVec4 col = t.kind == Toast::Kind::ApprovalPending ? accent_v4()
                               : t.kind == Toast::Kind::Warning       ? err_v4()
                                                                      : muted_v4();
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (t.kind == Toast::Kind::ApprovalPending) {
                /* clickable -> reopen + front the Approvals panel (the diff + Allow/Deny/Dismiss live there). Pure
                 * UI state; focus_window is applied + cleared by draw_all this same frame. The label text is
                 * host-built (agent + tool), so it carries no model-controlled "##". */
                if (ImGui::Selectable(t.text.c_str())) {
                    ctx.show.approvals = true;
                    ctx.focus_window = "Approvals";
                }
            } else {
                ImGui::TextUnformatted(t.text.c_str()); /* untrusted -> never a format string */
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace hc::ui
