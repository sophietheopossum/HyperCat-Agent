/* text_lexer — implementation of the pure, single-pass source tokenizer (see text_lexer.hpp). INTERNAL.
 * ONE generic scanner reads a Language descriptor's knobs; languages are data tables below. Every scan is
 * bounded by `len` (the line bytes are NOT NUL-terminated — a substring of the opened file), so nothing here
 * reads past the line. No allocation beyond the caller's `out`.
 * SIZE NOTE: lex_line is one ~130-line single-pass state machine (carry → preproc → comment → string → number
 * → identifier → default). The branches share the i/def_start cursor + the flush_default invariant, so a split
 * into per-token helpers would thread that shared state through six functions for no real locality — kept whole
 * as a conscious decision (policy rule 3: size is a smell-test, not a gate). */

#include "text_lexer.hpp"

#include <cstring>

namespace hc::ui {

namespace {

bool is_ident_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_ident(unsigned char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* Is the [s, s+len) word present in a NULL-terminated word list? (Exact, case-sensitive — source keywords
 * are.) A null list (a language with no keywords/types) is simply "no". */
bool in_list(const char *const *list, const char *s, int len)
{
    if (!list || len <= 0) return false;
    for (int i = 0; list[i]; i++)
        if ((int)std::strlen(list[i]) == len && std::strncmp(list[i], s, (size_t)len) == 0) return true;
    return false;
}

// ---- language tables -----------------------------------------------------------------------------------

const char *const k_c_keywords[] = {
    "if",        "else",     "for",      "while",   "do",       "switch",  "case",      "default",
    "break",     "continue", "return",   "goto",    "sizeof",   "struct",  "union",     "enum",
    "typedef",   "static",   "const",    "volatile","extern",   "inline",  "register",  "auto",
    "unsigned",  "signed",   "class",    "public",  "private",  "protected","virtual",  "template",
    "typename",  "namespace","using",    "new",     "delete",   "this",    "operator",  "friend",
    "explicit",  "nullptr",  "true",     "false",   "constexpr","noexcept","override",  "final",
    "try",       "catch",    "throw",    "mutable", "decltype", nullptr};
const char *const k_c_types[] = {"int",     "char",    "short",   "long",    "float",    "double",
                                 "void",    "bool",    "size_t",  "int8_t",  "int16_t",  "int32_t",
                                 "int64_t", "uint8_t", "uint16_t","uint32_t","uint64_t", "wchar_t",
                                 "ssize_t", "ptrdiff_t", nullptr};

const char *const k_py_keywords[] = {
    "def",   "class",  "if",    "elif",  "else",   "for",    "while",   "return", "import", "from",
    "as",    "pass",   "break", "continue","try",  "except", "finally", "raise",  "with",   "lambda",
    "yield", "global", "nonlocal","assert","del",  "in",     "is",      "not",    "and",    "or",
    "None",  "True",   "False", "async",  "await", "self",   nullptr};

const char *const k_js_keywords[] = {
    "function","var",   "let",     "const",  "if",     "else",   "for",    "while",  "do",     "switch",
    "case",    "default","break",  "continue","return","class",  "extends","new",    "delete", "typeof",
    "instanceof","this","super",   "import", "export", "from",   "as",     "try",    "catch",  "finally",
    "throw",   "async", "await",   "yield",  "null",   "undefined","true", "false",  "void",   "in",
    "of",      nullptr};

const char *const k_json_keywords[] = {"true", "false", "null", nullptr};

/*                         keywords         types        l-cmt  block  preproc dquote squote */
const Language k_lang_c     = {k_c_keywords,    k_c_types,   "//",  true,  true,   true,  true};
const Language k_lang_py    = {k_py_keywords,   nullptr,     "#",   false, false,  true,  true};
const Language k_lang_js    = {k_js_keywords,   nullptr,     "//",  true,  false,  true,  true};
const Language k_lang_json  = {k_json_keywords, nullptr,     "",    false, false,  true,  false};
const Language k_lang_plain = {nullptr,         nullptr,     "",    false, false,  false, false};

/* Case-insensitive equality of `a` (an extension, no dot) to a lowercase literal `b`. */
bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char lc = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        if (lc != *b) return false;
    }
    return *a == 0 && *b == 0;
}

} // namespace

const Language *language_for_path(const char *path)
{
    if (!path) return &k_lang_plain;
    const char *dot = nullptr;
    for (const char *p = path; *p; p++) {
        if (*p == '/') dot = nullptr; /* a dot before the last slash is a directory, not an extension */
        else if (*p == '.') dot = p;
    }
    if (!dot || !dot[1]) return &k_lang_plain;
    const char *ext = dot + 1;
    if (ieq(ext, "c") || ieq(ext, "h") || ieq(ext, "cc") || ieq(ext, "cpp") || ieq(ext, "cxx") ||
        ieq(ext, "hpp") || ieq(ext, "hh") || ieq(ext, "hxx") || ieq(ext, "inl"))
        return &k_lang_c;
    if (ieq(ext, "py") || ieq(ext, "pyi")) return &k_lang_py;
    if (ieq(ext, "js") || ieq(ext, "jsx") || ieq(ext, "ts") || ieq(ext, "tsx") || ieq(ext, "mjs") ||
        ieq(ext, "cjs"))
        return &k_lang_js;
    if (ieq(ext, "json")) return &k_lang_json;
    return &k_lang_plain;
}

