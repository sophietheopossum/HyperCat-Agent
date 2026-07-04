/* ui_editor — implementation of the IDE read view (see ui_editor.hpp). INTERNAL.
 * The render: a vertically+horizontally scrollable child, an ImGuiListClipper over the (up to the ceiling)
 * lines, and per visible line a custom ImDrawList draw — a horizontally-PINNED gutter (a band + a
 * right-aligned muted line number + a 1px rule) and the syntax-coloured tokens. Monospace (the whole UI is
 * JetBrains Mono), so token x positions advance by CalcTextSize of each span (robust to tabs). */

#include "ui_editor.hpp"

#include "ui_theme.hpp" /* muted_v4 — the gutter/comment tint */

#include "imgui.h"

#include <cstdio>

namespace hc::ui {

namespace {

/* The editor will not model more than this many lines (the opened buffer is already host-capped; this bounds
 * the line model + the clipper range). Beyond it the view truncates with a note rather than lexing forever. */
constexpr int kMaxLines = 2000;

ImU32 rgb(unsigned v) { return IM_COL32((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, 0xff); }

/* A restrained, desaturated syntax palette (no neon — the restrained-corporate mandate). Default + Comment
 * read from the live theme so they track the accent-independent text/muted tones. */
ImU32 tok_color(TokClass c)
{
    switch (c) {
    case TokClass::Keyword: return rgb(0x9aa6d4); /* soft periwinkle */
    case TokClass::Type:    return rgb(0x86b8b0); /* soft teal       */
    case TokClass::Number:  return rgb(0xc9b08a); /* soft tan        */
    case TokClass::String:  return rgb(0x9bbf8a); /* soft sage       */
    case TokClass::Preproc: return rgb(0xb09ac0); /* soft violet     */
    case TokClass::Comment: return ImGui::GetColorU32(muted_v4());
    case TokClass::Default:
    default:                return ImGui::GetColorU32(ImGuiCol_Text);
    }
}

} // namespace

void Editor::reload(const char *path, const char *bytes, std::size_t len, std::uint64_t hash)
{
    path_ = path ? path : "";
    loaded_hash_ = hash;
    loaded_ = true;
    lines_.clear();
    line_tokens_.clear();
    truncated_ = false;
    max_px_ = 0.0f;

    const Language *lang = language_for_path(path);
    LexState        st = LexState::Normal;

    auto push_line = [&](std::size_t s, std::size_t e) -> bool {
        if ((int)lines_.size() >= kMaxLines) {
            truncated_ = true;
            return false;
        }
        if (e > s && bytes[e - 1] == '\r') e--; /* strip a CR from a CRLF line ending */
        lines_.emplace_back(bytes + s, bytes + e);
        std::vector<Token> toks;
        st = lex_line(*lang, lines_.back().data(), (int)lines_.back().size(), st, toks);
        line_tokens_.push_back(std::move(toks));
        return true;
    };

    std::size_t start = 0;
    bool        room = true;
    if (bytes) {
        for (std::size_t i = 0; i < len && room; i++) {
            if (bytes[i] == '\n') {
                room = push_line(start, i);
                start = i + 1;
            }
        }
    }
    if (room && bytes && start < len) push_line(start, len); /* the final line (no trailing newline) */
    if (lines_.empty()) lines_.emplace_back(), line_tokens_.emplace_back(); /* an empty file -> one blank row */

    for (const auto &L : lines_) {
        float w = ImGui::CalcTextSize(L.c_str()).x;
        if (w > max_px_) max_px_ = w;
    }
}

void Editor::begin_edit(const char *bytes, std::size_t len, std::uint64_t hash)
{
    edit_buf_.assign(bytes ? bytes : "", bytes ? len : 0);
    edit_base_ = edit_buf_;
    edit_seed_hash_ = hash;
    editing_ = true;
}

void Editor::reconcile(const char *bytes, std::size_t len, std::uint64_t hash)
{
    if (hash == edit_seed_hash_) return; /* the displayed content is unchanged under us — nothing to do */
    std::string disk(bytes ? bytes : "", bytes ? len : 0);
    if (disk != edit_buf_) edit_buf_ = disk; /* a genuine external change / Reload -> adopt (discard edits) */
    edit_base_ = edit_buf_;                  /* a save round-trip (disk == buffer) just re-baselines dirty   */
    edit_seed_hash_ = hash;
}

void Editor::draw(const char *path, const char *bytes, std::size_t len, std::uint64_t hash)
{
    const std::string p = path ? path : "";
    if (!loaded_ || hash != loaded_hash_ || p != path_) reload(path, bytes, len, hash);

    if (truncated_)
        ImGui::TextColored(muted_v4(), "showing the first %d lines (file is longer)", kMaxLines);

    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const float  line_h = ImGui::GetTextLineHeight();
    const float  pad = ImGui::GetStyle().ItemSpacing.x;

    char maxnum[16];
    std::snprintf(maxnum, sizeof maxnum, "%d", (int)lines_.size());
    const float gutter_w = ImGui::CalcTextSize(maxnum).x + pad * 2.0f;
    const float code_x = gutter_w + pad; /* the code's left offset from the (unscrolled) content edge */
    const float content_w = code_x + max_px_ + pad;

    const ImU32 band = ImGui::GetColorU32(ImGuiCol_FrameBg); /* the pinned gutter band (occludes scrolled code) */
    const ImU32 num_col = ImGui::GetColorU32(muted_v4());
    const ImU32 rule_col = ImGui::GetColorU32(ImVec4(muted_v4().x, muted_v4().y, muted_v4().z, 0.5f));

    if (ImGui::BeginChild("edit_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        const float scroll_x = ImGui::GetScrollX();
        ImGuiListClipper clip;
        clip.Begin((int)lines_.size(), line_h);
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float  gx = p.x + scroll_x; /* the FIXED content-left (pins the gutter past h-scroll) */

                /* the syntax-coloured code FIRST, so the pinned gutter band drawn after occludes any of it
                 * that has scrolled under the gutter. */
                const std::string &L = lines_[(size_t)i];
                float              x = p.x + code_x;
                for (const Token &t : line_tokens_[(size_t)i]) {
                    if (t.start < 0 || t.len <= 0 || t.start + t.len > (int)L.size()) continue; /* defensive */
                    const char *b = L.data() + t.start;
                    const char *e = b + t.len;
                    dl->AddText(ImVec2(x, p.y), tok_color(t.cls), b, e);
                    x += ImGui::CalcTextSize(b, e).x;
                }

                /* the pinned gutter: an occluding band, a right-aligned line number, a 1px rule. */
                dl->AddRectFilled(ImVec2(gx, p.y), ImVec2(gx + gutter_w, p.y + line_h), band);
                char num[16];
                std::snprintf(num, sizeof num, "%d", i + 1);
                const float nw = ImGui::CalcTextSize(num).x;
                dl->AddText(ImVec2(gx + gutter_w - pad - nw, p.y), num_col, num);
                dl->AddLine(ImVec2(gx + gutter_w, p.y), ImVec2(gx + gutter_w, p.y + line_h), rule_col);

                ImGui::Dummy(ImVec2(content_w, line_h)); /* reserve the row (clipper height + scroll extent) */
            }
        }
        clip.End();
    }
    ImGui::EndChild();
}

} // namespace hc::ui
