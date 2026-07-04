/* ui_docview — see ui_docview.hpp. The pure classify: magic bytes first (authoritative), then the extension
 * (markdown vs plain text), then a NUL/control-byte content heuristic (text vs binary). No ImGui/GL/I-O. */

#include "ui_docview.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace hc::ui {

namespace {

/* Case-insensitive "does `name` end with `ext`" (ext includes the dot, lowercase). */
bool ends_with_ci(const std::string &name, const char *ext)
{
    size_t en = std::strlen(ext);
    if (name.size() < en) return false;
    for (size_t i = 0; i < en; i++) {
        char c = name[name.size() - en + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != ext[i]) return false;
    }
    return true;
}

bool magic_is(const unsigned char *b, size_t len, const unsigned char *sig, size_t n)
{
    return len >= n && std::memcmp(b, sig, n) == 0;
}

/* A leading-bytes heuristic: a NUL byte => binary; otherwise count control bytes that aren't TAB/LF/CR/FF — a
 * high density => binary, else text. Inspects at most the first 4 KiB (the caller usually passes a prefix). */
bool looks_like_text(const unsigned char *b, size_t len)
{
    if (!b || len == 0) return false;
    size_t n = len < 4096 ? len : 4096;
    size_t suspicious = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == 0) return false; /* a NUL never appears in text */
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f') suspicious++;
        /* high bytes (0x80+) are fine — valid UTF-8 multibyte; we don't fully validate UTF-8 here */
    }
    return suspicious * 100 < n * 5; /* < 5% odd control bytes => treat as text */
}

} // namespace

DocKind classify_doc(const std::string &name, const unsigned char *bytes, size_t len)
{
    /* 1) magic bytes — authoritative, win over a mislabeled extension */
    if (bytes && len) {
        static const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        static const unsigned char jpg[] = {0xff, 0xd8, 0xff};
        if (magic_is(bytes, len, png, sizeof png)) return DocKind::ImagePng;
        if (magic_is(bytes, len, jpg, sizeof jpg)) return DocKind::ImageJpeg;
        if (magic_is(bytes, len, (const unsigned char *)"qoif", 4)) return DocKind::ImageQoi;
        if (magic_is(bytes, len, (const unsigned char *)"%PDF-", 5)) return DocKind::Pdf;
        /* audio (Phase C.2): WAV (RIFF…WAVE) / OGG / FLAC / MP3 (ID3 tag or a frame sync) — opens in the player */
        if (len >= 12 && magic_is(bytes, len, (const unsigned char *)"RIFF", 4) &&
            std::memcmp(bytes + 8, "WAVE", 4) == 0)
            return DocKind::Audio;
        if (magic_is(bytes, len, (const unsigned char *)"OggS", 4)) return DocKind::Audio;
        if (magic_is(bytes, len, (const unsigned char *)"fLaC", 4)) return DocKind::Audio;
        if (magic_is(bytes, len, (const unsigned char *)"ID3", 3)) return DocKind::Audio;
        if (len >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0) return DocKind::Audio; /* MP3 frame sync */
    }

    /* 2) extension — the magic-less formats (and a .pdf/.png-named file with NO magic falls through to content) */
    if (ends_with_ci(name, ".md") || ends_with_ci(name, ".markdown")) return DocKind::Markdown;

    /* 3) content heuristic — text vs binary */
    if (looks_like_text(bytes, len)) return DocKind::PlainText;

    /* 4) no bytes to inspect: lean on a known text/code extension, else opaque binary */
    if (!bytes || len == 0) {
        static const char *const text_ext[] = {
            ".txt", ".text", ".c",    ".h",    ".cpp",  ".cc",  ".hpp", ".cxx", ".py",  ".js",   ".ts",
            ".json", ".cmake", ".sh", ".bash", ".toml", ".yaml", ".yml", ".rs", ".go",  ".html", ".htm",
            ".css", ".xml",  ".cfg",  ".ini",  ".log",  ".rb",  ".java", ".lua", ".sql", ".csv"};
        for (const char *e : text_ext)
            if (ends_with_ci(name, e)) return DocKind::PlainText;
    }
    return DocKind::Binary;
}

const char *doc_kind_label(DocKind k)
{
    switch (k) {
    case DocKind::PlainText: return "text";
    case DocKind::Markdown:  return "markdown";
    case DocKind::ImagePng:  return "PNG image";
    case DocKind::ImageJpeg: return "JPEG image";
    case DocKind::ImageQoi:  return "QOI image";
    case DocKind::Pdf:       return "PDF";
    case DocKind::Audio:     return "audio";
    case DocKind::Binary:    return "binary";
    }
    return "binary";
}

const char *kind_label_from_name(const std::string &name)
{
    struct Ext {
        const char *ext;
        const char *kind;
    };
    /* extension -> a friendly browser category; first match wins (order is irrelevant — exts are disjoint) */
    static const Ext table[] = {
        {".png", "image"},    {".jpg", "image"},   {".jpeg", "image"},  {".qoi", "image"},  {".gif", "image"},
        {".bmp", "image"},    {".webp", "image"},  {".md", "markdown"}, {".markdown", "markdown"},
        {".json", "json"},    {".html", "html"},   {".htm", "html"},    {".pdf", "pdf"},
        {".wav", "audio"},    {".mp3", "audio"},   {".flac", "audio"},  {".ogg", "audio"},  {".oga", "audio"},
        {".c", "code"},       {".h", "code"},      {".cpp", "code"},    {".cc", "code"},    {".cxx", "code"},
        {".hpp", "code"},     {".py", "code"},     {".js", "code"},     {".ts", "code"},    {".rs", "code"},
        {".go", "code"},      {".java", "code"},   {".lua", "code"},    {".sql", "code"},   {".sh", "code"},
        {".bash", "code"},    {".rb", "code"},     {".css", "code"},    {".xml", "code"},   {".cmake", "code"},
        {".toml", "code"},    {".yaml", "code"},   {".yml", "code"},    {".ini", "code"},   {".cfg", "code"},
        {".txt", "text"},     {".text", "text"},   {".log", "text"},    {".csv", "text"},
    };
    for (const Ext &e : table)
        if (ends_with_ci(name, e.ext)) return e.kind;
    return "file"; /* unknown/extensionless — honest: we only know the name, not the bytes */
}

} // namespace hc::ui