LexState lex_line(const Language &lang, const char *line, int len, LexState st, std::vector<Token> &out)
{
    if (!line || len <= 0) return st;
    int i = 0;
    int def_start = -1; /* start of a pending run of Default bytes (-1 = none open) */

    auto flush_default = [&](int end) {
        if (def_start >= 0 && end > def_start) out.push_back({def_start, end - def_start, TokClass::Default});
        def_start = -1;
    };

    /* Continuing a block comment opened on a previous line: consume up to and including a closing
     * star-slash, else the whole line is comment and we stay in BlockComment. */
    if (st == LexState::BlockComment) {
        int j = i;
        while (j + 1 < len && !(line[j] == '*' && line[j + 1] == '/')) j++;
        if (j + 1 < len && line[j] == '*' && line[j + 1] == '/') {
            out.push_back({i, (j + 2) - i, TokClass::Comment});
            i = j + 2;
            st = LexState::Normal;
        } else {
            out.push_back({i, len - i, TokClass::Comment});
            return LexState::BlockComment;
        }
    }

    /* C/C++ preprocessor: a leading '#' (after optional space) colours the whole line Preproc. */
    if (lang.hash_preproc && st == LexState::Normal) {
        int k = i;
        while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
        if (k < len && line[k] == '#') {
            if (k > i) out.push_back({i, k - i, TokClass::Default});
            out.push_back({k, len - k, TokClass::Preproc});
            return LexState::Normal;
        }
    }

    const int lc = (int)std::strlen(lang.line_comment);

    while (i < len) {
        const unsigned char c = (unsigned char)line[i];

        /* line comment -> rest of line */
        if (lc > 0 && i + lc <= len && std::strncmp(line + i, lang.line_comment, (size_t)lc) == 0) {
            flush_default(i);
            out.push_back({i, len - i, TokClass::Comment});
            return st;
        }
        /* block comment start */
        if (lang.block_comments && i + 1 < len && c == '/' && line[i + 1] == '*') {
            flush_default(i);
            int j = i + 2;
            while (j + 1 < len && !(line[j] == '*' && line[j + 1] == '/')) j++;
            if (j + 1 < len && line[j] == '*' && line[j + 1] == '/') {
                out.push_back({i, (j + 2) - i, TokClass::Comment});
                i = j + 2;
                continue;
            }
            out.push_back({i, len - i, TokClass::Comment});
            return LexState::BlockComment;
        }
        /* string literal (the closing quote, with backslash escapes; unterminated ends at EOL) */
        if ((c == '"' && lang.strings_double) || (c == '\'' && lang.strings_single)) {
            flush_default(i);
            const char q = (char)c;
            int        j = i + 1;
            while (j < len) {
                if (line[j] == '\\' && j + 1 < len) {
                    j += 2;
                    continue;
                }
                if (line[j] == q) {
                    j++;
                    break;
                }
                j++;
            }
            out.push_back({i, j - i, TokClass::String});
            i = j;
            continue;
        }
        /* number (decimal/hex/float with an exponent + common suffixes) */
        if (is_digit(c) || (c == '.' && i + 1 < len && is_digit((unsigned char)line[i + 1]))) {
            flush_default(i);
            int        j   = i;
            const bool hex = (line[j] == '0' && j + 1 < len && (line[j + 1] == 'x' || line[j + 1] == 'X'));
            if (hex) j += 2;
            while (j < len) {
                const unsigned char d = (unsigned char)line[j];
                if (is_digit(d) || d == '_' || d == '.') {
                    j++;
                    continue;
                }
                if (hex && ((d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F'))) {
                    j++;
                    continue;
                }
                if (!hex && (d == 'e' || d == 'E')) { /* exponent, with an optional sign */
                    j++;
                    if (j < len && (line[j] == '+' || line[j] == '-')) j++;
                    continue;
                }
                break;
            }
            while (j < len && line[j] && std::strchr("fFlLuU", line[j])) j++; /* numeric suffixes */
            out.push_back({i, j - i, TokClass::Number});
            i = j;
            continue;
        }
        /* identifier -> keyword / type / plain */
        if (is_ident_start(c)) {
            flush_default(i);
            int j = i + 1;
            while (j < len && is_ident((unsigned char)line[j])) j++;
            const int wlen = j - i;
            TokClass  cls  = TokClass::Default;
            if (in_list(lang.keywords, line + i, wlen)) cls = TokClass::Keyword;
            else if (in_list(lang.types, line + i, wlen)) cls = TokClass::Type;
            out.push_back({i, wlen, cls});
            i = j;
            continue;
        }
        /* anything else: accumulate into a Default run */
        if (def_start < 0) def_start = i;
        i++;
    }
    flush_default(len);
    return st;
}

} // namespace hc::ui
