#ifndef HC_UI_MARKDOWN_HPP
#define HC_UI_MARKDOWN_HPP

/* ui_markdown — a small, SAFE, bounded markdown subset for the FINISHED assistant message in the Transcript
 * (WI-4). Two pure-ish pieces: parse_markdown (TOTAL, bounded, linear — emits DATA, never executes) and
 * render_markdown (ImGui, display-only). The STORED transcript stays raw bytes; this is a render-time view,
 * so the replay/manifest hashing + the untrusted-text contract are untouched.
 *
 * Security: the parser turns model text into a block/run tree; every Run::text reaches the screen through
 *   TextUnformatted / "%s" (never a format string), so a literal %s / %n in model output survives as text.
 *   Unsupported constructs (tables, HTML, images) and malformed input (an unterminated fence, unmatched
 *   emphasis) degrade to literal text. Links are parsed as styled text only — NEVER actionable/clickable.
 * Threading: UI thread only (render_markdown touches the live ImGui context). parse_markdown is pure.
 */

#include <string>
#include <string_view>
#include <vector>

namespace hc::ui {

/* One inline span within a block. Bold/Italic/Code carry verbatim text (no nested styling — MVP). */
struct MdRun {
    enum class Style { Text, Bold, Italic, Code };
    Style       style = Style::Text;
    std::string text;
};

struct MdBlock {
    enum class Kind { Paragraph, Heading, CodeBlock, Bullet, Number, Quote, Rule };
    Kind               kind = Kind::Paragraph;
    int                level = 0;    /* Heading: 1..6.  Bullet/Number/Quote: nesting depth (capped).      */
    int                number = 0;   /* Number: the ordinal shown.                                        */
    std::vector<MdRun> runs;         /* inline runs (Paragraph/Heading/Bullet/Number/Quote)               */
    std::string        text;         /* CodeBlock: the verbatim, unstyled body                            */
};

struct MarkdownDoc {
    std::vector<MdBlock> blocks;
};

/* Parse a SUBSET of markdown (headings, fenced code, bullets/numbers, blockquote, rule, inline
 * bold/italic/code) into blocks. Total + bounded (input, block, and run caps) + linear; never throws. */
MarkdownDoc parse_markdown(std::string_view src);

/* Render a parsed doc via ImGui (call inside a window/child). Headings + bold -> the bold font face; italic
 * -> accent tint; inline code -> muted tint; code block -> a 1px-bordered bgInput region (monospace,
 * horizontal-scroll); bullets/numbers -> Bullet/indent; quote -> a 1px left rule + indent; rule ->
 * Separator. All within doc-10's restrained palette (no new colors). */
void render_markdown(const MarkdownDoc &doc);

/* Convenience: parse `content` (memoized by a content hash across frames so a viewed transcript isn't
 * re-parsed every frame) and render it. The static cache OWNS the parsed MarkdownDoc and retains it for the
 * process lifetime up to a >128-entry flush (session-agnostic). `content` is consumed only during this call
 * — never stored (parse_markdown deep-copies into the doc's std::strings). UI-thread-only. */
void render_markdown_cached(std::string_view content);

} // namespace hc::ui

#endif /* HC_UI_MARKDOWN_HPP */
