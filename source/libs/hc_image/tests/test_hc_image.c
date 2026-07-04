/* test_hc_image — the WI-5 QOI decoder gate (offline, deterministic, ASan/UBSan-clean). Proves three
 * things:
 *   (a) DECODE CORRECTNESS — a tiny hand-built 2x2 QOI exercising QOI_OP_RGB + QOI_OP_RUN + QOI_OP_RGBA +
 *       QOI_OP_INDEX, plus a separate 2x2 exercising QOI_OP_DIFF + QOI_OP_LUMA, decode to the exact RGBA
 *       bytes (the expected values were cross-checked against the canonical reference qoi.h);
 *   (b) ROUND-TRIP — the real bytes our tools/png2qoi produced for the smallest mascot asset
 *       (hypercat_mascot_face_80x68.qoi, 80x68, mono-invert) decode back to 80x68 with the documented
 *       monochrome-on-transparent property (transparent corner, an opaque white interior pixel);
 *   (c) ROBUSTNESS / FUZZ — every malformed input the threat model calls out (bad magic, truncated header,
 *       truncated mid-chunk for RGB/RGBA/LUMA, an oversized w*h that would overflow, a 1-byte input, an
 *       all-zero buffer, channels=2, an over-cap dimension, a RUN past the pixel count, a NULL out-param)
 *       returns -1 with NO crash, NO overread, and NO leak (*out stays NULL). ASan/UBSan turns any
 *       overread/leak on these paths into a hard test failure. No domain logic — just bytes -> pixels. */

