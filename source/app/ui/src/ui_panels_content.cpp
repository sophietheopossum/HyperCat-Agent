/* ui_panels (content cluster) — the file-CONTENT panels: the interactive workspace file browser (P4.1), the
 * file/media Viewer (image/markdown/text/PDF, P4.2), and the IDE Editor (gutter + syntax + the live-diff and
 * edit/save, P5.1–P5.3). Split out of ui_panels_io.cpp when it crossed the ~500 LOC smell-test — these all
 * render the operator-OPENED file (the OpenFile/open_* snapshot seam) + emit the file UiCommands, a single
 * cohesive responsibility distinct from the io cluster's observability/stream panels. See ui_panels.hpp for
 * the full contract; helpers shared across clusters live in ui_panels_internal.hpp. */

#include "ui_panels.hpp"
#include "ui_panels_internal.hpp" /* draw_pending_diff, input_multiline_str — shared with review/roles */
#include "ui_docview.hpp"         /* P4.2: classify the opened file for the Viewer */
#include "ui_editor.hpp"          /* P5.1: the IDE gutter + syntax + edit view */
#include "ui_texcache.hpp"        /* P4.2: decode + cache the opened image as a GPU texture */

#include "ui_markdown.hpp"
#include "ui_theme.hpp"

#include "imgui.h"

#include <cfloat>
#include <cstdio>
#include <string>

