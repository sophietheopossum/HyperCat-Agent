/* ui_mascot — implementation of the opt-in HyperCat mascot (see ui_mascot.hpp). INTERNAL.
 *
 * v1 maps three moods to the bundled art (white-on-transparent line work, so it reads on the near-black
 * panels): a calm idle face, a 6-frame "working" loop, and the same face dim-red-tinted for "attention" (an
 * honest fidelity limit — no dedicated error art yet). The sheets are borrowed from the SpriteRegistry, which
 * owns the textures (decoded from trusted, app-bundled QOI) and enforces the teardown order. */

#include "ui_mascot.hpp"

#include "ui_sprite_registry.hpp" /* SpriteRegistry — the borrowed sheet catalog */
#include "ui_theme.hpp"           /* err_v4 / muted_v4 — the restrained tints */

#include "imgui.h"

namespace hc::ui {

MascotState mascot_state_for(const UiSnapshot &s)
{
    for (const auto &a : s.agents)
        if (a.state == "dead") return MascotState::Error;
    for (const auto &t : s.tasks)
        if (t.state == "failed") return MascotState::Error;
    for (const auto &t : s.tasks)
        if (t.state == "running" || t.state == "assigned") return MascotState::Working;
    return MascotState::Idle;
}

void MascotView::set_state_anim(const SpriteRegistry &sprites)
{
    /* Working = the looped think strip; Idle/Error = the single held face frame. The registry supplies each
     * sheet's manifest-default animation (mascot.think = a ~9 fps loop, mascot.face = a still). */
    player_.set_animation(state_ == MascotState::Working ? sprites.animation("mascot.think")
                                                         : sprites.animation("mascot.face"));
}

void MascotView::draw(const UiSnapshot &s, const SpriteRegistry &sprites, bool *open)
{
    if (!open || !*open) return;

    ImGui::SetNextWindowSize(ImVec2(168, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mascot", open)) {
        ImGui::End();
        return;
    }

    /* Re-point the player only when the mood actually changes (or on the first frame). */
    const MascotState want = mascot_state_for(s);
    if (!primed_ || want != state_) {
        primed_ = true;
        state_ = want;
        set_state_anim(sprites);
    }
    player_.advance(ImGui::GetIO().DeltaTime * 1000.0f);

    const SpriteSheet *sheet =
        sprites.sheet(state_ == MascotState::Working ? "mascot.think" : "mascot.face");

    ImU32 tint = IM_COL32_WHITE;
    if (state_ == MascotState::Error) {
        const ImVec4 e = err_v4();
        tint = ImGui::ColorConvertFloat4ToU32(ImVec4(e.x, e.y, e.z, 1.0f)); /* dim-red "attention" */
    }

    /* Center a native-size 128px sprite (crisp NEAREST). A missing/failed sheet -> a muted text line. */
    const float box = 128.0f;
    if (sheet) {
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > box) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - box) * 0.5f);
        player_.draw(*sheet, ImVec2(box, box), tint);
    } else {
        ImGui::TextColored(muted_v4(), "mascot art unavailable");
    }

    const char *label = state_ == MascotState::Working ? "working"
                        : state_ == MascotState::Error ? "attention"
                                                       : "idle";
    ImGui::TextColored(state_ == MascotState::Error ? err_v4() : muted_v4(), "HyperCat \xE2\x80\x94 %s",
                       label); /* em-dash */

    ImGui::End();
}

} // namespace hc::ui