#include "hc_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;
#define CHECK(c, m)                                                                                    \
    do {                                                                                               \
        if (!(c)) {                                                                                    \
            fprintf(stderr, "FAIL: %s\n", (m));                                                        \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

/* W4 P4.0b: a valid 2x2 RGB PNG (PIL-verified) for the hc_image_decode_auto positive + fuzz battery. */
static const unsigned char k_png_2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00,
    0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x38, 0xa1, 0xa1, 0x01,
    0x44, 0x0c, 0x10, 0x0a, 0x00, 0x21, 0x2e, 0x04, 0x61, 0xf6, 0xe1, 0xc2,
    0x4d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82,
};

/* A pixel equals (r,g,b,a) at offset i (in pixels) of an RGBA buffer. */
static int px_eq(const unsigned char *buf, size_t i, int r, int g, int b, int a)
{
    return buf[i * 4 + 0] == r && buf[i * 4 + 1] == g && buf[i * 4 + 2] == b && buf[i * 4 + 3] == a;
}

/* The QOI index hash, so the test can compute the slot for the hand-built INDEX op without hardcoding 31. */
static unsigned hash_slot(int r, int g, int b, int a)
{
    return (unsigned)(r * 3 + g * 5 + b * 7 + a * 11) & 63u;
}

/* --- the round-trip fixture: the exact bytes tools/png2qoi --mono-invert wrote for the smallest asset.
 * Provenance: app/ui/assets/hypercat_mascot_face_80x68.qoi (1355 bytes, 80x68). Regenerate with
 *   build/tools/png2qoi Docs/resources/hypercat_mascot_face_80x68.png <out>.qoi --mono-invert
 * Embedded so the test is hermetic (no filesystem/asset-path dependency). --- */
static const unsigned char k_face80_qoi[] = {
    0x71, 0x6f, 0x69, 0x66, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x44, 0x04, 0x00, 0xff, 0xff,
    0xff, 0xff, 0x00, 0xfd, 0xfd, 0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc3, 0x31, 0xd5, 0x26, 0xc4,
    0x31, 0xd1, 0x26, 0xc2, 0x31, 0xd5, 0x26, 0x31, 0xc1, 0x26, 0xc1, 0x31, 0xd1, 0x26, 0xc5, 0x31,
    0xd0, 0x26, 0xc1, 0x31, 0xc0, 0x26, 0x31, 0xd4, 0x26, 0xc2, 0x31, 0xc1, 0x26, 0xc1, 0x31, 0xce,
    0x26, 0xc5, 0x31, 0xcf, 0x26, 0xc1, 0x31, 0xc0, 0x26, 0xc2, 0x31, 0xd3, 0x26, 0xc4, 0x31, 0xc1,
    0x26, 0xc1, 0x31, 0xcb, 0x26, 0xc5, 0x31, 0xce, 0x26, 0xc2, 0x31, 0xc1, 0x26, 0xc2, 0x31, 0xd3,
    0x26, 0xc5, 0x31, 0xc1, 0x26, 0xc2, 0x31, 0xc8, 0x26, 0xc5, 0x31, 0xcd, 0x26, 0xc2, 0x31, 0xc1,
    0x26, 0xc1, 0x31, 0x26, 0xc0, 0x31, 0xd3, 0x26, 0xc1, 0x31, 0x26, 0x31, 0xc0, 0x26, 0x31, 0xc1,
    0x26, 0xc2, 0x31, 0xc7, 0x26, 0xcd, 0x31, 0xc4, 0x26, 0xc2, 0x31, 0xc2, 0x26, 0xc1, 0x31, 0x26,
    0xc0, 0x31, 0xd3, 0x26, 0xc1, 0x31, 0xc3, 0x26, 0x31, 0xc1, 0x26, 0xc3, 0x31, 0x26, 0xd6, 0x31,
    0x26, 0xc3, 0x31, 0xc1, 0x26, 0xc2, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xd2, 0x26, 0xc1, 0x31, 0xc5,
    0x26, 0x31, 0xc0, 0x26, 0xe2, 0x31, 0xc1, 0x26, 0xc1, 0x31, 0xc1, 0x26, 0xc0, 0x31, 0xd2, 0x26,
    0xc1, 0x31, 0xc6, 0x26, 0x31, 0xc0, 0x26, 0xe2, 0x31, 0x26, 0x31, 0xc4, 0x26, 0xc0, 0x31, 0xd2,
    0x26, 0xc1, 0x31, 0xc1, 0x26, 0xc5, 0x31, 0xc0, 0x26, 0xe7, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xd2,
    0x26, 0xc1, 0x31, 0xc1, 0x26, 0x31, 0xc3, 0x26, 0xc0, 0x31, 0x26, 0xe2, 0x31, 0xc2, 0x26, 0x31,
    0xc0, 0x26, 0xc0, 0x31, 0xd2, 0x26, 0xc1, 0x31, 0xc2, 0x26, 0x31, 0xc3, 0x26, 0xe3, 0x31, 0xc2,
    0x26, 0x31, 0xc1, 0x26, 0xc0, 0x31, 0xd2, 0x26, 0xc1, 0x31, 0xc2, 0x26, 0xc0, 0x31, 0xc3, 0x26,
    0xe2, 0x31, 0xc2, 0x26, 0x31, 0xc1, 0x26, 0xc0, 0x31, 0xd2, 0x26, 0xc1, 0x31, 0xc1, 0x26, 0xc0,
    0x31, 0xc4, 0x26, 0xe3, 0x31, 0xc2, 0x26, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xd2, 0x26, 0xc1, 0x31,
    0xc0, 0x26, 0x31, 0xc5, 0x26, 0xe5, 0x31, 0xc2, 0x26, 0x31, 0x26, 0xc0, 0x31, 0xd2, 0x26, 0xc1,
    0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc3, 0x26, 0xd2, 0x31, 0x26, 0xd2, 0x31, 0xc0, 0x26, 0x31, 0xc0,
    0x26, 0xc0, 0x31, 0xd2, 0x26, 0xc2, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc1, 0x26, 0xd4, 0x31, 0x26,
    0xce, 0x31, 0xc0, 0x26, 0xc1, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xd3, 0x26, 0xc2, 0x31, 0xc1, 0x26,
    0x31, 0xc0, 0x26, 0xd5, 0x31, 0xc0, 0x26, 0xce, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0xc0,
    0x31, 0xd3, 0x26, 0xc2, 0x31, 0xc0, 0x26, 0x31, 0xc0, 0x26, 0xd7, 0x31, 0xc0, 0x26, 0xcb, 0x31,
    0x26, 0xc0, 0x31, 0xc0, 0x26, 0xc3, 0x31, 0xd4, 0x26, 0xc1, 0x31, 0xc0, 0x26, 0x31, 0x26, 0xc2,
    0x31, 0x26, 0xd3, 0x31, 0xc0, 0x26, 0xcb, 0x31, 0xc0, 0x26, 0x31, 0xc1, 0x26, 0xc2, 0x31, 0xd4,
    0x26, 0xc2, 0x31, 0xc0, 0x26, 0xc1, 0x31, 0xc0, 0x26, 0xc3, 0x31, 0x26, 0xcf, 0x31, 0xc0, 0x26,
    0xc4, 0x31, 0x26, 0xc0, 0x31, 0x26, 0xc1, 0x31, 0xc0, 0x26, 0x31, 0xc1, 0x26, 0xc1, 0x31, 0xd4,
    0x26, 0xc2, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc1, 0x26, 0x31, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0xc3,
    0x31, 0x26, 0xc9, 0x31, 0xc1, 0x26, 0x31, 0x26, 0xc2, 0x31, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0x31,
    0xc1, 0x26, 0x31, 0xc1, 0x26, 0xc1, 0x31, 0xd3, 0x26, 0xc5, 0x31, 0xc4, 0x26, 0x31, 0xc0, 0x26,
    0xc4, 0x31, 0x26, 0xc1, 0x31, 0x26, 0xc1, 0x31, 0x26, 0xc0, 0x31, 0xc2, 0x26, 0x31, 0xc0, 0x26,
    0xc2, 0x31, 0x26, 0xc0, 0x31, 0xc3, 0x26, 0xc1, 0x31, 0xc1, 0x26, 0xc5, 0x31, 0xcd, 0x26, 0xc5,
    0x31, 0xc0, 0x26, 0x31, 0xc3, 0x26, 0xc0, 0x31, 0xc1, 0x26, 0x31, 0xc0, 0x26, 0xc1, 0x31, 0xc0,
    0x26, 0xc0, 0x31, 0x26, 0x31, 0xc3, 0x26, 0x31, 0xc1, 0x26, 0xc1, 0x31, 0xc0, 0x26, 0x31, 0xc4,
    0x26, 0xc9, 0x31, 0xcd, 0x26, 0xc5, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0x26, 0xc0, 0x31, 0xc0, 0x26,
    0xc0, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0x26, 0xc3, 0x31, 0xc0, 0x26, 0x31, 0xcf, 0x26, 0x31, 0xc0,
    0x26, 0xc0, 0x31, 0x26, 0xc8, 0x31, 0xce, 0x26, 0xc5, 0x31, 0x26, 0xc1, 0x31, 0x26, 0xc0, 0x31,
    0x26, 0xc1, 0x31, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0xc3, 0x31, 0xc0, 0x26, 0x31, 0xc6, 0x26, 0x31,
    0xc6, 0x26, 0xc0, 0x31, 0x26, 0xca, 0x31, 0xcf, 0x26, 0xcc, 0x31, 0x26, 0xc4, 0x31, 0x26, 0xc1,
    0x31, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0x31, 0xc2, 0x26, 0x31, 0xc3, 0x26,
    0xc0, 0x31, 0x26, 0xca, 0x31, 0xd2, 0x26, 0xd2, 0x31, 0xc0, 0x26, 0xc1, 0x31, 0x26, 0xc1, 0x31,
    0x26, 0xc0, 0x31, 0x26, 0xc1, 0x31, 0xc1, 0x26, 0x31, 0xc3, 0x26, 0xce, 0x31, 0xd0, 0x26, 0xd7,
    0x31, 0xc1, 0x26, 0xc0, 0x31, 0x26, 0xc4, 0x31, 0xc1, 0x26, 0xc0, 0x31, 0xc3, 0x26, 0xcd, 0x31,
    0xd0, 0x26, 0xd7, 0x31, 0xc1, 0x26, 0xc7, 0x31, 0xc1, 0x26, 0xc0, 0x31, 0xc4, 0x26, 0xcc, 0x31,
    0xcf, 0x26, 0xd7, 0x31, 0xc3, 0x26, 0xc6, 0x31, 0xc1, 0x26, 0x31, 0x26, 0x31, 0xc4, 0x26, 0xcc,
    0x31, 0xce, 0x26, 0xd6, 0x31, 0xc4, 0x26, 0xc6, 0x31, 0xc1, 0x26, 0x31, 0xc0, 0x26, 0x31, 0xc4,
    0x26, 0xcb, 0x31, 0xcd, 0x26, 0xdc, 0x31, 0x26, 0xc6, 0x31, 0xc1, 0x26, 0x31, 0xc1, 0x26, 0x31,
    0xc4, 0x26, 0xca, 0x31, 0xcc, 0x26, 0xe5, 0x31, 0xc1, 0x26, 0x31, 0xc1, 0x26, 0xc1, 0x31, 0xc3,
    0x26, 0xcb, 0x31, 0xc7, 0x26, 0xd9, 0x31, 0xc5, 0x26, 0xc7, 0x31, 0xc1, 0x26, 0x31, 0xc0, 0x26,
    0xc3, 0x31, 0xc3, 0x26, 0xca, 0x31, 0xc8, 0x26, 0xd7, 0x31, 0xc8, 0x26, 0xc1, 0x31, 0x26, 0xc1,
    0x31, 0xc1, 0x26, 0x31, 0x26, 0xc1, 0x31, 0xc1, 0x26, 0x31, 0xc2, 0x26, 0xcb, 0x31, 0xc8, 0x26,
    0xd5, 0x31, 0xc9, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc1, 0x26, 0x31, 0xc0, 0x26, 0x31,
    0xc4, 0x26, 0x31, 0xc2, 0x26, 0xcb, 0x31, 0xc8, 0x26, 0xd3, 0x31, 0xc1, 0x26, 0xc5, 0x31, 0xc4,
    0x26, 0xc0, 0x31, 0xc1, 0x26, 0x31, 0xc3, 0x26, 0xc4, 0x31, 0xc1, 0x26, 0xcc, 0x31, 0xca, 0x26,
    0xcf, 0x31, 0xc0, 0x26, 0xc8, 0x31, 0xc3, 0x26, 0x31, 0xc1, 0x26, 0x31, 0xc3, 0x26, 0xc6, 0x31,
    0xc0, 0x26, 0xce, 0x31, 0xc7, 0x26, 0xd0, 0x31, 0x26, 0xc3, 0x31, 0xc2, 0x26, 0xc1, 0x31, 0xc2,
    0x26, 0x31, 0xc0, 0x26, 0x31, 0xc3, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0xc4, 0x31, 0xc0, 0x26, 0xcc,
    0x31, 0xc8, 0x26, 0xd5, 0x31, 0xc4, 0x26, 0xc1, 0x31, 0xc1, 0x26, 0x31, 0x26, 0x31, 0xc3, 0x26,
    0xc0, 0x31, 0xc2, 0x26, 0x31, 0x26, 0xc2, 0x31, 0x26, 0xc7, 0x31, 0xcd, 0x26, 0xd3, 0x31, 0x26,
    0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0x31, 0x26, 0x31, 0xc1, 0x26, 0xc0, 0x31, 0xc4, 0x26,
    0x31, 0xc0, 0x26, 0x31, 0xc0, 0x26, 0x31, 0xc0, 0x26, 0xcb, 0x31, 0xcc, 0x26, 0xd1, 0x31, 0x26,
    0xc0, 0x31, 0x26, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0x31, 0xcb, 0x26, 0x31, 0x26, 0xc0,
    0x31, 0xc0, 0x26, 0x31, 0xc0, 0x26, 0xcc, 0x31, 0xcb, 0x26, 0xd1, 0x31, 0x26, 0xc0, 0x31, 0x26,
    0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0x31, 0xcb, 0x26, 0x31, 0x26, 0xc0, 0x31, 0xc0, 0x26,
    0x31, 0xc0, 0x26, 0x31, 0x26, 0xca, 0x31, 0xcb, 0x26, 0xd1, 0x31, 0xc0, 0x26, 0x31, 0x26, 0x31,
    0xc0, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0x31, 0xcb, 0x26, 0x31, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0x31,
    0xc0, 0x26, 0x31, 0x26, 0xca, 0x31, 0xcb, 0x26, 0xd1, 0x31, 0xc0, 0x26, 0x31, 0x26, 0x31, 0xc4,
    0x26, 0x31, 0xcb, 0x26, 0x31, 0xc3, 0x26, 0x31, 0xc2, 0x26, 0xca, 0x31, 0xcb, 0x26, 0xd2, 0x31,
    0xc0, 0x26, 0x31, 0x26, 0x31, 0xc2, 0x26, 0x31, 0xd1, 0x26, 0x31, 0xc0, 0x26, 0x31, 0xc0, 0x26,
    0xca, 0x31, 0xcb, 0x26, 0xd2, 0x31, 0xe1, 0x26, 0xca, 0x31, 0xcc, 0x26, 0xd2, 0x31, 0xdf, 0x26,
    0xcb, 0x31, 0xcc, 0x26, 0xc2, 0x31, 0x26, 0xcd, 0x31, 0xd1, 0x26, 0x31, 0xcb, 0x26, 0xcb, 0x31,
    0xcd, 0x26, 0xc1, 0x31, 0x26, 0xce, 0x31, 0xdd, 0x26, 0xcb, 0x31, 0xce, 0x26, 0xc1, 0x31, 0xc0,
    0x26, 0xce, 0x31, 0xdb, 0x26, 0xcc, 0x31, 0xcf, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0xcf, 0x31, 0xdc,
    0x26, 0xc9, 0x31, 0xd1, 0x26, 0xc0, 0x31, 0xc0, 0x26, 0xc9, 0x31, 0xcc, 0x26, 0x31, 0xc2, 0x26,
    0xc0, 0x31, 0xc0, 0x26, 0x31, 0xc9, 0x26, 0xc5, 0x31, 0x26, 0xc1, 0x31, 0xd2, 0x26, 0x31, 0xc0,
    0x26, 0xca, 0x31, 0xcc, 0x26, 0xc2, 0x31, 0xc0, 0x26, 0xc0, 0x31, 0xc9, 0x26, 0xc5, 0x31, 0xc0,
    0x26, 0xc0, 0x31, 0xd7, 0x26, 0xc9, 0x31, 0xdf, 0x26, 0xc5, 0x31, 0x26, 0xc0, 0x31, 0xd9, 0x26,
    0xca, 0x31, 0xdc, 0x26, 0xc5, 0x31, 0xc0, 0x26, 0x31, 0xda, 0x26, 0xcc, 0x31, 0xd8, 0x26, 0xc6,
    0x31, 0xde, 0x26, 0xce, 0x31, 0xd4, 0x26, 0xc8, 0x31, 0xde, 0x26, 0xd0, 0x31, 0xd0, 0x26, 0xca,
    0x31, 0xdd, 0x26, 0xc3, 0x31, 0xc0, 0x26, 0xcd, 0x31, 0xca, 0x26, 0xc6, 0x31, 0x26, 0xc4, 0x31,
    0xdc, 0x26, 0xc3, 0x31, 0xc2, 0x26, 0xdf, 0x31, 0xc1, 0x26, 0xc4, 0x31, 0xe6, 0x26, 0xc2, 0x31,
    0x26, 0xc5, 0x31, 0xca, 0x26, 0xc4, 0x31, 0xc4, 0x26, 0xc3, 0x31, 0xe6, 0x26, 0xc0, 0x31, 0xfd,
    0xfd, 0xfd, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

/* Append the 14-byte header (magic, w, h, channels, colorspace=0) to `buf` at *p. */
static void put_header(unsigned char *buf, size_t *p, uint32_t w, uint32_t h, unsigned char ch)
{
    memcpy(buf + *p, "qoif", 4);
    *p += 4;
    buf[(*p)++] = (unsigned char)(w >> 24);
    buf[(*p)++] = (unsigned char)(w >> 16);
    buf[(*p)++] = (unsigned char)(w >> 8);
    buf[(*p)++] = (unsigned char)(w);
    buf[(*p)++] = (unsigned char)(h >> 24);
    buf[(*p)++] = (unsigned char)(h >> 16);
    buf[(*p)++] = (unsigned char)(h >> 8);
    buf[(*p)++] = (unsigned char)(h);
    buf[(*p)++] = ch;
    buf[(*p)++] = 0; /* colorspace */
}

int main(void)
{
    /* =====================================================================================
     * (a) DECODE CORRECTNESS — hand-built 2x2: RGB, RUN(len 1), RGBA, INDEX(->p0)
     *   p0 = RGB(200,100,50) with alpha carried from the seed prev.a=255 -> (200,100,50,255)
     *   p1 = RUN length 1                                                -> (200,100,50,255)
     *   p2 = RGBA(10,20,30,40)
     *   p3 = INDEX -> slot of p0                                          -> (200,100,50,255)
     * ===================================================================================== */
    {
        unsigned char buf[64];
        size_t        p = 0;
        put_header(buf, &p, 2, 2, 4);
        buf[p++] = 0xfe; /* QOI_OP_RGB  */
        buf[p++] = 200;
        buf[p++] = 100;
        buf[p++] = 50;
        buf[p++] = 0xc0 | 0x00;                            /* QOI_OP_RUN, run-1 = 0 (run length 1) */
        buf[p++] = 0xff;                                   /* QOI_OP_RGBA */
        buf[p++] = 10;
        buf[p++] = 20;
        buf[p++] = 30;
        buf[p++] = 40;
        buf[p++] = 0x00 | (unsigned char)hash_slot(200, 100, 50, 255); /* QOI_OP_INDEX -> p0 */
        /* 8-byte end marker */
        for (int k = 0; k < 7; k++) buf[p++] = 0x00;
        buf[p++] = 0x01;

        unsigned char *out = NULL;
        int            w = 0, h = 0;
        CHECK(hc_image_decode_qoi(buf, p, &out, &w, &h) == 0, "(a) hand-built 2x2 decodes");
        CHECK(w == 2 && h == 2, "(a) dims are 2x2");
        if (out) {
            CHECK(px_eq(out, 0, 200, 100, 50, 255), "(a) p0 = RGB(200,100,50,255)");
            CHECK(px_eq(out, 1, 200, 100, 50, 255), "(a) p1 = RUN repeats p0");
            CHECK(px_eq(out, 2, 10, 20, 30, 40), "(a) p2 = RGBA(10,20,30,40)");
            CHECK(px_eq(out, 3, 200, 100, 50, 255), "(a) p3 = INDEX -> p0");
        }
        hc_image_free(out);
    }

    /* =====================================================================================
     * (a') DECODE CORRECTNESS — hand-built 2x2 exercising DIFF + LUMA (the delta ops):
     *   p0 = RGBA(100,100,100,255)
     *   p1 = DIFF dr=+1,dg=0,db=-1 -> (101,100,99,255)
     *   p2 = LUMA byte1=(0x80|dg+32) with dg=10, byte2=((dr-dg+8)<<4)|(db-dg+8) with the high nibble 6
     *        and low nibble 11 -> reference qoi.h decodes this to (109,110,112,255) (alpha carried)
     *   p3 = INDEX -> p0
     * The p2 expected value is taken from the canonical reference decoder (the authoritative oracle), not
     * hand-derived, so the LUMA channel arithmetic is checked against the spec implementation. =========
     */
    {
        unsigned char buf[64];
        size_t        p = 0;
        put_header(buf, &p, 2, 2, 4);
        buf[p++] = 0xff; /* RGBA p0 */
        buf[p++] = 100;
        buf[p++] = 100;
        buf[p++] = 100;
        buf[p++] = 255;
        /* DIFF: tag 01; (dr+2)<<4 | (dg+2)<<2 | (db+2); dr=1,dg=0,db=-1 -> (3<<4)|(2<<2)|1 = 0x39 */
        buf[p++] = (unsigned char)(0x40 | (3 << 4) | (2 << 2) | 1);
        /* LUMA: byte1 = 0x80 | (dg+32); dg=10 -> 0x80|42. byte2 = ((dr_dg+8)<<4)|(db_dg+8);
         * dr_dg=-2 -> 6, db_dg=+3 -> 11 -> (6<<4)|11 = 0x6b */
        buf[p++] = (unsigned char)(0x80 | (10 + 32));
        buf[p++] = (unsigned char)((6 << 4) | 11);
        buf[p++] = 0x00 | (unsigned char)hash_slot(100, 100, 100, 255); /* INDEX -> p0 */
        for (int k = 0; k < 7; k++) buf[p++] = 0x00;
        buf[p++] = 0x01;

        unsigned char *out = NULL;
        int            w = 0, h = 0;
        CHECK(hc_image_decode_qoi(buf, p, &out, &w, &h) == 0, "(a') DIFF+LUMA 2x2 decodes");
        if (out) {
            CHECK(px_eq(out, 0, 100, 100, 100, 255), "(a') p0 = RGBA(100,100,100,255)");
            CHECK(px_eq(out, 1, 101, 100, 99, 255), "(a') p1 = DIFF(+1,0,-1)");
            CHECK(px_eq(out, 2, 109, 110, 112, 255), "(a') p2 = LUMA (ref-decoded oracle value)");
            CHECK(px_eq(out, 3, 100, 100, 100, 255), "(a') p3 = INDEX -> p0");
        }
        hc_image_free(out);
    }

    /* =====================================================================================
     * (b) ROUND-TRIP — the real png2qoi --mono-invert output for the smallest mascot asset.
     * Decodes to 80x68 with the documented monochrome-on-transparent property. ============
     */
    {
        unsigned char *out = NULL;
        int            w = 0, h = 0;
        CHECK(hc_image_decode_qoi(k_face80_qoi, sizeof k_face80_qoi, &out, &w, &h) == 0,
              "(b) mascot face_80x68 .qoi decodes");
        CHECK(w == 80 && h == 68, "(b) round-trip dims are 80x68");
        if (out) {
            /* corner (0,0) is the transparent background (mono-invert keeps a==0) */
            CHECK(px_eq(out, 0, 255, 255, 255, 0), "(b) corner is transparent white (mono-invert bg)");
            /* a fully-opaque white interior pixel exists (a black source line became opaque white).
             * Scan for one: white RGB with alpha 255. */
            int found_opaque_white = 0;
            for (long i = 0; i < (long)w * h; i++) {
                if (px_eq(out, (size_t)i, 255, 255, 255, 255)) {
                    found_opaque_white = 1;
                    break;
                }
            }
            CHECK(found_opaque_white, "(b) an opaque white line pixel exists (black source -> white)");
            /* every pixel is white RGB (mono output is white-on-transparent: only alpha varies) */
            int all_white_rgb = 1;
            for (long i = 0; i < (long)w * h; i++) {
                if (out[i * 4 + 0] != 255 || out[i * 4 + 1] != 255 || out[i * 4 + 2] != 255) {
                    all_white_rgb = 0;
                    break;
                }
            }
            CHECK(all_white_rgb, "(b) all RGB is white (mono output varies only in alpha)");
        }
        hc_image_free(out);
    }

    /* =====================================================================================
     * (b') POSITIVE BOUNDARIES — the success edges opposite the (c) rejections:
     *   - a RUN that EXACTLY fills w*h (spanning rows) must succeed (not be off-by-one rejected);
     *   - a stream that ends at w*h with NO 8-byte end marker must still succeed (the marker is
     *     tolerated, not required, per the banner). ========================================
     */
    {
        /* 2x2 = 4 pixels, all identical, as ONE run of length 4 (run-1 = 3). RGB sets the first pixel
         * then... actually a RUN repeats `prev` (seed = opaque black), so a bare RUN(4) on a fresh
         * stream emits 4 opaque-black pixels. Include the end marker here. */
        unsigned char buf[32];
        size_t        p = 0;
        put_header(buf, &p, 2, 2, 4);
        buf[p++] = 0xc0 | 0x03; /* RUN, run-1 = 3 -> length 4, exactly fills the 4-pixel image */
        for (int k = 0; k < 7; k++) buf[p++] = 0x00;
        buf[p++] = 0x01;
        unsigned char *out = NULL;
        int            w = 0, h = 0;
        CHECK(hc_image_decode_qoi(buf, p, &out, &w, &h) == 0, "(b') RUN exactly filling w*h decodes");
        CHECK(w == 2 && h == 2, "(b') exact-fill dims are 2x2");
        if (out) {
            int all_black = 1;
            for (int i = 0; i < 4; i++)
                if (!px_eq(out, (size_t)i, 0, 0, 0, 255)) all_black = 0;
            CHECK(all_black, "(b') the exact-fill RUN emitted 4 opaque-black pixels (prev seed)");
        }
        hc_image_free(out);

        /* Same payload but truncated BEFORE the end marker: ends exactly at the last pixel. Must succeed
         * (the end marker is optional). */
        unsigned char *out2 = NULL;
        int            w2 = 0, h2 = 0;
        size_t         no_marker_len = 14 + 1; /* header + the single RUN byte, no end marker */
        CHECK(hc_image_decode_qoi(buf, no_marker_len, &out2, &w2, &h2) == 0,
              "(b') a stream ending at w*h with NO end marker still decodes");
        CHECK(w2 == 2 && h2 == 2, "(b') no-end-marker dims are 2x2");
        hc_image_free(out2);
    }

    /* =====================================================================================
     * (c) ROBUSTNESS / FUZZ — every malformed input returns -1, sets *out=NULL, no crash/overread/leak.
     * ASan/UBSan converts any overread or leak on these paths into a hard failure. ==========
     */
    {
        unsigned char *out = (unsigned char *)0x1; /* poison: must be reset to NULL on failure */
        int            w = 7, h = 7;               /* poison */

        /* bad magic */
        {
            unsigned char b[14];
            memset(b, 0, sizeof b);
            memcpy(b, "qXif", 4);
            b[7] = 2;
            b[11] = 2;
            b[12] = 4;
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, sizeof b, &out, &w, &h) == -1, "(c) bad magic -> -1");
            CHECK(out == NULL, "(c) bad magic leaves *out NULL");
        }

        /* truncated header (13 bytes) */
        {
            unsigned char b[13];
            memcpy(b, "qoif", 4);
            memset(b + 4, 0, sizeof b - 4);
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, sizeof b, &out, &w, &h) == -1, "(c) truncated header -> -1");
            CHECK(out == NULL, "(c) truncated header leaves *out NULL");
        }

        /* truncated mid-chunk: a valid 1x1 header that promises a pixel but the RGB chunk is cut off */
        {
            unsigned char b[16];
            size_t        p = 0;
            put_header(b, &p, 1, 1, 4);
            b[p++] = 0xfe; /* RGB op announced... */
            b[p++] = 200;  /* ...but only 1 of the 3 colour bytes present, then EOF */
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, p, &out, &w, &h) == -1, "(c) truncated RGB chunk -> -1");
            CHECK(out == NULL, "(c) truncated RGB chunk leaves *out NULL (no overread)");
        }

        /* truncated mid-chunk: RGBA op with missing alpha byte */
        {
            unsigned char b[20];
            size_t        p = 0;
            put_header(b, &p, 1, 1, 4);
            b[p++] = 0xff; /* RGBA op */
            b[p++] = 1;
            b[p++] = 2;
            b[p++] = 3; /* alpha byte missing, then EOF */
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, p, &out, &w, &h) == -1, "(c) truncated RGBA chunk -> -1");
            CHECK(out == NULL, "(c) truncated RGBA chunk leaves *out NULL");
        }

        /* truncated mid-chunk: LUMA op (needs 2 bytes) with the second byte missing */
        {
            unsigned char b[20];
            size_t        p = 0;
            put_header(b, &p, 1, 1, 4);
            b[p++] = 0x80 | 10; /* LUMA op, first byte only, then EOF */
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, p, &out, &w, &h) == -1, "(c) truncated LUMA chunk -> -1");
            CHECK(out == NULL, "(c) truncated LUMA chunk leaves *out NULL");
        }

        /* oversized dims (0x10000 x 0x10000) — the product * 4 would be 2^34, well past any sane buffer.
         * These are rejected by the per-axis dimension cap (each axis > HC_IMAGE_MAX_DIM) BEFORE any
         * multiply or alloc; the 64-bit byte-count math is the overflow backstop behind that cap. */
        {
            unsigned char b[14];
            size_t        p = 0;
            put_header(b, &p, 0x10000u, 0x10000u, 4);
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, p, &out, &w, &h) == -1, "(c) oversized dims -> -1");
            CHECK(out == NULL, "(c) oversized dims leave *out NULL (no alloc)");
        }

        /* dimension just over the per-axis cap (HC_IMAGE_MAX_DIM+1) */
        {
            unsigned char b[14];
            size_t        p = 0;
            put_header(b, &p, HC_IMAGE_MAX_DIM + 1u, 1, 4);
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, p, &out, &w, &h) == -1, "(c) width over cap -> -1");
            CHECK(out == NULL, "(c) over-cap width leaves *out NULL");
        }

        /* a single byte of input */
        {
            unsigned char b[1] = {0x71};
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, 1, &out, &w, &h) == -1, "(c) 1-byte input -> -1");
            CHECK(out == NULL, "(c) 1-byte input leaves *out NULL");
        }

        /* an all-zero 14+ byte buffer (magic mismatch — zeros are not 'qoif') */
        {
            unsigned char b[32];
            memset(b, 0, sizeof b);
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, sizeof b, &out, &w, &h) == -1, "(c) all-zero buffer -> -1");
            CHECK(out == NULL, "(c) all-zero buffer leaves *out NULL");
        }

        /* channels = 2 (only 3 or 4 are valid) */
        {
            unsigned char b[14];
            size_t        p = 0;
            put_header(b, &p, 1, 1, 2); /* channels byte = 2 */
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, p, &out, &w, &h) == -1, "(c) channels=2 -> -1");
            CHECK(out == NULL, "(c) channels=2 leaves *out NULL");
        }

        /* width = 0 */
        {
            unsigned char b[14];
            size_t        p = 0;
            put_header(b, &p, 0, 4, 4);
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, p, &out, &w, &h) == -1, "(c) width=0 -> -1");
            CHECK(out == NULL, "(c) width=0 leaves *out NULL");
        }

        /* a RUN that overshoots the pixel count: 1x1 image, RUN length 5 -> would emit past w*h */
        {
            unsigned char b[24];
            size_t        p = 0;
            put_header(b, &p, 1, 1, 4);
            b[p++] = 0xc0 | 0x04; /* RUN, run-1 = 4 -> length 5, but only 1 pixel fits */
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, p, &out, &w, &h) == -1, "(c) RUN past w*h -> -1");
            CHECK(out == NULL, "(c) over-long RUN leaves *out NULL (no overwrite)");
        }

        /* a NULL out-param (and NULL data) */
        {
            unsigned char b[14];
            size_t        p = 0;
            put_header(b, &p, 1, 1, 4);
            CHECK(hc_image_decode_qoi(b, p, NULL, &w, &h) == -1, "(c) NULL out-param -> -1");
            CHECK(hc_image_decode_qoi(NULL, 14, &out, &w, &h) == -1, "(c) NULL data -> -1");
        }

        /* zero length */
        {
            unsigned char b[1] = {0};
            out = (unsigned char *)0x1;
            CHECK(hc_image_decode_qoi(b, 0, &out, &w, &h) == -1, "(c) zero length -> -1");
            CHECK(out == NULL, "(c) zero length leaves *out NULL");
        }

        /* hc_image_free(NULL) is a no-op (must not crash) */
        hc_image_free(NULL);
    }

    /* --- W4 P4.0b: PNG/JPEG decode (hc_image_decode_auto) + the untrusted-byte fuzz battery --- */
    {
        unsigned char *out = NULL;
        int            w = 0, h = 0;
        CHECK(hc_image_decode_auto(k_png_2x2, sizeof k_png_2x2, &out, &w, &h) == 0, "P4.0b: a valid PNG decodes");
        CHECK(w == 2 && h == 2 && out != NULL, "P4.0b: the decoded dims are 2x2");
        hc_image_free(out);
        out = NULL;

        CHECK(hc_image_decode_auto(NULL, 10, &out, &w, &h) == -1 && out == NULL, "P4.0b: NULL data -> -1");
        CHECK(hc_image_decode_auto(k_png_2x2, 0, &out, &w, &h) == -1 && out == NULL, "P4.0b: zero len -> -1");
        CHECK(hc_image_decode_auto(k_png_2x2, sizeof k_png_2x2, NULL, &w, &h) == -1, "P4.0b: NULL out -> -1");

        unsigned char junk[64];
        memset(junk, 0xAB, sizeof junk);
        CHECK(hc_image_decode_auto(junk, sizeof junk, &out, &w, &h) == -1 && out == NULL, "P4.0b: garbage -> -1");
        memset(junk, 0, sizeof junk);
        CHECK(hc_image_decode_auto(junk, sizeof junk, &out, &w, &h) == -1 && out == NULL, "P4.0b: zeros -> -1");

        /* every truncated prefix: stb may REJECT (-1) or leniently decode a still-valid prefix (a missing IEND
         * is tolerated) — either is fine; the security property is NO overread/crash (ASan) + a bounded output. */
        for (size_t p = 1; p < sizeof k_png_2x2; p++) {
            out = NULL;
            int r = hc_image_decode_auto(k_png_2x2, p, &out, &w, &h);
            if (r == 0) {
                CHECK((unsigned)w <= HC_IMAGE_MAX_DIM && (unsigned)h <= HC_IMAGE_MAX_DIM,
                      "P4.0b: a truncated-but-decodable PNG stays within the dim cap");
                hc_image_free(out);
            } else {
                CHECK(r == -1 && out == NULL, "P4.0b: a rejected truncated prefix leaves *out NULL");
            }
        }

        /* an over-cap dimension is rejected PRE-decode (the stbi_info probe + the cap) */
        {
            unsigned char big[sizeof k_png_2x2];
            memcpy(big, k_png_2x2, sizeof big);
            big[16] = 0x00; /* IHDR width (BE u32 at offset 16) -> 0x00010000 = 65536 > HC_IMAGE_MAX_DIM */
            big[17] = 0x01;
            big[18] = 0x00;
            big[19] = 0x00;
            out = NULL;
            CHECK(hc_image_decode_auto(big, sizeof big, &out, &w, &h) == -1 && out == NULL,
                  "P4.0b: an over-cap dimension is rejected pre-malloc");
        }

        /* FUZZ 1: single-bit mutations of the valid PNG never crash/leak/overread (ASan) */
        {
            unsigned char m[sizeof k_png_2x2];
            for (size_t i = 0; i < sizeof k_png_2x2; i++)
                for (int bit = 0; bit < 8; bit += 3) {
                    memcpy(m, k_png_2x2, sizeof m);
                    m[i] ^= (unsigned char)(1u << bit);
                    out = NULL;
                    if (hc_image_decode_auto(m, sizeof m, &out, &w, &h) == 0) {
                        CHECK((unsigned)w <= HC_IMAGE_MAX_DIM && (unsigned)h <= HC_IMAGE_MAX_DIM,
                              "P4.0b: a mutated-but-valid PNG stays within the dim cap");
                        hc_image_free(out);
                    }
                }
        }

        /* FUZZ 2: pseudo-random byte buffers (deterministic PRNG) never crash/leak */
        {
            uint32_t s = 0x1234567u;
            for (int it = 0; it < 4000; it++) {
                unsigned char buf[128];
                size_t        n = (s % 128u) + 1u;
                for (size_t i = 0; i < n; i++) {
                    s = s * 1103515245u + 12345u;
                    buf[i] = (unsigned char)(s >> 16);
                }
                out = NULL;
                if (hc_image_decode_auto(buf, n, &out, &w, &h) == 0) hc_image_free(out);
            }
        }
    }

    if (g_fails == 0) printf("test_hc_image: OK\n");
    return g_fails ? 1 : 0;
}
