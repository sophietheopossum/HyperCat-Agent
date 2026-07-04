/* test_ui_docview — the pure classify (W4 P4.0c): magic bytes are authoritative (win over a mislabeled
 * extension), the extension disambiguates markdown vs text, and a NUL/control heuristic separates text from
 * binary. No ImGui/GL. */

#include "ui_docview.hpp"

#include <cstdio>
#include <cstring>

using hc::ui::classify_doc;
using hc::ui::DocKind;

static int g_fail = 0;
#define CHECK(c, m)                                                                                            \
    do {                                                                                                       \
        if (!(c)) {                                                                                            \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                          \
            g_fail++;                                                                                          \
        }                                                                                                      \
    } while (0)

static DocKind cls(const char *name, const char *bytes)
{
    return classify_doc(name, (const unsigned char *)bytes, bytes ? std::strlen(bytes) : 0);
}

int main()
{
    /* --- magic bytes are authoritative --- */
    CHECK(cls("a.png", "\x89PNG\r\n\x1a\n....") == DocKind::ImagePng, "PNG magic -> ImagePng");
    CHECK(cls("a.jpg", "\xff\xd8\xff\xe0junk") == DocKind::ImageJpeg, "JPEG magic -> ImageJpeg");
    CHECK(cls("a.qoi", "qoif....") == DocKind::ImageQoi, "QOI magic -> ImageQoi");
    CHECK(cls("doc.pdf", "%PDF-1.7\n...") == DocKind::Pdf, "PDF magic -> Pdf");
    /* magic WINS over a mislabeled extension */
    CHECK(cls("notes.txt", "\x89PNG\r\n\x1a\n") == DocKind::ImagePng, "PNG magic beats a .txt extension");

    /* --- a mislabeled image with NO magic is NOT treated as an image --- */
    CHECK(cls("fake.png", "hello, this is plain text") == DocKind::PlainText,
          "a .png named file with text content is text, not an image");

    /* --- extension disambiguates markdown --- */
    CHECK(cls("README.md", "# Title\n\nbody") == DocKind::Markdown, ".md -> Markdown");
    CHECK(cls("notes.MARKDOWN", "# x") == DocKind::Markdown, ".markdown (case-insensitive) -> Markdown");

    /* --- content heuristic: text vs binary --- */
    CHECK(cls("code.c", "int main(void){return 0;}\n") == DocKind::PlainText, "source content -> PlainText");
    CHECK(cls("anything", "just some words with\ttabs and\nnewlines") == DocKind::PlainText,
          "plain words -> PlainText");
    {
        unsigned char bin[] = {'h', 'i', 0x00, 0x01, 0x02, 0xff}; /* embedded NUL => binary */
        CHECK(classify_doc("data.bin", bin, sizeof bin) == DocKind::Binary, "a NUL byte -> Binary");
    }
    {
        unsigned char ctrl[64];
        for (int i = 0; i < 64; i++) ctrl[i] = (unsigned char)(i % 7 + 1); /* dense control bytes */
        CHECK(classify_doc("x", ctrl, sizeof ctrl) == DocKind::Binary, "dense control bytes -> Binary");
    }

    /* --- no bytes: lean on a known text extension, else binary --- */
    CHECK(classify_doc("main.py", nullptr, 0) == DocKind::PlainText, "no bytes + .py -> PlainText");
    CHECK(classify_doc("a.unknownext", nullptr, 0) == DocKind::Binary, "no bytes + unknown ext -> Binary");
    CHECK(classify_doc("README.md", nullptr, 0) == DocKind::Markdown, "no bytes + .md -> Markdown");

    /* --- a high-byte (UTF-8) text file is still text --- */
    CHECK(cls("u.txt", "caf\xc3\xa9 \xe2\x80\x94 ok") == DocKind::PlainText, "UTF-8 high bytes -> PlainText");

    if (g_fail) {
        std::fprintf(stderr, "test_ui_docview: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("test_ui_docview: all checks passed\n");
    return 0;
}
