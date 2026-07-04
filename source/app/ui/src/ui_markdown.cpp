/* ui_markdown — bounded markdown subset parser + ImGui renderer. See ui_markdown.hpp.
 *
 * The parser is a single linear pass: a line is classified (fence / heading / rule / bullet / number /
 * quote / paragraph) and inline-parsed for bold/italic/code spans. Everything is capped (input bytes,
 * block count, runs per block, nesting) and degrades to literal text — it never throws and never executes.
 * The renderer maps the tree onto ImGui within doc-10's restrained palette (the bold font face + the accent
 * tint + the muted tint + a bordered code region — no new colors). Untrusted text is rendered ONLY via
 * TextUnformatted, so a literal %s/%n in model output is shown verbatim. */

#include "ui_markdown.hpp"

#include "ui_theme.hpp" /* bold_font(), accent_v4(), muted_v4() */
#include "ui_copy.hpp"  /* copy_button — the per-code-block copy affordance */

#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>

namespace hc::ui {

namespace {

constexpr size_t kMaxInput = 256u * 1024u; /* a transcript message is worker-bounded; cap defensively  */
constexpr size_t kMaxBlocks = 4096;        /* bound the block count against a pathological message      */
constexpr size_t kMaxRuns = 512;           /* bound inline runs per block                               */
constexpr int    kMaxIndent = 6;           /* list/quote nesting depth cap (visual + bound)             */
using sv = std::string_view;

/* Inline scan: emit Bold (**..**), Italic (*..* or _.._), Code (`..`) spans + literal Text. An unmatched
 * marker stays literal. No nested styling (MVP). Bounded by kMaxRuns. */
void parse_inline(sv s, std::vector<MdRun> &out)
{
    std::string text;
    auto        flush = [&]() {
        if (!text.empty()) {
            out.push_back({MdRun::Style::Text, text});
            text.clear();
        }
    };
    size_t i = 0, n = s.size();
    while (i < n && out.size() < kMaxRuns) {
        char c = s[i];
        if (c == '`') {
            size_t j = s.find('`', i + 1);
            if (j != sv::npos) {
                flush();
                out.push_back({MdRun::Style::Code, std::string(s.substr(i + 1, j - i - 1))});
                i = j + 1;
                continue;
            }
        } else if (c == '*' && i + 1 < n && s[i + 1] == '*') {
            size_t j = s.find("**", i + 2);
            if (j != sv::npos && j > i + 2) {
                flush();
                out.push_back({MdRun::Style::Bold, std::string(s.substr(i + 2, j - i - 2))});
                i = j + 2;
                continue;
            }
        } else if (c == '*' || c == '_') {
            size_t j = s.find(c, i + 1);
            if (j != sv::npos && j > i + 1) {
                flush();
                out.push_back({MdRun::Style::Italic, std::string(s.substr(i + 1, j - i - 1))});
                i = j + 1;
                continue;
            }
        }
        text += c;
        i++;
    }
    if (out.size() < kMaxRuns) flush(); /* keep the run count at the cap exactly (don't overshoot by one) */
}

/* A horizontal rule: 3+ identical -, *, or _ (nothing else). */
bool is_rule(sv t)
{
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char ch : t)
        if (ch != c) return false;
    return true;
}

} // namespace

