/* png2qoi — DEV-TIME asset converter (NOT linked into the app; see tools/CMakeLists.txt; WI-5).
 *
 * Reads a PNG (via the vendored, dev-only tools/third_party/stb_image.h — public domain) and writes a QOI
 * file (qoiformat.org) using a self-contained encoder below. Because the encoder lives here and the matching
 * decoder lives in libs/hc_image, the shipped app needs NO image dependency at all: at build time we embed
 * the .qoi bytes; at run time hc_image_decode_qoi turns them back into RGBA8.
 *
 * Usage:  png2qoi <in.png> <out.qoi> [--mono-invert]
 *
 *   --mono-invert  For the dark-UI mascot. The source art is BLACK (and white-highlight) line work on a
 *                  TRANSPARENT background; on HyperCat's near-black panels it must read as LIGHT line work on
 *                  TRANSPARENT. We map every source pixel to RGBA = (255, 255, 255, src_a * (255 - luma)/255)
 *                  where luma is the source grey (Rec.601). This is the faithful generalisation of the WI-5
 *                  spec's "(255,255,255, 255-luma)" — that formula assumes an OPAQUE light background; the
 *                  real art uses ALPHA for the background, so we fold the source alpha in. It degrades
 *                  exactly to the spec in every case: a black opaque line (luma 0, a 255) -> opaque white;
 *                  an opaque white background/highlight (luma 255) -> transparent; a transparent source
 *                  pixel (a 0) -> transparent. Without the flag, pixels are written faithfully (source RGBA).
 *
 * This is a build-time utility: it prints the decoded W*H to stdout and is allowed to use stb's defaults
 * (the tool target scopes away stb's warnings; the app/lib warning set is never relaxed). */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG /* we only ever feed it PNGs — drop the other format decoders from the dev tool */
#include "stb_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- QOI constants (must match libs/hc_image) --- */
#define QOI_OP_RGB   0xfeu
#define QOI_OP_RGBA  0xffu
#define QOI_OP_INDEX 0x00u
#define QOI_OP_DIFF  0x40u
#define QOI_OP_LUMA  0x80u
#define QOI_OP_RUN   0xc0u
#define QOI_HEADER_SIZE 14u

typedef struct {
    unsigned char r, g, b, a;
} rgba;

static unsigned qoi_hash(rgba p)
{
    return (unsigned)(p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) & 63u;
}

static void put_u32_be(unsigned char *buf, size_t *pos, uint32_t v)
{
    buf[(*pos)++] = (unsigned char)(v >> 24);
    buf[(*pos)++] = (unsigned char)(v >> 16);
    buf[(*pos)++] = (unsigned char)(v >> 8);
    buf[(*pos)++] = (unsigned char)(v);
}

/* Encode `px` (w*h RGBA pixels) to QOI into a freshly malloc'd buffer; *out_len gets the byte length.
 * Returns the buffer (caller frees) or NULL on allocation failure. The worst case is every pixel an
 * RGBA chunk (5 bytes) plus the 14-byte header and 8-byte end marker — we size for that, so no write
 * is ever out of bounds (this is a trusted, single-shot dev encode, not a streaming consumer). */
static unsigned char *qoi_encode(const rgba *px, uint32_t w, uint32_t h, size_t *out_len)
{
    size_t         max = QOI_HEADER_SIZE + (size_t)w * h * 5u + 8u;
    unsigned char *buf = (unsigned char *)malloc(max);
    if (!buf) return NULL;

    size_t pos = 0;
    memcpy(buf, "qoif", 4);
    pos = 4;
    put_u32_be(buf, &pos, w);
    put_u32_be(buf, &pos, h);
    buf[pos++] = 4; /* channels: we always emit RGBA (the decoder produces RGBA8 regardless) */
    buf[pos++] = 0; /* colorspace: sRGB-with-linear-alpha (informational) */

    rgba     index[64];
    memset(index, 0, sizeof index);
    rgba     prev = {0, 0, 0, 255};
    uint64_t n = (uint64_t)w * h;
    unsigned run = 0;

    for (uint64_t i = 0; i < n; i++) {
        rgba cur = px[i];

        if (cur.r == prev.r && cur.g == prev.g && cur.b == prev.b && cur.a == prev.a) {
            run++;
            /* flush at 62 (63/64 are stolen by the RGB/RGBA tags) or at the final pixel */
            if (run == 62 || i == n - 1) {
                buf[pos++] = (unsigned char)(QOI_OP_RUN | (run - 1));
                run = 0;
            }
        } else {
            if (run > 0) {
                buf[pos++] = (unsigned char)(QOI_OP_RUN | (run - 1));
                run = 0;
            }

            unsigned hash = qoi_hash(cur);
            if (index[hash].r == cur.r && index[hash].g == cur.g && index[hash].b == cur.b &&
                index[hash].a == cur.a) {
                buf[pos++] = (unsigned char)(QOI_OP_INDEX | hash);
            } else {
                index[hash] = cur;

                if (cur.a == prev.a) {
                    int dr = (int)cur.r - prev.r;
                    int dg = (int)cur.g - prev.g;
                    int db = (int)cur.b - prev.b;
                    int dr_dg = dr - dg;
                    int db_dg = db - dg;

                    if (dr > -3 && dr < 2 && dg > -3 && dg < 2 && db > -3 && db < 2) {
                        buf[pos++] = (unsigned char)(QOI_OP_DIFF | ((dr + 2) << 4) | ((dg + 2) << 2) |
                                                     (db + 2));
                    } else if (dg > -33 && dg < 32 && dr_dg > -9 && dr_dg < 8 && db_dg > -9 &&
                               db_dg < 8) {
                        buf[pos++] = (unsigned char)(QOI_OP_LUMA | (dg + 32));
                        buf[pos++] = (unsigned char)(((dr_dg + 8) << 4) | (db_dg + 8));
                    } else {
                        buf[pos++] = (unsigned char)QOI_OP_RGB;
                        buf[pos++] = cur.r;
                        buf[pos++] = cur.g;
                        buf[pos++] = cur.b;
                    }
                } else {
                    buf[pos++] = (unsigned char)QOI_OP_RGBA;
                    buf[pos++] = cur.r;
                    buf[pos++] = cur.g;
                    buf[pos++] = cur.b;
                    buf[pos++] = cur.a;
                }
            }
        }
        prev = cur;
    }

    /* 8-byte end marker: seven 0x00 then 0x01 */
    for (int k = 0; k < 7; k++) buf[pos++] = 0x00;
    buf[pos++] = 0x01;

    *out_len = pos;
    return buf;
}

