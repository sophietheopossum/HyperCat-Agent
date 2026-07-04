#ifndef HC_UI_DOCVIEW_HPP
#define HC_UI_DOCVIEW_HPP

/* ui_docview — classify an opened file's bytes into a viewer KIND (W4 P4.0c). One responsibility: given a file
 * name + its leading bytes, decide how the media panel / file viewer should render it — text, markdown, a
 * specific image format, a PDF (stubbed), or opaque binary. PURE (no ImGui, no GL, no I/O) so it is unit-tested
 * directly, like dag_layout. The MAGIC BYTES are authoritative (they win over a mislabeled extension): a "foo.txt"
 * that actually starts with the PNG signature is an image; a "foo.png" with no image magic is NOT treated as one.
 * The extension only disambiguates the magic-less text formats (markdown vs plain text), and a content heuristic
 * (NUL / control-byte density) separates text from binary when neither magic nor a known extension applies. */

#include <cstddef>
#include <string>

namespace hc::ui {

enum class DocKind {
    PlainText, /* text / source code — rendered as wrapped monospaced text   */
    Markdown,  /* .md / .markdown — rendered through the markdown renderer    */
    ImagePng,  /* PNG magic — decoded via hc_image_decode_auto                */
    ImageJpeg, /* JPEG magic — decoded via hc_image_decode_auto               */
    ImageQoi,  /* QOI magic ("qoif") — decoded via hc_image_decode_qoi        */
    Pdf,       /* %PDF — stub: metadata + a "preview not available" + reveal  */
    Audio,     /* WAV/MP3/FLAC/OGG magic — opens in the Music Player, not the Viewer (Phase C.2) */
    Binary,    /* none of the above — opaque (size + reveal-in-files, no body) */
};

/* Classify `name` + its leading `bytes` (`len` may be a bounded prefix — only the first bytes are inspected).
 * `bytes` may be NULL (len 0) — then classification falls back to the extension alone (Markdown/PlainText for a
 * known one, else Binary). Never reads past `len`. */
DocKind classify_doc(const std::string &name, const unsigned char *bytes, size_t len);

/* The kind's short human label (for the viewer header / a status line). Static string, never freed. */
const char *doc_kind_label(DocKind k);

/* A short, extension-ONLY kind category for the file browser's "Kind" column — one of
 * "image"/"markdown"/"json"/"html"/"code"/"text"/"file" (the catch-all). Distinct from classify_doc, which is
 * byte-authoritative for the opened-file viewer; this is a cheap label for a directory listing where only the
 * name is known (no bytes read). Static string, never freed. */
const char *kind_label_from_name(const std::string &name);

} // namespace hc::ui

#endif /* HC_UI_DOCVIEW_HPP */