MarkdownDoc parse_markdown(sv src)
{
    if (src.size() > kMaxInput) src = src.substr(0, kMaxInput);
    MarkdownDoc doc;
    std::string para; /* accumulating paragraph text (consecutive plain lines, space-joined) */

    auto flush_para = [&]() {
        if (para.empty()) return;
        if (doc.blocks.size() < kMaxBlocks) {
            MdBlock b;
            b.kind = MdBlock::Kind::Paragraph;
            parse_inline(para, b.runs);
            doc.blocks.push_back(std::move(b));
        }
        para.clear();
    };

    size_t      i = 0, n = src.size();
    bool        in_fence = false;
    std::string code;
    while (i < n && doc.blocks.size() < kMaxBlocks) {
        size_t eol = src.find('\n', i);
        sv     line = src.substr(i, (eol == sv::npos ? n : eol) - i);
        i = (eol == sv::npos ? n : eol + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        bool fence = line.size() >= 3 && line.substr(0, 3) == "```";
        if (in_fence) {
            if (fence) { /* close the fence */
                MdBlock b;
                b.kind = MdBlock::Kind::CodeBlock;
                b.text = code;
                doc.blocks.push_back(std::move(b));
                code.clear();
                in_fence = false;
            } else if (code.size() < kMaxInput) {
                code.append(line);
                code.push_back('\n');
            }
            continue;
        }
        if (fence) {
            flush_para();
            in_fence = true;
            code.clear();
            continue;
        }

        size_t ls = 0;
        while (ls < line.size() && line[ls] == ' ') ls++;
        sv t = line.substr(ls);
        if (t.empty()) {
            flush_para();
            continue;
        }
        int indent = std::min((int)(ls / 2), kMaxIndent);

        if (t[0] == '#') { /* heading: 1..6 '#' then a space */
            size_t h = 0;
            while (h < t.size() && t[h] == '#' && h < 6) h++;
            if (h > 0 && h < t.size() && t[h] == ' ') {
                flush_para();
                MdBlock b;
                b.kind = MdBlock::Kind::Heading;
                b.level = (int)h;
                parse_inline(t.substr(h + 1), b.runs);
                doc.blocks.push_back(std::move(b));
                continue;
            }
        }
        if (is_rule(t)) {
            flush_para();
            MdBlock b;
            b.kind = MdBlock::Kind::Rule;
            doc.blocks.push_back(std::move(b));
            continue;
        }
        if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
            flush_para();
            MdBlock b;
            b.kind = MdBlock::Kind::Bullet;
            b.level = indent;
            parse_inline(t.substr(2), b.runs);
            doc.blocks.push_back(std::move(b));
            continue;
        }
        { /* numbered: digits, then ". " */
            size_t d = 0;
            while (d < t.size() && t[d] >= '0' && t[d] <= '9') d++;
            if (d > 0 && d + 1 < t.size() && t[d] == '.' && t[d + 1] == ' ') {
                flush_para();
                MdBlock b;
                b.kind = MdBlock::Kind::Number;
                b.level = indent;
                /* strtol + clamp (not atoi): a hostile/huge ordinal must not overflow to a negative int */
                long num = std::strtol(std::string(t.substr(0, d)).c_str(), nullptr, 10);
                b.number = num < 0 ? 0 : (num > 999999 ? 999999 : (int)num);
                parse_inline(t.substr(d + 2), b.runs);
                doc.blocks.push_back(std::move(b));
                continue;
            }
        }
        if (t[0] == '>') {
            flush_para();
            sv q = t.substr(1);
            if (!q.empty() && q[0] == ' ') q = q.substr(1);
            MdBlock b;
            b.kind = MdBlock::Kind::Quote;
            parse_inline(q, b.runs);
            doc.blocks.push_back(std::move(b));
            continue;
        }
        /* a plain paragraph line — accumulate */
        if (!para.empty()) para.push_back(' ');
        para.append(t);
    }
    /* EOF: an UNTERMINATED fence degrades to a code block (graceful, never breaks the view) */
    if (in_fence && !code.empty() && doc.blocks.size() < kMaxBlocks) {
        MdBlock b;
        b.kind = MdBlock::Kind::CodeBlock;
        b.text = code;
        doc.blocks.push_back(std::move(b));
    }
    flush_para();
    return doc;
}

namespace {

/* Render inline runs as one wrapped flow, WORD BY WORD. ImGui wraps each TextUnformatted within its OWN
 * start-x, so the old "one wrapped widget per run + SameLine(0,0)" collapsed any run that began near the right
 * edge (e.g. the plain text after a mid-paragraph emphasis span) into a one-glyph-wide column down the margin.
 * Instead: measure each word in its run's font, keep it on the current line if it fits, else break to a fresh
 * line at the block's left margin — so emphasis spans flow inline and long prose wraps normally. A space seen
 * since the last word (including ACROSS a run boundary, where the spacing lives in the run text) becomes the
 * inter-word gap; the next word's placement is decided from the PREVIOUS word's actual rect, so it self-corrects
 * after every wrap. A PushTextWrapPos safety net still wraps a single word wider than the whole line. */
void render_runs(const std::vector<MdRun> &runs)
{
    const float right_x = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float space_w = ImGui::CalcTextSize(" ").x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);

