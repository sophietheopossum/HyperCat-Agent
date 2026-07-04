#ifndef HC_UI_EDITOR_HPP
#define HC_UI_EDITOR_HPP

/* ui_editor — the read path of the basic IDE (W5 P5.1): a line-numbered, syntax-highlighted source view of
 * the operator-opened file. INTERNAL to hc_ui.
 *
 * Holds a per-line model (the line-vector + per-line tokens) re-lexed ONLY when the opened (path, content
 * hash) changes — so the clipper-driven render is cheap (no per-frame re-lex). The gutter is a custom
 * ImDrawList draw, horizontally PINNED (it does not scroll off when a long line scrolls right); syntax colour
 * comes from the pure text_lexer mapped to a restrained on-theme palette. The Editor also holds the EDIT-mode
 * buffer (P5.3): the panel renders the input over edit_mutable() + emits SaveFile with edit_buffer(); reconcile
 * syncs the buffer when the displayed content changes under an active edit (the live-diff banner is P5.2).
 *
 * Owns:      the cached line model (plain std::strings + token vectors — no GPU/ImGui resources, freed by the
 *            members). One Editor per editor surface, owned by UiApp::Impl.
 * Lifetime:  `bytes`/`path` are READ during draw() only (not retained past the call); the Editor outlives
 *            every draw() (a UiApp::Impl member). The cached model persists across frames until the next
 *            (path, hash) change re-lexes it.
 * Threading: UI thread only (draw() touches the live ImGui context). */

#include "text_lexer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hc::ui {

class Editor {
public:
    /* Render the file `path` (its extension picks the language) with content `bytes`(len) as a line-numbered,
     * syntax-highlighted, read-only code view inside the CURRENT window. `hash` is the content fingerprint
     * (the snapshot open_hash): a change (or a new path) re-lexes; an unchanged hash reuses the cached model. */
    void draw(const char *path, const char *bytes, std::size_t len, std::uint64_t hash);

    /* --- edit mode (W5 P5.3) — the buffer + dirty state; the panel renders the input widget over edit_mutable()
     * and emits SaveFile with edit_buffer(). The Editor owns no file I/O — the host does the jailed write. --- */
    bool                editing() const { return editing_; }
    bool                dirty() const { return edit_buf_ != edit_base_; } /* unsaved changes */
    const std::string  &edit_buffer() const { return edit_buf_; }         /* the content to save */
    std::string        &edit_mutable() { return edit_buf_; }              /* bound to the InputTextMultiline widget */

    /* Enter edit mode, seeding the editable buffer (and the dirty baseline) from `bytes` at content `hash`. */
    void begin_edit(const char *bytes, std::size_t len, std::uint64_t hash);
    void end_edit() { editing_ = false; }

    /* Reconcile the buffer when the DISPLAYED content changes under an active edit (the snapshot hash moved):
     * a save round-trip (disk == buffer) just re-baselines dirty; a genuine external change / reload (disk !=
     * buffer) ADOPTS the disk version (discards local edits). Call once per frame before rendering the input. */
    void reconcile(const char *bytes, std::size_t len, std::uint64_t hash);

private:
    /* Rebuild the line model + tokens. Requires a LIVE ImGui frame (it calls CalcTextSize to size the
     * horizontal extent), so it is only ever called from draw() — never off the UI thread / outside a frame. */
    void reload(const char *path, const char *bytes, std::size_t len, std::uint64_t hash);

    std::uint64_t                   loaded_hash_ = 0;
    bool                            loaded_ = false;
    std::string                     path_;
    std::vector<std::string>        lines_;       /* the line-vector model (per-line addressing)        */
    std::vector<std::vector<Token>> line_tokens_; /* tokens per line, parallel to lines_                */
    float                           max_px_ = 0.0f;   /* widest line in pixels (the horizontal extent)  */
    bool                            truncated_ = false; /* file exceeded the editor's line ceiling      */

    /* edit mode (P5.3) */
    bool                            editing_ = false;
    std::string                     edit_buf_;          /* the editable copy (bound to the input widget)  */
    std::string                     edit_base_;         /* the dirty baseline (== edit_buf_ when clean)   */
    std::uint64_t                   edit_seed_hash_ = 0; /* the content hash the buffer was last synced to */
};

} // namespace hc::ui

#endif /* HC_UI_EDITOR_HPP */
