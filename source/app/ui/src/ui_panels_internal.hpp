#ifndef HC_UI_PANELS_INTERNAL_HPP
#define HC_UI_PANELS_INTERNAL_HPP

/* ui_panels_internal — the PRIVATE cross-TU surface for the split ui_panels implementation. NOT public:
 * it is not on hc_ui's public include path; it only lets the per-cluster ui_panels_*.cpp translation
 * units share the handful of file-local helpers that more than one cluster needs, without duplicating a
 * definition (ODR). Helpers used by a SINGLE cluster stay in that TU's anonymous namespace and are not
 * declared here.
 *   - state_color: declared here, DEFINED once in ui_panels_common.cpp (non-anonymous hc::ui, external
 *     linkage within the library) — it reads the live ImGui style, so it is an out-of-line function.
 *   - k_accents: the six restrained accents are the same source for the View->Accent menu (common) and
 *     the Settings accent picker (settings). Defined here as `inline constexpr` so BOTH TUs see the
 *     complete array (its bound makes the existing range-based-for loops compile unchanged) with a single
 *     shared definition (inline => no ODR violation).
 * Threading / lifetime follow the public ui_panels.hpp contract (UI/GL thread only, free functions over
 * the read-only snapshot + the UiApp-owned DrawCtx). */

#include "hc_ui.hpp" /* hc::ui::Accent — AccentOpt carries it */

#include "imgui.h" /* ImVec4 — state_color returns it */

#include <string>

namespace hc::ui {

/* A status color for an agent/task state string (accent active / dim red dead-failed / muted pending /
 * default done). Shared by the work-cluster panels (fleet, dag, tasks, task detail). Defined in
 * ui_panels_common.cpp. */
ImVec4 state_color(const std::string &st);

/* Render a unified diff (the P11 hunk view: @@ anchors + coloured +/-/context lines, untrusted text via
 * "%s"). DEFINED in ui_panels_review.cpp; SHARED with the IDE editor's change-on-disk diff (W5 P5.2). */
void draw_pending_diff(const PendingDiff &d);

/* An InputTextMultiline backed directly by a std::string (no imgui_stdlib in-tree): a resize callback grows
 * the string as the user types, so the text is not capped by a fixed buffer (the host bounds it on save).
 * `str` must stay alive for the call. Returns true on an edit. DEFINED in ui_panels_common.cpp; SHARED by the
 * Roles editor (P2.3b) + the IDE editor's edit mode (W5 P5.3). */
bool input_multiline_str(const char *label, std::string &str, const ImVec2 &size);

/* B4: a reusable TYPE-TO-CONFIRM consent gate for ARMING a dangerous capability (the first such primitive). The
 * caller OpenPopup(id)s on the trigger and calls this every frame; it renders the modal, shows `disclaimer`
 * (red when `danger`), and enables "Confirm" ONLY when the operator types `phrase` exactly — never a mere
 * checkbox. Returns true EXACTLY ONCE, on a confirmed match (then closes the popup). Reuse for any future footgun.
 * DEFINED in ui_panels_common.cpp. */
bool consent_modal(const char *id, const char *disclaimer, const char *phrase, bool danger);

/* The six restrained accents, shared by the View->Accent menu + the Settings picker (one source). */
struct AccentOpt {
    const char *name;
    Accent      val;
};
inline constexpr AccentOpt k_accents[] = {{"white", Accent::White},   {"cyan", Accent::Cyan},
                                          {"amber", Accent::Amber},    {"emerald", Accent::Emerald},
                                          {"violet", Accent::Violet},  {"crimson", Accent::Crimson}};

} // namespace hc::ui

#endif /* HC_UI_PANELS_INTERNAL_HPP */
