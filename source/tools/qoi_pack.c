/* qoi_pack — DEV-TIME spritesheet packer (NOT linked into the app; see tools/CMakeLists.txt;
 * WI-5 / Conductor P5-S0). A manifest-driven sibling of png2qoi: it reads a TSV manifest of sheets, decodes
 * each source PNG (via the vendored, dev-only stb_image.h), optionally mono-inverts it for the dark UI, QOI-
 * encodes it, and emits ONE generated header (app/ui/assets/sprite_assets.h) embedding every sheet's QOI bytes
 * plus a `hc_sprite_assets[]` catalog (name + grid + fps/loop). The shipped app therefore needs NO runtime
 * asset path and NO image dependency: the PNG decode happens once here, offline; only the in-house QOI decoder
 * (libs/hc_image) ships, fed the embedded bytes. Replaces the hand-edited mascot_assets.h — adding a sheet is
 * now one manifest row + a re-run, not a hand-edited byte array.
 *
 * Usage:  qoi_pack <manifest.tsv> <out_header.h> <png_dir>
 *
 * Manifest: one sheet per line; '#' comments and blank lines ignored. Whitespace-separated columns:
 *     name  src_png  cols  rows  fps  loop  mono_invert
 *   - name        : stable catalog key, e.g. "mascot.think" (dots become '_' in the C symbol)
 *   - src_png     : file under <png_dir>
 *   - cols rows   : the frame grid (frame_w/h are DERIVED by the decoder as tex/cols, tex/rows)
 *   - fps         : playback rate; <= 0 means a single still frame
 *   - loop        : once | loop | pingpong  (ignored for a still)
 *   - mono_invert : 1 to map near-black line art -> white-on-transparent (the dark-UI treatment), else 0
 *
 * Only TRUSTED, in-repo PNGs named in the manifest are ever read. This is a build-time utility; it shares no
 * code with the shipped decoder and links no app/lib target. */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- QOI encoder (a standalone encoder; spec: qoiformat.org). NO shared code with the runtime decoder
 * libs/hc_image — both independently produce spec-conformant QOI (the decoder reads anything spec-valid). --- */
#define QOI_OP_RGB      0xfeu
#define QOI_OP_RGBA     0xffu
#define QOI_OP_INDEX    0x00u
#define QOI_OP_DIFF     0x40u
#define QOI_OP_LUMA     0x80u
#define QOI_OP_RUN      0xc0u
#define QOI_HEADER_SIZE 14u

typedef struct {
    unsigned char r, g, b, a;
} rgba;

static unsigned qoi_hash(rgba p) { return (unsigned)(p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) & 63u; }

