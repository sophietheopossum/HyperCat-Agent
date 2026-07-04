/* ui_theme — restrained-corporate theme. See ui_theme.hpp + doc 10 (the source color table). */

#include "ui_theme.hpp"

#include "imgui.h"

#include <cstdarg>
#include <cstdio>

namespace hc::ui {

namespace {

ImVec4 hex(unsigned rgb, float a = 1.0f)
{
    return ImVec4(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f,
                  a);
}

ImVec4 accent_color(Accent a)
{
    switch (a) {
    case Accent::Cyan:    return hex(0x00bcd4);
    case Accent::Amber:   return hex(0xfbbf24);
    case Accent::Emerald: return hex(0x10b981);
    case Accent::Violet:  return hex(0x8b5cf6);
    case Accent::Crimson: return hex(0xef4444);
    case Accent::White:
    default:              return hex(0xffffff);
    }
}

} // namespace

void apply_theme(Accent accent)
{
    ImGuiStyle &s = ImGui::GetStyle();
    ImVec4     *c = s.Colors;

    const ImVec4 bgPanel = hex(0x0a0a0a), bgInput = hex(0x050505), bgHeader = hex(0x111111),
                 bgHover = hex(0x1a1a1a), text = hex(0xe5e5e5), muted = hex(0x555555),
                 border = hex(0x333333), borderH = hex(0x666666), acc = accent_color(accent),
                 clear = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = bgPanel;
    c[ImGuiCol_PopupBg] = c[ImGuiCol_FrameBg] = bgInput;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_MenuBarBg] = c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = bgHeader;
    c[ImGuiCol_TitleBgActive] = bgHover;
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = bgHover;
    c[ImGuiCol_TabHovered] = c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = bgHover;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive] = bgHover;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_Border] = c[ImGuiCol_Separator] = c[ImGuiCol_ScrollbarGrab] = border;
    /* ImGui has no BorderHovered slot — borderH drives the scrollbar/separator hover instead */
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_SeparatorHovered] = borderH;
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_Button] = c[ImGuiCol_Header] = clear; /* never filled (doc 10) */
    c[ImGuiCol_ScrollbarBg] = bgInput;
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = c[ImGuiCol_SliderGrabActive] = acc;
    c[ImGuiCol_TabSelectedOverline] = acc;  /* the restrained accent indicator (a 1px line) */
    c[ImGuiCol_SeparatorActive] = acc;
    c[ImGuiCol_DockingPreview] = acc;
    c[ImGuiCol_PlotHistogram] = acc;        /* progress-bar fill = the accent */
    c[ImGuiCol_TextSelectedBg] = ImVec4(acc.x, acc.y, acc.z, 0.25f);

    /* Override ImGui's dark-default slots we didn't touch above — left alone, they bleed blue
     * (resize grips, nav cursor) and yellow (drag-drop) through the monochrome base (doc 10). */
    c[ImGuiCol_ResizeGrip] = clear;          /* no fill — a 1px corner is enough */
    c[ImGuiCol_ResizeGripHovered] = border;
    c[ImGuiCol_ResizeGripActive] = acc;
    c[ImGuiCol_ScrollbarGrabActive] = borderH;
    c[ImGuiCol_TableHeaderBg] = bgHeader;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = border;
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f); /* a barely-there zebra */
    c[ImGuiCol_DockingEmptyBg] = bgPanel;
    c[ImGuiCol_TitleBgCollapsed] = bgHeader;
    c[ImGuiCol_NavCursor] = acc;             /* was default blue */
    c[ImGuiCol_DragDropTarget] = acc;        /* was default yellow */

    s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.0f;
    s.PopupRounding = s.ScrollbarRounding = s.TabRounding = s.GrabRounding = 0.0f;
    s.WindowBorderSize = s.ChildBorderSize = s.FrameBorderSize = 1.0f; /* 1px everywhere */
    s.ScrollbarSize = 6.0f;
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6); /* dense */
}

/* The bold face for the markdown renderer (headings/bold). Null when no Bold TTF is present — the renderer
 * then uses the regular face (an honest fidelity limit). Set once in load_fonts; UI-thread-only. */
static ImFont *g_bold = nullptr;

