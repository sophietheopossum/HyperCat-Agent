#ifndef HC_UI_THEME_HPP
#define HC_UI_THEME_HPP

/* ui_theme — the restrained-corporate theme (doc 10): the house palette translated to ImGuiStyle.
 * Near-black flat panels, 1px borders everywhere, no rounding, no fills; a single accent drives
 * ONLY the primary slots (check mark, slider, selected-tab overline, docking preview). No hazard
 * stripes, angular brackets, glow, or neon. INTERNAL to hc_ui.
 * Threading: single-threaded (operates on the current ImGui context). Lifetime: free functions; call
 * after an ImGui context exists and before the first frame (fonts). Exception: ONE process-local
 * write-once singleton — `g_bold`, the Bold font face, set by load_fonts before the first frame (null
 * until then) — justified because ImGui's font atlas owns a single font pointer valid for the process.
 */

#include "hc_ui.hpp" /* hc::ui::Accent is now public (so UiApp::set_accent can take it) */

#include "imgui.h" /* ImVec4 — the promoted accent readers return it */

#include <string> /* fit_ellipsis / EllipsisText operate on std::string */

namespace hc::ui {

/* Apply the theme colors + flat/dense style values to the current ImGui context. */
void apply_theme(Accent accent);

/* Load the UI font once, BEFORE the first frame: JetBrains Mono if present, else a system mono
 * fallback (DejaVu), else ImGui's built-in default. Also loads a Bold face when present (for markdown
 * headings/bold). Returns the (regular) family name actually loaded. */
const char *load_fonts();

/* The bold font face loaded by load_fonts, or null when no Bold TTF was found (the markdown renderer then
 * falls back to the regular face — an honest fidelity limit, never a crash). UI thread only. */
ImFont *bold_font();

/* A restrained section header: a small muted uppercase label + a 1px rule (no accent square — the
 * single accent stays on functional widgets only). */
void PanelHeader(const char *label);

/* The shared restrained tints read from the live style (functional accent / dim error red / muted) + a
 * "label / value" stat cell — promoted here so every panel TU shares ONE source (de-duplicated, and
 * reachable once ui_panels.cpp splits into per-cluster TUs). */
ImVec4 accent_v4();
ImVec4 muted_v4();
ImVec4 err_v4();
void   stat_cell(const char *label, const char *value, bool accent = false);

/* Wrap text to the live COLUMN right edge (GetCursorPosX + content-avail), NOT the window edge — so text
 * inside a column / after SameLine / in a narrow child reflows instead of overflowing until a resize. Use
 * WrappedTextUnformatted (or WrappedText("%s", ...)) for untrusted model/worker text. */
void WrappedTextUnformatted(const char *s);
void WrappedText(const char *fmt, ...);

/* Single-line overflow control (the restrained "no horizontal overflow" rule). fit_ellipsis truncates `text`
 * to `max_px` with a trailing ".." on a UTF-8 boundary (the primitive). EllipsisText renders `text` on one
 * line ellipsized to the current available content width (e.g. a table cell), with a hover tooltip showing
 * the FULL value when it was truncated — for single-line identity (ids, paths, names) so a long value never
 * forces the panel/column wider. Untrusted-safe (TextUnformatted; no format string). */
std::string fit_ellipsis(const std::string &text, float max_px);
void        EllipsisText(const std::string &text);

} // namespace hc::ui

#endif /* HC_UI_THEME_HPP */
