#ifndef HC_IMAGE_H
#define HC_IMAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_image — the SHIPPED, security-critical image decoders: bytes -> RGBA8. TWO paths, both defensive:
 *   - QOI (hc_image_decode_qoi, WI-5): the in-house decoder for the app's TRUSTED bundled assets (the mascot
 *     sprites png2qoi converts at dev time + the app embeds). Zero third-party code on this path.
 *   - PNG/JPEG (hc_image_decode_auto, W4 P4.0b): decodes UNTRUSTED on-disk images the operator opens in the
 *     media panel / file viewer, via the vendored third_party/stb_image.h (PNG+JPEG parsers ONLY, no stdio,
 *     failure strings off, dims hard-capped). This DELIBERATELY adds a runtime image-parser dependency that the
 *     QOI-only posture avoided — accepted for P4.0b because the media feature needs real formats; the surface is
 *     bounded by the pre-malloc probe + the format restriction + a fuzz battery (see hc_image_decode_auto).
 * It knows nothing of files, GL textures, or the asset names — the caller (app/ui) owns loading the bytes
 * (UNTRUSTED images via hc_fs_read_file with a size cap) and uploading the result.
 *
 * Purpose:   decode a complete QOI byte stream into a freshly malloc'd, tightly-packed RGBA8 buffer
 *            (w*h pixels, 4 bytes each, row-major, no padding). A single forward pass, O(pixels), no
 *            recursion, no global state.
 * Owns:      nothing persistent. On success hc_image_decode_qoi hands the caller a malloc'd buffer it
 *            MUST release with hc_image_free (which is just free(); the separate entry point keeps the
 *            allocator paired across a future ABI/allocator change and documents the ownership transfer).
 * Threading: stateless + reentrant. The running index array and previous-pixel cursor live on the stack
 *            of each call, so concurrent decodes of different inputs are independent and lock-free.
 * Security:  the decoder is the NEW attack surface, so it is defensive even though v1 only feeds it
 *            trusted bundled assets. EVERY byte read is bounds-checked against `len` (a truncated header
 *            or a truncated RGB/RGBA/LUMA chunk returns -1 — never an overread). The 14-byte header is
 *            validated (magic "qoif", channels in {3,4}, w>0, h>0) BEFORE anything else, and the pixel
 *            count is bounded BEFORE allocating: w,h each <= HC_IMAGE_MAX_DIM AND (uint64) w*h*4 <=
 *            HC_IMAGE_MAX_BYTES, the multiply done in 64-bit so it cannot overflow size_t. (At the current
 *            HC_IMAGE_MAX_DIM the per-axis cap is the binding constraint — 8192*8192*4 == 256 MiB; the
 *            byte cap is the overflow/raise-the-dim-cap backstop, deliberate defense in depth.) A RUN (or
 *            any op) that would emit past w*h pixels is malformed -> -1. On ANY failure there is
 *            no partial output: *out is set to NULL and nothing is leaked. The 8-byte end marker is
 *            tolerated/skipped, not required (a stream that simply ends at w*h pixels still succeeds).
 * Returns:   0 on success (with *out, *w and *h set); -1 on ANY malformed / truncated / oversized input,
 *            on a NULL out-param, or on allocation failure (and *out left NULL). */

#define HC_IMAGE_MAX_DIM   8192u                       /* per-axis cap: rejects an absurd width/height   */
#define HC_IMAGE_MAX_BYTES (256u * 1024u * 1024u)      /* RGBA8 buffer cap (256 MiB) checked pre-malloc  */

/* Decode QOI `data` (`len` bytes) into a malloc'd RGBA8 buffer. On success returns 0, sets *out to the
 * buffer (caller frees with hc_image_free), and *w and *h to the dimensions. On ANY malformed,
 * truncated or oversized input, a NULL data/out/w/h, or OOM, returns -1 and sets *out to NULL. */
int hc_image_decode_qoi(const unsigned char *data, size_t len, unsigned char **out, int *w, int *h);

/* Decode a PNG or JPEG byte stream (auto-detected) into a malloc'd RGBA8 buffer — W4 P4.0b, for the media
 * panel / file viewer showing UNTRUSTED on-disk images. Backed by the vendored stb_image.h (PNG+JPEG parsers
 * only, NO stdio, failure strings off), but hardened around it: an stbi_info PROBE reads the dimensions WITHOUT
 * decoding and rejects w/h <= 0 or > HC_IMAGE_MAX_DIM or w*h*4 > HC_IMAGE_MAX_BYTES BEFORE the decode allocates
 * (stb itself also caps dims via STBI_MAX_DIMENSIONS), and the decoded dims must match the probe (no partial /
 * over-cap output). On success returns 0, *out = the buffer (caller frees with hc_image_free — same free() as
 * the QOI path), *w and *h set. On ANY malformed/truncated/oversized input or OOM: -1 and *out = NULL. The caller
 * must bound `len` itself (read the file through hc_fs_read_file with a size cap). FUZZED. NOTE: the byte cap
 * bounds the OUTPUT RGBA8 buffer (w*h*4); a 16-bit or interlaced PNG transiently allocates a small multiple of
 * that inside the decode (overflow-checked, bounded by the dim cap) — peak RSS for an in-cap image can be a few
 * hundred MiB, so a future consumer that cares about memory pressure may want a tighter ceiling. */
int hc_image_decode_auto(const unsigned char *data, size_t len, unsigned char **out, int *w, int *h);

/* Release a buffer returned by hc_image_decode_qoi OR hc_image_decode_auto. NULL is a no-op. */
void hc_image_free(unsigned char *rgba);

#ifdef __cplusplus
}
#endif
#endif /* HC_IMAGE_H */
