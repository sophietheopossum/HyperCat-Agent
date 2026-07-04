#ifndef HC_UI_MASCOT_HPP
#define HC_UI_MASCOT_HPP

/* ui_mascot — the opt-in HyperCat mascot (WI-5). INTERNAL to hc_ui. One consumer of the generic ui_sprite
 * engine: it maps fleet state to the bundled mascot art and renders a small dockable "Mascot" window.
 * Default-OFF (the operator opts in from View -> Panels; the settings module persists the toggle). Kept
 * separate from ui_sprite so the engine stays free of HyperCat specifics.
 *
 * Owns:      a SpritePlayer (a playhead — no GPU state). The sheets are BORROWED from the UiApp-owned
 *            SpriteRegistry (which owns the textures + enforces the backend-before-sprites teardown), so a
 *            MascotView owns no textures and imposes no teardown ordering of its own.
 * Threading: single-threaded — UI/GL thread only. A missing/failed sheet degrades to a text fallback, never
 *            a crash. */

#include "hc_ui.hpp"     /* UiSnapshot */
#include "ui_sprite.hpp" /* SpritePlayer */

namespace hc::ui {

class SpriteRegistry; /* the UiApp-owned sheet catalog the mascot borrows from */

/* The three coarse moods the fleet snapshot maps to. */
enum class MascotState { Idle, Working, Error };

/* PURE: classify a snapshot into a mascot mood, precedence Error > Working > Idle. No ImGui — offline-testable. */
MascotState mascot_state_for(const UiSnapshot &s);

/* The mascot window + its animation state. See the file banner for the ownership contract. */
class MascotView {
public:
    MascotView() = default;
    ~MascotView() = default;

    MascotView(const MascotView &) = delete;
    MascotView &operator=(const MascotView &) = delete;

    /* Draw the mascot window for this frame from `s`, borrowing its sheets from `sprites` (which the host has
     * ensured loaded). `*open` follows the window's close box (the host's ctx.show.mascot); a no-op if !*open. */
    void draw(const UiSnapshot &s, const SpriteRegistry &sprites, bool *open);

private:
    void set_state_anim(const SpriteRegistry &sprites); /* (re)point the player at the animation for state_ */

    bool         primed_ = false; /* false until the first state is applied */
    SpritePlayer player_;
    MascotState  state_ = MascotState::Idle;
};

} // namespace hc::ui

#endif /* HC_UI_MASCOT_HPP */
