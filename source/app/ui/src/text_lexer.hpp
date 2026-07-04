#ifndef HC_UI_TEXT_LEXER_HPP
#define HC_UI_TEXT_LEXER_HPP

/* text_lexer — a tiny, PURE, single-pass source tokenizer for the editor's syntax highlighting (W5 P5.1).
 * INTERNAL to hc_ui. It lives WITH its only consumer (ui_editor) rather than in app/ — syntax colouring is a
 * UI concern, and the engineering DAG is downward (the UI may not be depended on by app/, so a host-level
 * module could not be used here). No GL, no ImGui, no allocation beyond the caller's output vector — the
 * offline-testable core (the text_diff precedent).
 *
 * ONE generic tokenizer parameterized by a Language descriptor (keyword/type sets + comment/string/preproc
 * knobs), so C/C++/Python/JS/JSON share a single code path and a new language is a data row, not new logic.
 * A per-line LexState carries the one multi-line construct that matters in practice — an open block comment —
 * across line boundaries, so the editor can lex line-by-line (cheap re-lex, clipper-friendly).
 *
 * Owns:      nothing — stateless; all state is the caller's line bytes, the carried LexState, and the output.
 * Lifetime:  language_for_path returns a pointer to a STATIC descriptor (process-lifetime; safe to store).
 *            lex_line only appends to the caller-owned `out` and reads the caller's line bytes for the call.
 * Threading: stateless + reentrant (all state is the caller's line bytes + the carried LexState). */

#include <cstddef>
#include <vector>

namespace hc::ui {

/* The token classes the editor colours. Kept deliberately small + restrained (no rainbow) — the renderer
 * maps each to one muted on-theme tint; Default is the base text colour. */
enum class TokClass { Default, Keyword, Type, Number, String, Comment, Preproc };

/* One coloured span within a line: [start, start+len) BYTE offsets, all of one class. lex_line emits FULL,
 * GAPLESS, coalesced coverage of the line (every byte is in exactly one token), so the renderer just walks
 * the tokens left-to-right with no gap-filling. */
struct Token {
    int      start;
    int      len;
    TokClass cls;
};

/* The multi-line carry. Normal = a fresh line; BlockComment = inside a C-style block comment opened on a
 * previous line and not yet closed. (Multi-line strings are rare enough that v1 ends a string at the line
 * end.) */
enum class LexState { Normal, BlockComment };

/* A language descriptor — the knobs the generic tokenizer reads. `keywords`/`types` are NULL-terminated
 * arrays of NUL-terminated words (types may be null). `line_comment` is a 0-2 char marker ("" = none). The
 * bool knobs enable block comments, leading-`#` preprocessor lines (C/C++), and each string-quote style. */
struct Language {
    const char *const *keywords;
    const char *const *types;
    char               line_comment[3];
    bool               block_comments;
    bool               hash_preproc;
    bool               strings_double;
    bool               strings_single;
};

/* Identify a language from a filename/extension. Returns a stable pointer to a static descriptor, or a
 * generic "plain" descriptor (all-Default, gutter still works) for an unknown/missing extension. */
const Language *language_for_path(const char *path);

/* Tokenize ONE line (bytes [0,len)) starting in carry-in state `st`; APPEND full gapless coverage to `out`
 * (not cleared — the caller owns it); return the carry-out LexState for the next line. Bounded by `len`; no
 * allocation beyond `out`. A null/empty line emits nothing and returns `st` unchanged (still in a comment if
 * it was). */
LexState lex_line(const Language &lang, const char *line, int len, LexState st, std::vector<Token> &out);

} // namespace hc::ui

#endif /* HC_UI_TEXT_LEXER_HPP */
