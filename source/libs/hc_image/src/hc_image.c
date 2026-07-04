/* hc_image — QOI decoder implementation. See hc_image.h for the contract, ownership, and the threat model.
 *
 * Reference: the QOI specification (qoiformat.org / the 1-page spec). A QOI file is a 14-byte header, a
 * sequence of chunks, then an 8-byte end marker (00 00 00 00 00 00 00 01). Decoding maintains two pieces
 * of state that the encoder mirrored: `prev`, the previously emitted pixel (seeded to {0,0,0,255}), and a
 * 64-entry running array `index` addressed by the pixel hash (r*3 + g*5 + b*7 + a*11) & 63 (the array is
 * zero-initialised, matching the spec — a real decoder and encoder agree on that seed). Each chunk either
 * names a pixel directly (RGB/RGBA), references an index slot, applies a small delta to `prev`, or repeats
 * `prev` `run` times. We emit exactly w*h pixels; anything that would read past `len` or write past w*h is
 * malformed and aborts with -1 (no partial output).
 *
 * Defensiveness lives in two helpers used on EVERY access: a read cursor checked against `len`, and the
 * pre-allocation bound check on w*h*4. There is no recursion and no global state. */

#include "hc_image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- QOI chunk tags (from the spec) --- */
#define QOI_OP_RGB   0xfeu /* 8-bit tag: full RGB, alpha = prev.a               */
#define QOI_OP_RGBA  0xffu /* 8-bit tag: full RGBA                              */
#define QOI_OP_INDEX 0x00u /* 2-bit tag 00: index into the running array        */
#define QOI_OP_DIFF  0x40u /* 2-bit tag 01: -2..1 difference on r,g,b           */
#define QOI_OP_LUMA  0x80u /* 2-bit tag 10: green diff + r/b relative to green   */
#define QOI_OP_RUN   0xc0u /* 2-bit tag 11: repeat prev (run+1, bias of -1)      */
#define QOI_MASK_2   0xc0u /* mask to extract the 2-bit tag                      */

#define QOI_HEADER_SIZE 14u

typedef struct {
    unsigned char r, g, b, a;
} qoi_rgba;

/* The running-array slot for a pixel — the spec's index hash, kept in ONE place so encode/decode agree. */
static unsigned qoi_hash(qoi_rgba p)
{
    return (unsigned)(p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) & 63u;
}

/* Read a 32-bit big-endian value at `*pos` (4 bytes). Caller has already bounds-checked the 14-byte header,
 * so this is only ever invoked within it. Advances `*pos`. */
static uint32_t qoi_read_u32_be(const unsigned char *data, size_t *pos)
{
    uint32_t v = ((uint32_t)data[*pos] << 24) | ((uint32_t)data[*pos + 1] << 16) |
                 ((uint32_t)data[*pos + 2] << 8) | (uint32_t)data[*pos + 3];
    *pos += 4;
    return v;
}

