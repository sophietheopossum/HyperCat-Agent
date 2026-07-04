/* test_text_lexer — the PURE source tokenizer (W5 P5.1): language selection by extension, per-line lexing of
 * keywords/types/numbers/strings/comments/preproc, the FULL-gapless-coverage invariant the renderer relies
 * on, and the multi-line block-comment carry. No GL/ImGui. */

#include "text_lexer.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace hc::ui;

static int g_fail = 0;
#define CHECK(c, m)                                                                                            \
    do {                                                                                                       \
        if (!(c)) {                                                                                            \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                          \
            g_fail++;                                                                                          \
        }                                                                                                      \
    } while (0)

/* Lex one line and assert the renderer invariant: the tokens TILE [0,len) — contiguous, no gaps, no overlap. */
static std::vector<Token> lex(const Language &lang, const char *s, LexState st = LexState::Normal,
                              LexState *out_st = nullptr)
{
    std::vector<Token> toks;
    int                len = (int)std::strlen(s);
    LexState           after = lex_line(lang, s, len, st, toks);
    if (out_st) *out_st = after;
    int pos = 0;
    for (const Token &t : toks) {
        if (t.start != pos) {
            std::fprintf(stderr, "FAIL: coverage gap/overlap at %d (expected %d) in \"%s\"\n", t.start, pos, s);
            g_fail++;
        }
        pos += t.len;
    }
    if (pos != len) {
        std::fprintf(stderr, "FAIL: coverage stops at %d, line len %d in \"%s\"\n", pos, len, s);
        g_fail++;
    }
    return toks;
}

/* The token class covering byte column `col` (Default if none — shouldn't happen given full coverage). */
static TokClass at(const std::vector<Token> &toks, int col)
{
    for (const Token &t : toks)
        if (col >= t.start && col < t.start + t.len) return t.cls;
    return TokClass::Default;
}

int main()
{
    /* --- language selection --- */
    CHECK(language_for_path("a/b/main.c") == language_for_path("x.c"), "extension picks language, not path");
    CHECK(language_for_path("foo.py") != language_for_path("foo.c"), "py != c");
    CHECK(language_for_path("foo.JSON") == language_for_path("bar.json"), "extension match is case-insensitive");
    CHECK(language_for_path(nullptr) == language_for_path("noext"), "null + no-extension -> the same plain lang");
    CHECK(language_for_path("/a.b/Makefile") == language_for_path("plain.txt"), "a dot in a DIR is not an ext");

    const Language &C = *language_for_path("x.c");
    const Language &PY = *language_for_path("x.py");
    const Language &JS = *language_for_path("x.js");
    const Language &JSON = *language_for_path("x.json");
    const Language &PLAIN = *language_for_path("x.unknownext");

    /* --- C: keyword / type / number / string / line comment --- */
    {
        auto t = lex(C, "int x = 42; // note");
        CHECK(at(t, 0) == TokClass::Type, "C: int -> Type");        /* int */
        CHECK(at(t, 8) == TokClass::Number, "C: 42 -> Number");     /* 42 at col 8 */
        CHECK(at(t, 12) == TokClass::Comment, "C: // -> Comment");  /* the comment */
        CHECK(at(t, 4) == TokClass::Default, "C: identifier x -> Default");
    }
    {
        auto t = lex(C, "return \"hi\\\"there\";"); /* return  "hi\"there" ; */
        CHECK(at(t, 0) == TokClass::Keyword, "C: return -> Keyword");
        CHECK(at(t, 7) == TokClass::String, "C: string starts at the quote");
        CHECK(at(t, 9) == TokClass::String, "C: escaped quote stays INSIDE the string");
    }
    /* C preprocessor: a leading # colours the whole line Preproc (even though // is the line comment) */
    {
        auto t = lex(C, "#include <stdio.h>");
        CHECK(at(t, 0) == TokClass::Preproc && at(t, 9) == TokClass::Preproc, "C: #include -> Preproc");
    }

    /* --- the multi-line block-comment carry --- */
    {
        LexState st;
        auto     a = lex(C, "x /* open", LexState::Normal, &st);
        CHECK(at(a, 0) == TokClass::Default, "C: code before the block comment is Default");
        CHECK(at(a, 2) == TokClass::Comment, "C: /* begins a comment");
        CHECK(st == LexState::BlockComment, "C: an unterminated /* carries BlockComment to the next line");
        LexState st2;
        auto     b = lex(C, "still comment */ y", st, &st2); /* carry in BlockComment */
        CHECK(at(b, 0) == TokClass::Comment, "C: the continued line stays Comment until */");
        CHECK(st2 == LexState::Normal, "C: */ closes the block comment");
        CHECK(at(b, 17) == TokClass::Default, "C: code after */ is Default again");
    }
    /* a self-contained block comment on one line returns Normal */
    {
        LexState st;
        auto     t = lex(C, "a /* mid */ b", LexState::Normal, &st);
        CHECK(st == LexState::Normal, "C: a closed /* ... */ does not carry");
        CHECK(at(t, 4) == TokClass::Comment && at(t, 12) == TokClass::Default, "C: comment then code");
    }

    /* --- Python: # is a COMMENT (not preproc); a keyword + a string --- */
    {
        auto t = lex(PY, "def f(): # hi");
        CHECK(at(t, 0) == TokClass::Keyword, "PY: def -> Keyword");
        CHECK(at(t, 9) == TokClass::Comment, "PY: # -> Comment (not preproc)");
    }
    {
        auto t = lex(PY, "s = 'a\\'b'"); /* a single-quoted string with an escape */
        CHECK(at(t, 4) == TokClass::String && at(t, 6) == TokClass::String, "PY: single-quote string + escape");
    }

    /* --- JS keyword + JSON keyword --- */
    CHECK(at(lex(JS, "const x = null"), 0) == TokClass::Keyword, "JS: const -> Keyword");
    CHECK(at(lex(JSON, "{\"k\": true}"), 6) == TokClass::Keyword, "JSON: true -> Keyword");
    CHECK(at(lex(JSON, "{\"k\": true}"), 1) == TokClass::String, "JSON: a key string is a String");

    /* --- plain: everything is Default, but coverage still holds (the gutter still works) --- */
    {
        auto t = lex(PLAIN, "int return // not highlighted");
        for (const Token &tok : t) CHECK(tok.cls == TokClass::Default, "PLAIN: no class is highlighted");
    }

    /* --- edge: empty line, and a hex number --- */
    {
        std::vector<Token> e;
        CHECK(lex_line(C, "", 0, LexState::Normal, e) == LexState::Normal && e.empty(), "empty line -> no tokens");
    }
    CHECK(at(lex(C, "y = 0xFF;"), 4) == TokClass::Number, "C: 0xFF -> Number (hex)");

    if (g_fail == 0) std::printf("test_text_lexer: all checks passed\n");
    return g_fail ? 1 : 0;
}