namespace hc::ui {

/* The interactive workspace file browser (draw_files_panel) lives in its own TU, ui_panels_files.cpp — it grew
 * into a desktop-style split explorer (folder tree + sortable contents + context menus) distinct from this
 * cluster's read-only content viewer/editor. See ui_panels.hpp for the shared declaration. */

/* W4 P4.2: render an opened IMAGE — decode (cached) -> a backend texture -> fit-within-region ImGui::Image. The
 * cache is content-keyed on s.open_hash, so the decode happens once per opened file, not per frame; a decode
 * failure (corrupt, or truncated past the host read cap) is cached too and shows an honest note. */
static void draw_image(const UiSnapshot &s, OperatorTextureCache &tex, DocKind k)
{
    const ImageCodec codec = (k == DocKind::ImageQoi) ? ImageCodec::Qoi : ImageCodec::Auto;
    const OperatorTexture *t =
        tex.get(s.open_hash, (const unsigned char *)s.open_bytes->data(), s.open_bytes->size(), codec);
    if (!t) {
        ImGui::TextColored(muted_v4(),
                           "[%s · %ld B] — could not decode (corrupt, or larger than the in-app preview cap)",
                           doc_kind_label(k), s.open_size);
        return;
    }
    ImGui::TextColored(muted_v4(), "%d x %d", t->w, t->h);
    /* fit fully WITHIN the remaining region, preserving aspect (letterboxed); upscale small images so a tiny
     * icon is visible, downscale large photos so they don't overflow. */
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    float        sx = (t->w > 0) ? avail.x / (float)t->w : 1.0f;
    float        sy = (t->h > 0) ? avail.y / (float)t->h : 1.0f;
    float        scale = (sx < sy) ? sx : sy;
    if (!(scale > 0.0f)) scale = 1.0f; /* degenerate region -> native size */
    ImGui::Image(t->tex_ref, ImVec2((float)t->w * scale, (float)t->h * scale));
}

/* W4 P4.2: the Viewer — render the file the operator opened in the browser. Classifies the bytes via ui_docview
 * (magic-byte tiebreak) and dispatches: markdown -> the markdown renderer; text/code -> wrapped untrusted text
 * ("%s", never interpreted); image -> decode + GPU texture via the OperatorTextureCache; PDF -> "preview not
 * available"; binary -> a size note. Also backs the file-OPEN view. Pure renderer (the cache is UiApp-owned). */
void draw_viewer_panel(const UiSnapshot &s, OperatorTextureCache &tex, bool *open)
{
    if (!ImGui::Begin("Viewer", open)) {
        ImGui::End();
        return;
    }
    if (s.open_path.empty() || !s.open_bytes) {
        ImGui::TextColored(muted_v4(), "no file open — click 'view' on a file in the Files panel");
        ImGui::End();
        return;
    }
    DocKind k = classify_doc(s.open_path, (const unsigned char *)s.open_bytes->data(), s.open_bytes->size());
    ImGui::TextUnformatted(s.open_path.c_str());
    ImGui::SameLine();
    ImGui::TextColored(muted_v4(), "  %s · %ld B", doc_kind_label(k), s.open_size);
    ImGui::Separator();
    switch (k) {
    case DocKind::Markdown:
        render_markdown_cached(*s.open_bytes); /* WI-4: the bounded markdown renderer */
        break;
    case DocKind::PlainText:
        WrappedText("%s", s.open_bytes->c_str()); /* untrusted text — "%s" never interprets it */
        break;
    case DocKind::ImagePng:
    case DocKind::ImageJpeg:
    case DocKind::ImageQoi:
        draw_image(s, tex, k);
        break;
    case DocKind::Pdf:
        ImGui::TextColored(muted_v4(), "[PDF · %ld B] — preview not available in-app; open it in a system viewer",
                           s.open_size);
        break;
    case DocKind::Audio:
        ImGui::TextColored(muted_v4(), "[audio · %ld B] — open it in the Music Player to play it", s.open_size);
        break;
    case DocKind::Binary:
        ImGui::TextColored(muted_v4(), "[binary · %ld B] — no preview", s.open_size);
        break;
    }
    ImGui::End();
}

/* W5 P5.1/P5.2: the IDE editor view — the opened file as line-numbered, syntax-highlighted source, plus the
 * live-diff-on-external-change overlay. Borrows the UiApp-owned Editor (the cached line model), reads the same
 * OpenFile seam as the Viewer (open_path/bytes/hash) + the host's change-watch (open_disk_changed/open_diff).
 * When an agent rewrites the open file, a "changed on disk" banner appears with [Reload] (re-opens, reusing
 * the OpenFile command) + a [View diff] toggle that renders the reused P11 diff. (Edit + Save = P5.3.) */
void draw_editor_panel(const UiSnapshot &s, Editor &ed, DrawCtx &ctx, bool *open)
{
    if (!ImGui::Begin("Editor", open)) {
        ImGui::End();
        return;
    }
    if (s.open_path.empty() || !s.open_bytes) {
        ImGui::TextColored(muted_v4(), "no file open — click 'view' on a file in the Files panel");
        ImGui::End();
        return;
    }
    /* ImGui in-place state (single UI thread; the established panel idiom — cf. draw_files_panel's statics).
     * SAFE because draw_editor_panel is called at most ONCE per frame from draw_all; if a second editor panel
     * is ever added, migrate these into DrawCtx. `show_code` flips the body from the default diff view back to
     * the code; `was_changed` edge-detects a NEW external change; `last_path` exits edit mode on a file switch. */
    static bool        show_code = false;
    static bool        was_changed = false;
    static std::string last_path;
    const bool         have_diff = s.open_disk_changed && !s.open_diff.too_large && !s.open_diff.hunks.empty();
    if (s.open_disk_changed && !was_changed) show_code = false; /* a fresh change -> show the diff */
    was_changed = s.open_disk_changed;
    if (s.open_path != last_path) { /* a DIFFERENT file opened -> leave edit mode (a same-path save/reload is */
        ed.end_edit();              /* reconciled, not exited, by Editor::reconcile below)                    */
        last_path = s.open_path;
    }

    /* DATA-LOSS GUARD (W5 P5.3): the host caps the read, so a file larger than what was loaded is shown
     * truncated — editing + saving it would DESTROY the unseen tail. Block editing such a file outright. */
    const bool truncated = (uint64_t)s.open_size > s.open_bytes->size();
    if (truncated) ed.end_edit();

    ImGui::TextUnformatted(s.open_path.c_str());
    ImGui::SameLine();
    ImGui::TextColored(muted_v4(), "  %ld B", s.open_size);

    /* W5 P5.3: the edit/save controls. Operator-direct (the human-in-the-loop): "save" emits the jailed atomic
     * write. Save is disabled until dirty; the unseen-tail case is blocked above. */
    if (truncated) {
        ImGui::SameLine();
        ImGui::TextColored(muted_v4(), "  (showing the first %zuKiB — too large to edit)", s.open_bytes->size() / 1024);
    } else {
        ImGui::SameLine();
        if (ImGui::SmallButton(ed.editing() ? "done" : "edit")) {
            if (ed.editing()) ed.end_edit();
            else ed.begin_edit(s.open_bytes->data(), s.open_bytes->size(), s.open_hash);
        }
        if (ed.editing()) {
            const bool dirty = ed.dirty();
            ImGui::SameLine();
            ImGui::BeginDisabled(!dirty);
            if (ImGui::SmallButton("save"))
                ctx.commands.push_back({UiCommand::Kind::SaveFile, s.open_path, ed.edit_buffer(), 0, {}});
            ImGui::EndDisabled();
            if (dirty) {
                ImGui::SameLine();
                ImGui::TextColored(accent_v4(), "unsaved");
            }
        }
    }

    /* W5 P5.2: the change-on-disk banner (also while editing — an agent may rewrite the file under you; keep
     * your edits, or Reload to adopt the disk version and discard them). */
    if (s.open_disk_changed) {
        ImGui::SameLine();
        ImGui::TextColored(accent_v4(), "  • changed on disk");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload"))
            ctx.commands.push_back({UiCommand::Kind::OpenFile, s.open_path, "", 0, {}});
        if (!ed.editing() && have_diff) {
            ImGui::SameLine();
            ImGui::Checkbox("show code", &show_code);
        }
    }
    ImGui::Separator();

    if (ed.editing()) {
        ed.reconcile(s.open_bytes->data(), s.open_bytes->size(), s.open_hash); /* sync on save/reload */
        input_multiline_str("##editbuf", ed.edit_mutable(), ImVec2(-FLT_MIN, -FLT_MIN));
    } else if (have_diff && !show_code) {
        draw_pending_diff(s.open_diff); /* the reused P11 hunk view (displayed -> disk) */
    } else {
        ed.draw(s.open_path.c_str(), s.open_bytes->data(), s.open_bytes->size(), s.open_hash);
    }
    ImGui::End();
}

} // namespace hc::ui