int hc_image_decode_qoi(const unsigned char *data, size_t len, unsigned char **out, int *w, int *h)
{
    /* Pin every out-param to a defined failure value up front, so every early `return -1` already leaves
     * the caller in the documented "no partial output" state without per-branch cleanup. */
    if (out) *out = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!data || !out || !w || !h) return -1;

    /* --- header: 14 bytes minimum, validated BEFORE we trust any field --- */
    if (len < QOI_HEADER_SIZE) return -1;
    if (memcmp(data, "qoif", 4) != 0) return -1; /* bad magic */

    size_t   pos = 4;
    uint32_t width = qoi_read_u32_be(data, &pos);
    uint32_t height = qoi_read_u32_be(data, &pos);
    unsigned channels = data[pos++];   /* 3 or 4 */
    unsigned colorspace = data[pos++]; /* informational only; 0 (sRGB) or 1 (linear) per spec */
    (void)colorspace;                  /* we always produce RGBA8; the source colorspace tag is not used */

    if (channels != 3 && channels != 4) return -1; /* reject channels=2 and any other value */
    if (width == 0 || height == 0) return -1;

    /* --- bound the pixel count BEFORE allocating (anti-overflow, anti-OOM-by-design) --- */
    if (width > HC_IMAGE_MAX_DIM || height > HC_IMAGE_MAX_DIM) return -1;
    /* 64-bit multiply so a crafted w*h cannot wrap size_t; compare against the byte cap. With the current
     * HC_IMAGE_MAX_DIM (8192) this byte check never fires on its own — 8192*8192*4 == HC_IMAGE_MAX_BYTES
     * exactly, so the per-axis cap above already bounds us. It is kept as deliberate defense in depth: the
     * overflow-safe ceiling that still binds if HC_IMAGE_MAX_DIM is ever raised. */
    uint64_t px_count = (uint64_t)width * (uint64_t)height;
    uint64_t byte_count = px_count * 4u;
    if (byte_count > (uint64_t)HC_IMAGE_MAX_BYTES) return -1;

    unsigned char *rgba = (unsigned char *)malloc((size_t)byte_count);
    if (!rgba) return -1; /* OOM: -1, *out already NULL */

    /* --- decode state (spec): zeroed index array, prev seeded to opaque black --- */
    qoi_rgba index[64];
    memset(index, 0, sizeof index);
    qoi_rgba px = {0, 0, 0, 255};

    uint64_t produced = 0; /* pixels emitted so far; must reach exactly px_count */
    size_t   p = pos;      /* read cursor into the chunk stream (just past the header) */

    while (produced < px_count) {
        /* A chunk is at least one byte. If the stream ends before we have all the pixels, it is a
         * truncated file -> malformed. (The end marker only appears AFTER the final pixel.) */
        if (p >= len) {
            free(rgba);
            return -1;
        }
        unsigned char b1 = data[p++];

        if (b1 == QOI_OP_RGB) {
            /* 3 more bytes: r,g,b (alpha carries over from prev). Bounds-check the trailing bytes. */
            if (p + 3 > len) {
                free(rgba);
                return -1;
            }
            px.r = data[p++];
            px.g = data[p++];
            px.b = data[p++];
            /* px.a unchanged */
            index[qoi_hash(px)] = px;
        } else if (b1 == QOI_OP_RGBA) {
            if (p + 4 > len) {
                free(rgba);
                return -1;
            }
            px.r = data[p++];
            px.g = data[p++];
            px.b = data[p++];
            px.a = data[p++];
            index[qoi_hash(px)] = px;
        } else {
            unsigned tag = b1 & QOI_MASK_2;
            if (tag == QOI_OP_INDEX) {
                /* The 6 low bits ARE the slot; no extra byte to read, no bounds risk. The spec forbids an
                 * INDEX that resolves to the just-emitted pixel mid-stream, but a decoder need not reject
                 * it — it simply re-emits that slot. */
                px = index[b1 & 0x3fu];
                /* (re-storing index[qoi_hash(px)] = px is a no-op here; skipped) */
            } else if (tag == QOI_OP_DIFF) {
                /* Three 2-bit signed channel diffs with a bias of 2 (stored 0..3 => -2..1), wrapping. */
                int dr = ((b1 >> 4) & 0x03) - 2;
                int dg = ((b1 >> 2) & 0x03) - 2;
                int db = (b1 & 0x03) - 2;
                px.r = (unsigned char)(px.r + dr);
                px.g = (unsigned char)(px.g + dg);
                px.b = (unsigned char)(px.b + db);
                /* px.a unchanged */
                index[qoi_hash(px)] = px;
            } else if (tag == QOI_OP_LUMA) {
                /* 2 bytes total. Byte1 low 6 bits = green diff (bias 32, -32..31). Byte2 high nibble =
                 * (dr - dg) (bias 8), low nibble = (db - dg) (bias 8). Wrapping 8-bit arithmetic. */
                if (p >= len) {
                    free(rgba);
                    return -1;
                }
                unsigned char b2 = data[p++];
                int dg = (int)(b1 & 0x3fu) - 32;
                int dr = ((b2 >> 4) & 0x0f) - 8 + dg;
                int db = (b2 & 0x0f) - 8 + dg;
                px.r = (unsigned char)(px.r + dr);
                px.g = (unsigned char)(px.g + dg);
                px.b = (unsigned char)(px.b + db);
                /* px.a unchanged */
                index[qoi_hash(px)] = px;
            } else { /* QOI_OP_RUN */
                /* Repeat prev (run+1) times; the count is the low 6 bits with a bias of -1 (so 1..62; the
                 * values 63/64 are stolen by the RGB/RGBA tags and never appear here). A run that would
                 * push past px_count is malformed -> abort (we do NOT silently clamp a corrupt stream). */
                unsigned run = (unsigned)(b1 & 0x3fu) + 1u;
                if ((uint64_t)run > px_count - produced) {
                    free(rgba);
                    return -1;
                }
                for (unsigned i = 0; i < run; i++) {
                    size_t o = (size_t)(produced * 4u);
                    rgba[o + 0] = px.r;
                    rgba[o + 1] = px.g;
                    rgba[o + 2] = px.b;
                    rgba[o + 3] = px.a;
                    produced++;
                }
                continue; /* a RUN emitted its own pixels; skip the single-pixel emit below */
            }
        }

        /* All non-RUN ops produce exactly one pixel; `produced < px_count` is the loop guard, so this
         * write is always in-bounds. */
        size_t o = (size_t)(produced * 4u);
        rgba[o + 0] = px.r;
        rgba[o + 1] = px.g;
        rgba[o + 2] = px.b;
        rgba[o + 3] = px.a;
        produced++;
    }

    /* We emitted exactly px_count pixels. The trailing 8-byte end marker is tolerated but not required:
     * we neither demand it nor validate its contents, and any bytes after the last pixel are ignored. A
     * stream that under-delivered pixels already failed above; one that over-delivers simply stopped here
     * at the pixel cap (extra chunk bytes are harmless trailing data). */
    *out = rgba;
    *w = (int)width;
    *h = (int)height;
    return 0;
}

void hc_image_free(unsigned char *rgba)
{
    free(rgba);
}