    bool any = false;
    bool pending_space = false; /* a space since the last word — carried across run boundaries */
    for (const auto &r : runs) {
        size_t i = 0, n = r.text.size();
        while (i < n) {
            if (r.text[i] == ' ') {
                pending_space = true; /* a break opportunity; collapsed to one gap at the next word */
                i++;
                continue;
            }
            size_t j = i;
            while (j < n && r.text[j] != ' ') j++;
            const std::string word = r.text.substr(i, j - i);
            i = j;

            /* push the run's font so the width we measure matches what we draw (bold is wider). */
            ImFont *bf = (r.style == MdRun::Style::Bold) ? bold_font() : nullptr;
            if (bf) ImGui::PushFont(bf);
            const float word_w = ImGui::CalcTextSize(word.c_str()).x;
            if (any) {
                const float gap = pending_space ? space_w : 0.0f;
                const float last_x2 = ImGui::GetItemRectMax().x; /* previous word's right edge (screen) */
                if (last_x2 + gap + word_w <= right_x)
                    ImGui::SameLine(0.0f, gap); /* fits -> continue this line; else fall to a new one */
            }
            any = true;
            pending_space = false;

            const bool tint = (r.style == MdRun::Style::Italic || r.style == MdRun::Style::Code);
            if (r.style == MdRun::Style::Italic) ImGui::PushStyleColor(ImGuiCol_Text, accent_v4());
            else if (r.style == MdRun::Style::Code) ImGui::PushStyleColor(ImGuiCol_Text, muted_v4());
            ImGui::TextUnformatted(word.c_str()); /* untrusted -> verbatim, never a format string */
            if (tint) ImGui::PopStyleColor();
            if (bf) ImGui::PopFont();
        }
    }

    ImGui::PopTextWrapPos();
    if (!any) ImGui::NewLine(); /* an empty / space-only block still advances a line */
}

} // namespace

void render_markdown(const MarkdownDoc &doc)
{
    for (size_t bi = 0; bi < doc.blocks.size(); bi++) {
        const MdBlock &b = doc.blocks[bi];
        ImGui::PushID((int)bi);
        switch (b.kind) {
        case MdBlock::Kind::Heading: {
            ImFont *bf = bold_font();
            if (bf) ImGui::PushFont(bf);
            render_runs(b.runs);
            if (bf) ImGui::PopFont();
            ImGui::Spacing();
            break;
        }
        case MdBlock::Kind::CodeBlock: {
            int lines = 1;
            for (char c : b.text)
                if (c == '\n') lines++;
            float h = ImGui::GetTextLineHeightWithSpacing() * (float)std::min(lines, 40) +
                      ImGui::GetStyle().FramePadding.y * 2.0f;
            /* B: a slim header row with a right-aligned "copy" button lifting the raw block onto the clipboard */
            {
                const float bw = ImGui::CalcTextSize("copy").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                const float av = ImGui::GetContentRegionAvail().x;
                if (av > bw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + av - bw);
                copy_button("##codecopy", b.text);
            }
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            if (ImGui::BeginChild("##code", ImVec2(0, h), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_HorizontalScrollbar))
                ImGui::TextUnformatted(b.text.c_str()); /* verbatim, monospace base font, h-scroll */
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Spacing();
            break;
        }
        case MdBlock::Kind::Bullet:
            if (b.level > 0) ImGui::Indent((float)b.level * 14.0f);
            ImGui::Bullet();
            ImGui::SameLine();
            render_runs(b.runs);
            if (b.level > 0) ImGui::Unindent((float)b.level * 14.0f);
            break;
        case MdBlock::Kind::Number:
            if (b.level > 0) ImGui::Indent((float)b.level * 14.0f);
            ImGui::Text("%d.", b.number);
            ImGui::SameLine();
            render_runs(b.runs);
            if (b.level > 0) ImGui::Unindent((float)b.level * 14.0f);
            break;
        case MdBlock::Kind::Quote: {
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Indent(10.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, muted_v4());
            render_runs(b.runs);
            ImGui::PopStyleColor();
            ImGui::Unindent(10.0f);
            ImVec2 p1 = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x + 2, p0.y), ImVec2(p0.x + 2, p1.y),
                                                ImGui::GetColorU32(muted_v4()), 1.0f); /* 1px left rule */
            break;
        }
        case MdBlock::Kind::Rule:
            ImGui::Separator();
            break;
        case MdBlock::Kind::Paragraph:
        default:
            render_runs(b.runs);
            ImGui::Spacing();
            break;
        }
        ImGui::PopID();
    }
}

void render_markdown_cached(sv content)
{
    /* memoize the parse by content hash so a viewed transcript isn't re-parsed every frame; bounded. */
    static std::unordered_map<size_t, MarkdownDoc> cache;
    size_t                                         key = std::hash<sv>{}(content);
    auto                                           it = cache.find(key);
    if (it == cache.end()) {
        if (cache.size() > 128) cache.clear(); /* bound the cache (distinct messages viewed) */
        it = cache.emplace(key, parse_markdown(content)).first;
    }
    render_markdown(it->second);
}

} // namespace hc::ui
