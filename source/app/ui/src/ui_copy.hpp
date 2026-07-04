#ifndef HC_UI_COPY_HPP
#define HC_UI_COPY_HPP

/* ui_copy — make the read-only TEXT VIEWS copyable. The conversation/observability panels (conductor chat,
 * console token-stream, reasoning chain, transcript, log, terminal) render through ImGui's INERT
 * TextUnformatted / markdown, so until now their text could not be selected or lifted onto the clipboard.
 * This adds the missing affordance via the project's EXISTING right-click context-menu pattern (cf. the
 * Files panel "Copy path") + ImGui::SetClipboardText — the same system clipboard the input boxes already
 * paste from with Ctrl+V (GLFW-backed). Pure UI: no host round-trip, no snapshot mutation, model/worker text
 * travels as literal bytes (never a format string).
 *
 * Threading: UI/GL thread only (touches the live ImGui context + clipboard). Lifetime: free functions over
 * caller-owned strings, consumed only during the call.
 */

#include <string>

namespace hc::ui {

/* Attach a right-click "Copy" menu to the item OR BeginGroup/EndGroup BLOCK just submitted: right-clicking
 * that block offers "Copy" (this block's `block` text) and "Copy all" (`all` — the whole panel's text). `id`
 * must be unique within the live ID stack (PushID a per-message index around repeated blocks). Renders
 * nothing until right-clicked; the hover/click test uses the last item's rect, so call it immediately after
 * the block (after EndGroup for a multi-widget message). */
void copy_block_menu(const char *id, const std::string &block, const std::string &all);

/* A window/child-level right-click "Copy all" menu for `all` (the whole panel's text). `only_empty_space`
 * true => opens ONLY over non-item space, so it composes with per-block menus on a discrete-message panel
 * (the block menu wins over a message, this catches the gaps); false (default) => opens anywhere in the
 * region, for single-render panels (log/terminal/reasoning) that have no per-block menus. `id` must be
 * unique within the enclosing Begin/End window (the popup is namespaced to that window — each panel owns its
 * own window, so the per-panel literals at the call sites don't collide). */
void copy_region_menu(const char *id, const std::string &all, bool only_empty_space = false);

/* A small always-visible "copy" button that lifts `text` onto the system clipboard when clicked — the per-code-
 * block affordance in rendered markdown (chat + the Viewer). Submits one SmallButton at the current cursor (the
 * caller positions it); `id` must be unique in the live ID stack (the markdown renderer PushID's the block index).
 * `text` travels as literal clipboard bytes (never a format string). Returns true on the click. */
bool copy_button(const char *id, const std::string &text);

} // namespace hc::ui

#endif /* HC_UI_COPY_HPP */
