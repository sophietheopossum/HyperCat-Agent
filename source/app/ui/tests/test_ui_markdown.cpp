/* test_ui_markdown — the WI-4 parser gate (PURE, offline, no ImGui context needed: only parse_markdown is
 * exercised). Proves the security-critical + robustness properties: a literal %s/%n survives as TEXT (the
 * untrusted-text contract — the parser emits data, never a format string), an UNTERMINATED code fence
 * degrades to a code block (graceful, no crash/hang), and headings/bold/italic/inline-code/bullets/
 * numbers/quote/rule parse to the right blocks; unmatched emphasis markers stay literal. */

#include "ui_markdown.hpp"

#include <cstdio>
#include <string>

using namespace hc::ui;

static int g_fails = 0;
#define CHECK(c, m)                                                                                    \
    do {                                                                                               \
        if (!(c)) {                                                                                    \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                   \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

int main()
{
    /* --- a literal %s / %n survives as text (no format interpretation) --- */
    {
        MarkdownDoc d = parse_markdown("printf(\"%s%n\", x);");
        CHECK(d.blocks.size() == 1 && d.blocks[0].kind == MdBlock::Kind::Paragraph,
              "a plain line is one paragraph");
        bool found = false;
        for (const auto &r : d.blocks[0].runs)
            if (r.text.find("%s%n") != std::string::npos) found = true;
        CHECK(found, "literal %s%n survives verbatim as a Text run (never a format string)");
    }

    /* --- an unterminated code fence degrades to a code block (graceful) --- */
    {
        MarkdownDoc d = parse_markdown("```\nint x = 1;\nno closing fence");
        CHECK(!d.blocks.empty() && d.blocks.back().kind == MdBlock::Kind::CodeBlock,
              "an unterminated fence degrades to a CodeBlock");
        CHECK(!d.blocks.empty() && d.blocks.back().text.find("int x = 1;") != std::string::npos,
              "the fence body is captured verbatim");
    }

    /* --- a fenced code block (closed) is verbatim, NOT inline-parsed --- */
    {
        MarkdownDoc d = parse_markdown("text\n```\n**not bold** `not code`\n```\nafter");
        bool has_code = false;
        for (const auto &b : d.blocks)
            if (b.kind == MdBlock::Kind::CodeBlock && b.text.find("**not bold**") != std::string::npos)
                has_code = true;
        CHECK(has_code, "code-block content is verbatim (markers not interpreted)");
    }

    /* --- heading levels --- */
    {
        MarkdownDoc d = parse_markdown("## Title here");
        CHECK(d.blocks.size() == 1 && d.blocks[0].kind == MdBlock::Kind::Heading && d.blocks[0].level == 2,
              "## -> Heading level 2");
    }

    /* --- inline bold / italic / code spans --- */
    {
        MarkdownDoc d = parse_markdown("a **bold** and *it* and `code` x");
        CHECK(d.blocks.size() == 1, "inline emphasis stays one paragraph");
        bool b = false, it = false, co = false;
        for (const auto &r : d.blocks[0].runs) {
            if (r.style == MdRun::Style::Bold && r.text == "bold") b = true;
            if (r.style == MdRun::Style::Italic && r.text == "it") it = true;
            if (r.style == MdRun::Style::Code && r.text == "code") co = true;
        }
        CHECK(b && it && co, "bold / italic / inline-code spans parse to styled runs");
    }

    /* --- bullets, numbers, quote, rule --- */
    {
        MarkdownDoc d = parse_markdown("- one\n- two\n3. third\n> quoted\n---");
        CHECK(d.blocks.size() == 5, "5 distinct blocks");
        CHECK(d.blocks.size() == 5 && d.blocks[0].kind == MdBlock::Kind::Bullet &&
                  d.blocks[1].kind == MdBlock::Kind::Bullet,
              "- lines -> Bullet blocks");
        CHECK(d.blocks.size() == 5 && d.blocks[2].kind == MdBlock::Kind::Number && d.blocks[2].number == 3,
              "N. -> Number block keeping the ordinal");
        CHECK(d.blocks.size() == 5 && d.blocks[3].kind == MdBlock::Kind::Quote, "> -> Quote block");
        CHECK(d.blocks.size() == 5 && d.blocks[4].kind == MdBlock::Kind::Rule, "--- -> Rule block");
    }

    /* --- unmatched emphasis markers stay literal (no half-open span) --- */
    {
        MarkdownDoc d = parse_markdown("a * b and ` c");
        CHECK(d.blocks.size() == 1, "one paragraph");
        bool styled = false;
        for (const auto &r : d.blocks[0].runs)
            if (r.style != MdRun::Style::Text) styled = true;
        CHECK(!styled, "unmatched * and ` stay literal Text");
    }

    /* --- empty input is empty (no crash) --- */
    CHECK(parse_markdown("").blocks.empty(), "empty input -> no blocks");

    if (g_fails == 0) std::printf("test_ui_markdown: OK\n");
    return g_fails ? 1 : 0;
}