static void put_u32_be(unsigned char *buf, size_t *pos, uint32_t v)
{
    buf[(*pos)++] = (unsigned char)(v >> 24);
    buf[(*pos)++] = (unsigned char)(v >> 16);
    buf[(*pos)++] = (unsigned char)(v >> 8);
    buf[(*pos)++] = (unsigned char)(v);
}

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
    buf[pos++] = 4;
    buf[pos++] = 0;

    rgba index[64];
    memset(index, 0, sizeof index);
    rgba     prev = {0, 0, 0, 255};
    uint64_t n = (uint64_t)w * h;
    unsigned run = 0;

    for (uint64_t i = 0; i < n; i++) {
        rgba cur = px[i];
        if (cur.r == prev.r && cur.g == prev.g && cur.b == prev.b && cur.a == prev.a) {
            run++;
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
                    int dr = (int)cur.r - prev.r, dg = (int)cur.g - prev.g, db = (int)cur.b - prev.b;
                    int dr_dg = dr - dg, db_dg = db - dg;
                    if (dr > -3 && dr < 2 && dg > -3 && dg < 2 && db > -3 && db < 2) {
                        buf[pos++] = (unsigned char)(QOI_OP_DIFF | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2));
                    } else if (dg > -33 && dg < 32 && dr_dg > -9 && dr_dg < 8 && db_dg > -9 && db_dg < 8) {
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
    for (int k = 0; k < 7; k++) buf[pos++] = 0x00;
    buf[pos++] = 0x01;
    *out_len = pos;
    return buf;
}

static unsigned char luma601(unsigned char r, unsigned char g, unsigned char b)
{
    return (unsigned char)((77u * r + 150u * g + 29u * b + 128u) >> 8);
}

/* --- the packer --- */

typedef struct {
    char           sym[96]; /* the manifest name with '.'/'-' -> '_' (the C symbol stem) */
    char           name[96];
    int            cols, rows, loop;
    float          fps;
    unsigned char *qoi;
    size_t         qoi_len;
} Sheet;

/* Decode one source PNG, optionally mono-invert, QOI-encode into sh->qoi. Returns 0 on success, -1 on error. */
static int pack_one(const char *png_dir, const char *src_png, int mono_invert, Sheet *sh)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", png_dir, src_png);
    int            iw = 0, ih = 0, ic = 0;
    unsigned char *src = stbi_load(path, &iw, &ih, &ic, 4);
    if (!src) {
        fprintf(stderr, "qoi_pack: cannot decode '%s': %s\n", path, stbi_failure_reason());
        return -1;
    }
    if (iw <= 0 || ih <= 0) {
        fprintf(stderr, "qoi_pack: '%s' has non-positive dimensions\n", path);
        stbi_image_free(src);
        return -1;
    }
    uint64_t n = (uint64_t)iw * (uint64_t)ih;
    rgba    *px = (rgba *)malloc((size_t)(n * sizeof(rgba)));
    if (!px) {
        stbi_image_free(src);
        fprintf(stderr, "qoi_pack: out of memory for %dx%d\n", iw, ih);
        return -1;
    }
    for (uint64_t i = 0; i < n; i++) {
        unsigned char r = src[i * 4 + 0], g = src[i * 4 + 1], b = src[i * 4 + 2], a = src[i * 4 + 3];
        if (mono_invert) {
            unsigned char y = luma601(r, g, b);
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
    sh->qoi = qoi_encode(px, (uint32_t)iw, (uint32_t)ih, &sh->qoi_len);
    free(px);
    if (!sh->qoi) {
        fprintf(stderr, "qoi_pack: out of memory encoding '%s'\n", src_png);
        return -1;
    }
    printf("  %-28s %dx%d -> %zu QOI bytes%s\n", src_png, iw, ih, sh->qoi_len,
           mono_invert ? " (mono-invert)" : "");
    return 0;
}

static int loop_code(const char *s)
{
    if (strcmp(s, "loop") == 0) return 1;
    if (strcmp(s, "pingpong") == 0) return 2;
    return 0; /* "once" / anything else */
}

static void emit_header(FILE *f, const char *manifest, const Sheet *sheets, int count)
{
    fprintf(f, "/* sprite_assets.h - GENERATED by tools/qoi_pack from %s - do not edit by hand.\n", manifest);
    fprintf(f, " *\n");
    fprintf(f, " * Each array is the raw bytes of a QOI file (qoiformat.org); decode at runtime with\n");
    fprintf(f, " * hc_image_decode_qoi (libs/hc_image). The hc_sprite_assets[] catalog carries the grid +\n");
    fprintf(f, " * playback policy so app/ui/src/ui_sprite_registry.cpp hardcodes nothing. Re-run qoi_pack\n");
    fprintf(f, " * (or the `regen_sprite_assets` CMake target) after editing the manifest. Read-only static\n");
    fprintf(f, " * data; nothing to free; valid for the whole process. */\n\n");
    fprintf(f, "#ifndef HC_APP_UI_SPRITE_ASSETS_H\n#define HC_APP_UI_SPRITE_ASSETS_H\n\n");
    fprintf(f, "#include <stddef.h>\n\n");
    fprintf(f, "typedef struct {\n");
    fprintf(f, "    const char          *name;\n");
    fprintf(f, "    const unsigned char *qoi;\n");
    fprintf(f, "    size_t               qoi_len;\n");
    fprintf(f, "    int                  cols, rows;\n");
    fprintf(f, "    float                fps;  /* <= 0 => a single still frame */\n");
    fprintf(f, "    int                  loop; /* 0=Once 1=Loop 2=PingPong   */\n");
    fprintf(f, "} hc_sprite_asset;\n\n");

    for (int s = 0; s < count; s++) {
        const Sheet *sh = &sheets[s];
        fprintf(f, "/* %s : %d x %d grid, %zu QOI bytes */\n", sh->name, sh->cols, sh->rows, sh->qoi_len);
        fprintf(f, "static const unsigned char %s_qoi[] = {", sh->sym);
        for (size_t i = 0; i < sh->qoi_len; i++) {
            if (i % 16 == 0) fprintf(f, "\n    ");
            fprintf(f, "0x%02x,", sh->qoi[i]);
        }
        fprintf(f, "\n};\n\n");
    }

    fprintf(f, "static const hc_sprite_asset hc_sprite_assets[] = {\n");
    for (int s = 0; s < count; s++) {
        const Sheet *sh = &sheets[s];
        fprintf(f, "    { \"%s\", %s_qoi, sizeof %s_qoi, %d, %d, %.4ff, %d },\n", sh->name, sh->sym, sh->sym,
                sh->cols, sh->rows, (double)sh->fps, sh->loop);
    }
    fprintf(f, "};\n");
    fprintf(f, "static const size_t hc_sprite_assets_count = sizeof hc_sprite_assets / sizeof hc_sprite_assets[0];\n\n");
    fprintf(f, "#endif /* HC_APP_UI_SPRITE_ASSETS_H */\n");
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: qoi_pack <manifest.tsv> <out_header.h> <png_dir>\n");
        return 2;
    }
    const char *manifest = argv[1], *out_header = argv[2], *png_dir = argv[3];

    FILE *mf = fopen(manifest, "r");
    if (!mf) {
        fprintf(stderr, "qoi_pack: cannot open manifest '%s'\n", manifest);
        return 1;
    }

    enum { kMaxSheets = 256 };
    Sheet *sheets = (Sheet *)calloc(kMaxSheets, sizeof(Sheet));
    if (!sheets) {
        fclose(mf);
        return 1;
    }
    int  count = 0, rc = 0;
    char line[1024];
    printf("qoi_pack: packing from %s\n", manifest);
    while (fgets(line, sizeof line, mf)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0' || *p == '\r') continue; /* comment / blank */

        char name[96], src[256], loop[32];
        int  cols = 0, rows = 0, mono = 0;
        float fps = 0.0f;
        if (sscanf(p, "%95s %255s %d %d %f %31s %d", name, src, &cols, &rows, &fps, loop, &mono) != 7) {
            fprintf(stderr, "qoi_pack: malformed manifest line: %s", line);
            rc = 1;
            break;
        }
        if (count >= kMaxSheets) {
            fprintf(stderr, "qoi_pack: too many sheets (cap %d)\n", kMaxSheets);
            rc = 1;
            break;
        }
        if (cols < 1 || rows < 1) {
            fprintf(stderr, "qoi_pack: '%s' cols/rows must be >= 1\n", name);
            rc = 1;
            break;
        }
        /* Defence-in-depth on the (trusted, in-repo) manifest: src is a plain name under png_dir, never an
         * escape (`..`) or an absolute path — so a hand-edited manifest cannot make the tool read outside it. */
        if (strstr(src, "..") != NULL || src[0] == '/') {
            fprintf(stderr, "qoi_pack: '%s' src must be a plain relative name under the png dir\n", name);
            rc = 1;
            break;
        }
        Sheet *sh = &sheets[count];
        snprintf(sh->name, sizeof sh->name, "%s", name);
        snprintf(sh->sym, sizeof sh->sym, "%s", name);
        for (char *c = sh->sym; *c; c++)
            if (*c == '.' || *c == '-')
                *c = '_'; /* the C symbol stem */
        sh->cols = cols;
        sh->rows = rows;
        sh->fps = fps;
        sh->loop = loop_code(loop);
        if (pack_one(png_dir, src, mono != 0, sh) != 0) {
            rc = 1;
            break;
        }
        count++;
    }
    fclose(mf);

    if (rc == 0) {
        FILE *of = fopen(out_header, "w");
        if (!of) {
            fprintf(stderr, "qoi_pack: cannot open '%s' for writing\n", out_header);
            rc = 1;
        } else {
            emit_header(of, manifest, sheets, count);
            if (fclose(of) != 0) rc = 1;
            if (rc == 0) printf("qoi_pack: wrote %s (%d sheets)\n", out_header, count);
        }
    }

    for (int s = 0; s < count; s++) free(sheets[s].qoi);
    free(sheets);
    return rc;
}