/* Rec.601 luma of an (r,g,b) grey/colour, 0..255. */
static unsigned char luma601(unsigned char r, unsigned char g, unsigned char b)
{
    /* integer-rounded 0.299 R + 0.587 G + 0.114 B */
    return (unsigned char)((77u * r + 150u * g + 29u * b + 128u) >> 8);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    int         mono_invert = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mono-invert") == 0) {
            mono_invert = 1;
        } else if (!in) {
            in = argv[i];
        } else if (!out) {
            out = argv[i];
        } else {
            fprintf(stderr, "png2qoi: unexpected extra argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!in || !out) {
        fprintf(stderr, "usage: png2qoi <in.png> <out.qoi> [--mono-invert]\n");
        return 2;
    }

    int            iw = 0, ih = 0, ic = 0;
    unsigned char *src = stbi_load(in, &iw, &ih, &ic, 4); /* force RGBA8 */
    if (!src) {
        fprintf(stderr, "png2qoi: cannot decode '%s': %s\n", in, stbi_failure_reason());
        return 1;
    }
    if (iw <= 0 || ih <= 0) {
        fprintf(stderr, "png2qoi: '%s' has non-positive dimensions\n", in);
        stbi_image_free(src);
        return 1;
    }

    uint64_t n = (uint64_t)iw * (uint64_t)ih;
    rgba    *px = (rgba *)malloc((size_t)(n * sizeof(rgba)));
    if (!px) {
        fprintf(stderr, "png2qoi: out of memory for %dx%d pixels\n", iw, ih);
        stbi_image_free(src);
        return 1;
    }

    for (uint64_t i = 0; i < n; i++) {
        unsigned char r = src[i * 4 + 0];
        unsigned char g = src[i * 4 + 1];
        unsigned char b = src[i * 4 + 2];
        unsigned char a = src[i * 4 + 3];
        if (mono_invert) {
            unsigned char y = luma601(r, g, b);
            /* light line on transparent: white, alpha = source alpha scaled by ink coverage (255-luma) */
            unsigned char ink = (unsigned char)((unsigned)(255u - y) * (unsigned)a / 255u);
            px[i].r = 255;
            px[i].g = 255;
            px[i].b = 255;
            px[i].a = ink;
        } else {
            px[i].r = r;
            px[i].g = g;
            px[i].b = b;
            px[i].a = a;
        }
    }
    stbi_image_free(src);

    size_t         qlen = 0;
    unsigned char *qoi = qoi_encode(px, (uint32_t)iw, (uint32_t)ih, &qlen);
    free(px);
    if (!qoi) {
        fprintf(stderr, "png2qoi: out of memory encoding QOI\n");
        return 1;
    }

    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "png2qoi: cannot open '%s' for writing\n", out);
        free(qoi);
        return 1;
    }
    size_t wrote = fwrite(qoi, 1, qlen, f);
    int    ok = (wrote == qlen);
    if (fclose(f) != 0) ok = 0;
    free(qoi);
    if (!ok) {
        fprintf(stderr, "png2qoi: short write to '%s'\n", out);
        return 1;
    }

    printf("%s -> %s : %dx%d  (%zu QOI bytes%s)\n", in, out, iw, ih, qlen,
           mono_invert ? ", mono-invert" : "");
    return 0;
}