const char *load_fonts()
{
    ImGuiIO &io = ImGui::GetIO();
    struct Cand {
        const char *family;
        const char *path;
    };
    static const Cand candidates[] = {
        {"JetBrains Mono", "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf"},
        {"JetBrains Mono", "/usr/local/share/fonts/JetBrainsMono-Regular.ttf"},
        {"DejaVu Sans Mono", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"},
    };
    /* AddFontFromFileTTF ASSERTS on a missing file (it does not return null), so confirm the file
     * exists before calling it. Fall back through the chain, then ImGui's built-in default. */
    const char *family = nullptr;
    for (const auto &cand : candidates) {
        FILE *f = std::fopen(cand.path, "rb");
        if (!f) continue;
        std::fclose(f);
        if (io.Fonts->AddFontFromFileTTF(cand.path, 15.0f)) {
            family = cand.family;
            break;
        }
    }
    if (!family) {
        io.Fonts->AddFontDefault();
        family = "ImGui default";
    }
    /* Merge a broad-coverage fallback into the base face so the agent's kaomoji render instead of missing-glyph
     * boxes. JetBrains Mono is a coding font and lacks the decorative codepoints kaomoji use (Greek omega,
     * dingbats like U+2726, half/full-width kana). Noto Sans CJK covers all of them; DejaVu Sans is a narrower
     * backup (Greek/dingbats, no kana). MergeMode adds these glyphs to the PRIOR font; ImGui 1.92 rasterizes them
     * ON DEMAND, so merging a large CJK collection is cheap (only used glyphs load). Same system-font posture +
     * exists-before-load guard as the base (absent => no merge, the boxes simply remain — never a crash). The
     * kaomoji are the AGENT voice, not the restrained-corporate UI chrome, so widening their coverage is in line
     * with the persona canon. UI-thread / pre-first-frame only. */
    static const char *const fallback_paths[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char *path : fallback_paths) {
        FILE *f = std::fopen(path, "rb");
        if (!f) continue;
        std::fclose(f);
        ImFontConfig cfg;
        cfg.MergeMode = true; /* supply only the glyphs the base lacks; the base stays JetBrains Mono */
        if (io.Fonts->AddFontFromFileTTF(path, 15.0f, &cfg)) break;
    }
    /* Optional Bold face (markdown headings/bold). Same exists-before-load guard; null => regular fallback. */
    static const char *const bold_paths[] = {
        "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Bold.ttf",
        "/usr/local/share/fonts/JetBrainsMono-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    };
    for (const char *path : bold_paths) {
        FILE *f = std::fopen(path, "rb");
        if (!f) continue;
        std::fclose(f);
        g_bold = io.Fonts->AddFontFromFileTTF(path, 15.0f);
        if (g_bold) break;
    }
    return family;
}

ImFont *bold_font() { return g_bold; }

void PanelHeader(const char *label)
{
    /* A restrained section header: a small muted uppercase label + a 1px rule. No accent square (the
     * single accent lives only on functional widgets — checkmark / progress / selected-tab overline). */
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", label);
    ImGui::Separator();
    ImGui::Spacing();
}

/* The single accent (functional use), a dim semantic red (error/dead only), and the muted text color — the
 * only three tints, all within the restrained palette. Read from the live style so they track the accent. */
ImVec4 accent_v4() { return ImGui::GetStyleColorVec4(ImGuiCol_CheckMark); }
ImVec4 muted_v4() { return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled); }
ImVec4 err_v4() { return ImVec4(0.82f, 0.42f, 0.42f, 1.0f); }

void stat_cell(const char *label, const char *value, bool accent)
{
    ImGui::TextColored(muted_v4(), "%s", label);
    if (accent) ImGui::TextColored(accent_v4(), "%s", value);
    else ImGui::TextUnformatted(value);
}

void WrappedTextUnformatted(const char *s)
{
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(s);
    ImGui::PopTextWrapPos();
}

void WrappedText(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextV(fmt, args); /* TextV respects the pushed wrap pos (the same path TextWrapped uses) */
    ImGui::PopTextWrapPos();
    va_end(args);
}

std::string fit_ellipsis(const std::string &text, float max_px)
{
    if (text.empty() || ImGui::CalcTextSize(text.c_str()).x <= max_px) return text;
    const char *ell = "..";
    float       budget = max_px - ImGui::CalcTextSize(ell).x;
    if (budget <= 0.0f) return ell;
    size_t cut = 0;
    for (size_t i = 1; i <= text.size(); i++) {
        if (i < text.size() && ((unsigned char)text[i] & 0xC0) == 0x80) continue; /* mid-UTF-8 byte */
        if (ImGui::CalcTextSize(text.c_str(), text.c_str() + i).x <= budget) cut = i;
        else break;
    }
    return text.substr(0, cut) + ell;
}

void EllipsisText(const std::string &text)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(fit_ellipsis(text, avail).c_str());
    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(text.c_str()).x > avail) ImGui::SetTooltip("%s", text.c_str());
}

} // namespace hc::ui
