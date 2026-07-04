/* hc_image_stb — the PNG/JPEG decode path (W4 P4.0b). This TU ISOLATES the vendored stb_image.h (public
 * domain) — the only place its ~8k lines compile — and wraps it with HyperCat's hardening for UNTRUSTED
 * on-disk bytes (the media panel / file viewer). The defenses, in order:
 *   - only the PNG + JPEG parsers are compiled (STBI_ONLY_*) — the other format decoders are not attack surface;
 *   - NO stdio (STBI_NO_STDIO) — decode from a caller-bounded memory buffer, never a path stb opens itself;
 *   - failure strings off (STBI_NO_FAILURE_STRINGS) — no error-string formatting on hostile input;
 *   - stb's own dimension guard set to our cap (STBI_MAX_DIMENSIONS) — stb rejects oversized dims internally too;
 *   - asserts disabled (STBI_ASSERT no-op) — a malformed input must never abort() the host;
 *   - an stbi_info PROBE reads w/h WITHOUT decoding, so we reject an oversized image BEFORE the decode allocates;
 *   - the decoded dims must equal the probe — no partial / over-cap output is ever returned.
 * Threading: stateless + reentrant (stb keeps no global state we use). The buffer is malloc'd; hc_image_free
 * (a plain free) releases it, paired with stb's default STBI_MALLOC. FUZZED via the test's malformed-input
 * battery + the libFuzzer entry point below. */

#include "hc_image.h"

#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS HC_IMAGE_MAX_DIM
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <limits.h>
#include <stdint.h>

int hc_image_decode_auto(const unsigned char *data, size_t len, unsigned char **out, int *w, int *h)
{
    if (out) *out = NULL; /* pin every out-param up front so every early return leaves a defined state */
    if (w) *w = 0;
    if (h) *h = 0;
    if (!data || !out || !w || !h || len == 0 || len > (size_t)INT_MAX) return -1;

    /* PROBE: dimensions WITHOUT decoding -> reject oversized BEFORE stb allocates the pixel buffer. */
    int iw = 0, ih = 0, ic = 0;
    if (!stbi_info_from_memory(data, (int)len, &iw, &ih, &ic)) return -1;
    if (iw <= 0 || ih <= 0 || (unsigned)iw > HC_IMAGE_MAX_DIM || (unsigned)ih > HC_IMAGE_MAX_DIM) return -1;
    if ((uint64_t)iw * (uint64_t)ih * 4u > (uint64_t)HC_IMAGE_MAX_BYTES) return -1;

    /* DECODE forcing 4 channels (RGBA8, the texture format). */
    int            dw = 0, dh = 0, dc = 0;
    unsigned char *px = stbi_load_from_memory(data, (int)len, &dw, &dh, &dc, 4);
    if (!px) return -1;
    /* Defensive: the decode must agree with the probe and stay within the caps (no partial/over-cap output). */
    if (dw != iw || dh != ih || dw <= 0 || dh <= 0 || (unsigned)dw > HC_IMAGE_MAX_DIM ||
        (unsigned)dh > HC_IMAGE_MAX_DIM) {
        stbi_image_free(px);
        return -1;
    }
    *out = px;
    *w = dw;
    *h = dh;
    return 0;
}

#ifdef HC_IMAGE_FUZZ
/* libFuzzer entry point (built with -DHC_IMAGE_FUZZ + a fuzzing toolchain): feed arbitrary bytes to the
 * decoder + free any output. The harness asserts only "no crash / no leak / no overread" — ASan/UBSan +
 * libFuzzer enforce that. */
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    unsigned char *out = NULL;
    int            w = 0, h = 0;
    if (hc_image_decode_auto(data, size, &out, &w, &h) == 0) hc_image_free(out);
    return 0;
}
#endif
