/* decode — the streaming decoder, the one-shot full decode, and the metadata probe (hc_audio).
 *
 * Purpose:   turn UNTRUSTED WAV/MP3/FLAC/OGG bytes into (a) a uniform pull-stream of s16 PCM behind
 *            audio_internal.h's hc_decoder so runtime.c streams every codec identically; (b) a one-shot full
 *            decode to an interleaved-PCM buffer (hc_audio_decode_pcm — what the confined audio helper calls for
 *            playback, W-Audio.2); and (c) a bounded metadata record (hc_audio_probe). It also provides a
 *            PCM-PASSTHROUGH decoder (hc_decoder_open_pcm) so runtime.c can stream already-decoded PCM through the
 *            very same pull interface, with no change to its stream loop.
 * Owns:      this TU ISOLATES the vendored single-header decoders dr_wav/dr_mp3/dr_flac (their implementations
 *            compile ONLY here) + drives stb_vorbis (impl in stb_vorbis_impl.c). hc_decoder does NOT own the
 *            compressed buffer — the caller keeps it alive open..close; the PCM-passthrough decoder likewise
 *            BORROWS its samples. hc_audio_decode_pcm returns a malloc'd interleaved-PCM buffer the CALLER frees.
 * Threading: reentrant over distinct decoders; a single decoder is used from one thread at a time.
 * Security:  hardened for hostile input — NO stdio (DR_*_NO_STDIO / STB_VORBIS_NO_STDIO; decode from a bounded
 *            memory buffer only); asserts disabled (DR*_ASSERT no-op; a malformed file never abort()s the host);
 *            size/channel/samplerate/duration caps enforced BEFORE allocation; the hand-written ID3v2 + Vorbis-
 *            comment readers are bounded; stb_vorbis gets a bounded alloc arena (see the OGG open path). */

#include "audio_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ---- vendored decoders (this TU owns their implementations; hardened) ---- */
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DRWAV_ASSERT(x) ((void)0)
#include "dr_wav.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DRMP3_ASSERT(x) ((void)0)
#include "dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DRFLAC_ASSERT(x) ((void)0)
#include "dr_flac.h"

/* stb_vorbis: declarations only here (the implementation is stb_vorbis_impl.c). NO_STDIO / NO_PUSHDATA_API are
 * set as target compile definitions so both TUs agree. */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define HC_OGG_ARENA (32u * 1024u * 1024u) /* bounded arena for stb_vorbis — see the OGG open path */

struct hc_decoder {
    hc_audio_fmt         fmt;
    const unsigned char *data; /* borrowed — alive for the decoder's life */
    size_t               len;
    unsigned             samplerate;
    unsigned             channels;
    union {
        drwav       wav;
        drmp3       mp3;
        drflac     *flac;
        stb_vorbis *ogg;
    } u;
    unsigned char *ogg_arena; /* bounded bump-arena handed to stb_vorbis (caps its allocations; see HC_OGG_ARENA) */
    /* FLAC Vorbis comments are captured during open (the metadata callback); staged here until read_tags. */
    char ft_title[HC_AUDIO_META_STR];
    char ft_artist[HC_AUDIO_META_STR];
    char ft_album[HC_AUDIO_META_STR];
    char ft_genre[HC_AUDIO_META_STR];
    /* W-Audio.2 PCM-passthrough source (is_pcm=1): play ALREADY-decoded PCM through the same pull interface, so
     * runtime.c streams trusted PCM with no change to its loop. The vendored-decoder union is unused in this mode;
     * `pcm` is BORROWED (the player keeps it alive open..close). */
    int            is_pcm;
    const int16_t *pcm;
    uint64_t       pcm_frames;
    uint64_t       pcm_cursor;
};

const char *hc_audio_fmt_label(hc_audio_fmt f)
{
    switch (f) {
    case HC_AUDIO_FMT_WAV:  return "WAV";
    case HC_AUDIO_FMT_MP3:  return "MP3";
    case HC_AUDIO_FMT_FLAC: return "FLAC";
    case HC_AUDIO_FMT_OGG:  return "OGG";
    default:                return "?";
    }
}

/* Copy up to HC_AUDIO_META_STR-1 bytes, dropping ASCII control bytes (< 0x20 and 0x7f) — bounded + NUL-term.
 * Defense-in-depth on untrusted tags; the host defangs again before display/injection. */
static void copy_tag(char *dst, const char *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < HC_AUDIO_META_STR; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0) break;
        if (c < 0x20 || c == 0x7f) continue;
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
}

static int ext_is(const char *hint, const char *ext)
{
    if (!hint) return 0;
    size_t hl = strlen(hint), el = strlen(ext);
    if (hl < el) return 0;
    const char *t = hint + (hl - el);
    for (size_t i = 0; i < el; i++) {
        char a = t[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* Magic-first detection; the name_hint only disambiguates an ID3-prefixed file. */
static hc_audio_fmt detect_fmt(const unsigned char *d, size_t n, const char *hint)
{
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0) return HC_AUDIO_FMT_WAV;
    if (n >= 4 && memcmp(d, "OggS", 4) == 0) return HC_AUDIO_FMT_OGG;
    if (n >= 4 && memcmp(d, "fLaC", 4) == 0) return HC_AUDIO_FMT_FLAC;
    if (n >= 3 && memcmp(d, "ID3", 3) == 0) { /* an ID3v2-tagged file — usually MP3; the hint refines it */
        if (ext_is(hint, ".flac")) return HC_AUDIO_FMT_FLAC;
        if (ext_is(hint, ".ogg") || ext_is(hint, ".oga")) return HC_AUDIO_FMT_OGG;
        return HC_AUDIO_FMT_MP3;
    }
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return HC_AUDIO_FMT_MP3; /* MPEG frame sync */
    /* no magic — lean on the extension */
    if (ext_is(hint, ".wav")) return HC_AUDIO_FMT_WAV;
    if (ext_is(hint, ".mp3")) return HC_AUDIO_FMT_MP3;
    if (ext_is(hint, ".flac")) return HC_AUDIO_FMT_FLAC;
    if (ext_is(hint, ".ogg") || ext_is(hint, ".oga")) return HC_AUDIO_FMT_OGG;
    return HC_AUDIO_FMT_UNKNOWN;
}

static int vorbis_kv(const char *kv, const char *key, const char **val_out, int *vlen_out)
{
    /* match "KEY=value" case-insensitively on the key; report the value span */
    size_t kl = strlen(key);
    size_t i = 0;
    for (; i < kl; i++) {
        char a = kv[i];
        if (a == '\0') return 0;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        char b = key[i]; /* key passed lowercase */
        if (a != b) return 0;
    }
    if (kv[kl] != '=') return 0;
    *val_out = kv + kl + 1;
    *vlen_out = (int)strlen(kv + kl + 1);
    return 1;
}

/* Apply one "KEY=value" Vorbis comment to the staging tag buffers (used by FLAC open-callback + OGG read). */
static void apply_vorbis_comment(const char *kv, int klen, char *t, char *ar, char *al, char *g)
{
    (void)klen;
    const char *v;
    int         vl;
    if (vorbis_kv(kv, "title", &v, &vl)) copy_tag(t, v, (size_t)vl);
    else if (vorbis_kv(kv, "artist", &v, &vl)) copy_tag(ar, v, (size_t)vl);
    else if (vorbis_kv(kv, "album", &v, &vl)) copy_tag(al, v, (size_t)vl);
    else if (vorbis_kv(kv, "genre", &v, &vl)) copy_tag(g, v, (size_t)vl);
}

static void flac_on_meta(void *pUser, drflac_metadata *pMeta)
{
    hc_decoder *d = (hc_decoder *)pUser;
    if (pMeta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, pMeta->data.vorbis_comment.commentCount,
                                        pMeta->data.vorbis_comment.pComments);
    drflac_uint32 clen;
    const char   *c;
    while ((c = drflac_next_vorbis_comment(&it, &clen)) != NULL) {
        /* the iterator gives a non-NUL-terminated span of length clen — stage into a bounded temp */
        char tmp[512];
        size_t n = clen < sizeof tmp - 1 ? clen : sizeof tmp - 1;
        memcpy(tmp, c, n);
        tmp[n] = '\0';
        apply_vorbis_comment(tmp, (int)n, d->ft_title, d->ft_artist, d->ft_album, d->ft_genre);
    }
}

hc_decoder *hc_decoder_open(const unsigned char *data, size_t len, const char *name_hint)
{
    if (!data || len == 0 || len > HC_AUDIO_MAX_BYTES) return NULL;
    hc_audio_fmt fmt = detect_fmt(data, len, name_hint);
    if (fmt == HC_AUDIO_FMT_UNKNOWN) return NULL;

    hc_decoder *d = (hc_decoder *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->fmt = fmt;
    d->data = data;
    d->len = len;

    int ok = 0;
    switch (fmt) {
    case HC_AUDIO_FMT_WAV:
        if (drwav_init_memory(&d->u.wav, data, len, NULL)) {
            d->samplerate = d->u.wav.sampleRate;
            d->channels = d->u.wav.channels;
            ok = 1;
        }
        break;
    case HC_AUDIO_FMT_MP3:
        if (drmp3_init_memory(&d->u.mp3, data, len, NULL)) {
            d->samplerate = d->u.mp3.sampleRate;
            d->channels = d->u.mp3.channels;
            ok = 1;
        }
        break;
    case HC_AUDIO_FMT_FLAC:
        d->u.flac = drflac_open_memory_with_metadata(data, len, flac_on_meta, d, NULL);
        if (d->u.flac) {
            d->samplerate = d->u.flac->sampleRate;
            d->channels = d->u.flac->channels;
            ok = 1;
        }
        break;
    case HC_AUDIO_FMT_OGG: {
        /* Hand stb_vorbis a BOUNDED bump-arena (its documented alloc API) instead of letting it malloc freely:
         * this caps total decoder memory (a crafted comment-length header can't drive a multi-GB allocation)
         * AND contains stb's signed-`len` edge (a 0xFFFFFFFF vendor length -> malloc(0) -> a 1-byte underflow
         * write) inside OUR arena rather than the heap — without editing the vendored source (no fork drift).
         * 32 MiB dwarfs any legitimate Vorbis setup yet refuses the pathological request. */
        int              err = 0;
        d->ogg_arena = (unsigned char *)malloc(HC_OGG_ARENA);
        stb_vorbis_alloc arena = {(char *)d->ogg_arena, (int)HC_OGG_ARENA};
        d->u.ogg = d->ogg_arena ? stb_vorbis_open_memory(data, (int)len, &err, &arena) : NULL;
        if (d->u.ogg) {
            stb_vorbis_info info = stb_vorbis_get_info(d->u.ogg);
            d->samplerate = info.sample_rate;
            d->channels = (unsigned)info.channels;
            ok = 1;
        }
        break;
    }
    default: break;
    }

    if (!ok || d->channels == 0 || d->channels > HC_AUDIO_MAX_CHANNELS || d->samplerate == 0 ||
        d->samplerate > HC_AUDIO_MAX_SAMPLERATE) {
        hc_decoder_close(d);
        return NULL;
    }
    return d;
}

hc_decoder *hc_decoder_open_pcm(const int16_t *interleaved, uint64_t frames, unsigned rate, unsigned channels)
{
    if (!interleaved || channels == 0 || channels > HC_AUDIO_MAX_CHANNELS || rate == 0 ||
        rate > HC_AUDIO_MAX_SAMPLERATE)
        return NULL;
    hc_decoder *d = (hc_decoder *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->is_pcm = 1;
    d->fmt = HC_AUDIO_FMT_WAV; /* a label only — the player shows the original meta.fmt, never this */
    d->pcm = interleaved;      /* BORROWED — the player keeps the PCM buffer alive open..close */
    d->pcm_frames = frames;
    d->pcm_cursor = 0;
    d->samplerate = rate;
    d->channels = channels;
    return d;
}

void hc_decoder_close(hc_decoder *d)
{
    if (!d) return;
    if (d->is_pcm) { /* the passthrough source borrows its PCM — free only the decoder struct */
        free(d);
        return;
    }
    switch (d->fmt) {
    case HC_AUDIO_FMT_WAV:  drwav_uninit(&d->u.wav); break;
    case HC_AUDIO_FMT_MP3:  drmp3_uninit(&d->u.mp3); break;
    case HC_AUDIO_FMT_FLAC: if (d->u.flac) drflac_close(d->u.flac); break;
    case HC_AUDIO_FMT_OGG:  if (d->u.ogg) stb_vorbis_close(d->u.ogg); break;
    default: break;
    }
    free(d->ogg_arena); /* NULL for non-OGG (calloc-zeroed); freed after stb_vorbis_close */
    free(d);
}

hc_audio_fmt hc_decoder_format(const hc_decoder *d) { return d ? d->fmt : HC_AUDIO_FMT_UNKNOWN; }
unsigned     hc_decoder_samplerate(const hc_decoder *d) { return d ? d->samplerate : 0; }
unsigned     hc_decoder_channels(const hc_decoder *d) { return d ? d->channels : 0; }

uint64_t hc_decoder_total_frames(const hc_decoder *d)
{
    if (!d) return 0;
    if (d->is_pcm) return d->pcm_frames;
    switch (d->fmt) {
    case HC_AUDIO_FMT_WAV:  return d->u.wav.totalPCMFrameCount;
    case HC_AUDIO_FMT_MP3:  return drmp3_get_pcm_frame_count(&((hc_decoder *)d)->u.mp3);
    case HC_AUDIO_FMT_FLAC: return d->u.flac->totalPCMFrameCount;
    case HC_AUDIO_FMT_OGG:  return stb_vorbis_stream_length_in_samples(((hc_decoder *)d)->u.ogg);
    default: return 0;
    }
}

uint64_t hc_decoder_read_s16(hc_decoder *d, int16_t *out, uint64_t frames)
{
    if (!d || !out || frames == 0) return 0;
    if (d->is_pcm) {
        uint64_t remaining = d->pcm_frames - d->pcm_cursor;
        uint64_t n = frames < remaining ? frames : remaining;
        if (n == 0) return 0;
        memcpy(out, d->pcm + d->pcm_cursor * d->channels, (size_t)(n * d->channels) * sizeof(int16_t));
        d->pcm_cursor += n;
        return n;
    }
    switch (d->fmt) {
    case HC_AUDIO_FMT_WAV:  return drwav_read_pcm_frames_s16(&d->u.wav, frames, out);
    case HC_AUDIO_FMT_MP3:  return drmp3_read_pcm_frames_s16(&d->u.mp3, frames, out);
    case HC_AUDIO_FMT_FLAC: return drflac_read_pcm_frames_s16(d->u.flac, frames, out);
    case HC_AUDIO_FMT_OGG: {
        uint64_t cap = (uint64_t)INT_MAX / (d->channels ? d->channels : 1u); /* never wrap the int num_shorts */
        if (frames > cap) frames = cap;
        int got = stb_vorbis_get_samples_short_interleaved(d->u.ogg, (int)d->channels, out,
                                                           (int)(frames * d->channels));
        return got < 0 ? 0 : (uint64_t)got;
    }
    default: return 0;
    }
}

int hc_decoder_seek_frame(hc_decoder *d, uint64_t frame)
{
    if (!d) return -1;
    if (d->is_pcm) {
        d->pcm_cursor = frame > d->pcm_frames ? d->pcm_frames : frame;
        return 0;
    }
    switch (d->fmt) {
    case HC_AUDIO_FMT_WAV:  return drwav_seek_to_pcm_frame(&d->u.wav, frame) ? 0 : -1;
    case HC_AUDIO_FMT_MP3:  return drmp3_seek_to_pcm_frame(&d->u.mp3, frame) ? 0 : -1;
    case HC_AUDIO_FMT_FLAC: return drflac_seek_to_pcm_frame(d->u.flac, frame) ? 0 : -1;
    case HC_AUDIO_FMT_OGG:  return stb_vorbis_seek(d->u.ogg, (unsigned)frame) ? 0 : -1;
    default: return -1;
    }
}

/* ---- ID3v2 (MP3) minimal tag reader: TIT2/TPE1/TALB/TCON, encodings 0 (Latin-1) + 3 (UTF-8); UTF-16
 *      frames are taken best-effort (low byte). Every step is bounds-checked — the input is untrusted. ---- */
static uint32_t syncsafe(const unsigned char *p) /* 4 x 7-bit */
{
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f);
}

static void id3_text(const unsigned char *payload, uint32_t plen, char *dst)
{
    if (plen < 1) return;
    unsigned char enc = payload[0];
    const unsigned char *t = payload + 1;
    uint32_t tn = plen - 1;
    if (enc == 0 || enc == 3) { /* Latin-1 / UTF-8 — copy bytes (copy_tag strips controls) */
        copy_tag(dst, (const char *)t, tn);
    } else { /* UTF-16 (enc 1 = with BOM, enc 2 = BE) — best-effort ASCII via the low byte */
        char   tmp[HC_AUDIO_META_STR];
        size_t o = 0, i = 0;
        int    be = (enc == 2); /* enc 2 is UTF-16BE, no BOM */
        if (tn >= 2 && t[0] == 0xFF && t[1] == 0xFE) { be = 0; i = 2; }      /* LE BOM */
        else if (tn >= 2 && t[0] == 0xFE && t[1] == 0xFF) { be = 1; i = 2; } /* BE BOM */
        for (; i + 1 < tn && o + 1 < sizeof tmp; i += 2) {
            unsigned char lo = be ? t[i + 1] : t[i];
            if (lo) tmp[o++] = (char)lo;
        }
        tmp[o] = '\0';
        copy_tag(dst, tmp, o);
    }
}

void hc_id3v2_read(const unsigned char *d, size_t len, hc_audio_meta *m)
{
    if (len < 10 || memcmp(d, "ID3", 3) != 0) return;
    unsigned char major = d[3];
    uint32_t tag_size = syncsafe(d + 6);
    if ((size_t)tag_size + 10 > len) tag_size = (uint32_t)(len - 10); /* clamp to what we have */
    const unsigned char *p = d + 10;
    const unsigned char *end = p + tag_size;
    while (p + 10 <= end) {
        char id[5];
        memcpy(id, p, 4);
        id[4] = '\0';
        if (id[0] == 0) break; /* padding */
        uint32_t fsz = (major >= 4) ? syncsafe(p + 4)
                                    : ((uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 |
                                       (uint32_t)p[6] << 8 | (uint32_t)p[7]);
        const unsigned char *payload = p + 10;
        if (fsz > (uint32_t)(end - payload)) break; /* bound without forming an out-of-object pointer (payload<=end) */
        if (memcmp(id, "TIT2", 4) == 0) id3_text(payload, fsz, m->title);
        else if (memcmp(id, "TPE1", 4) == 0) id3_text(payload, fsz, m->artist);
        else if (memcmp(id, "TALB", 4) == 0) id3_text(payload, fsz, m->album);
        else if (memcmp(id, "TCON", 4) == 0) id3_text(payload, fsz, m->genre);
        p = payload + fsz;
    }
}

void hc_decoder_read_tags(hc_decoder *d, hc_audio_meta *m)
{
    if (!d || !m) return;
    switch (d->fmt) {
    case HC_AUDIO_FMT_FLAC: /* captured during open */
        copy_tag(m->title, d->ft_title, strlen(d->ft_title));
        copy_tag(m->artist, d->ft_artist, strlen(d->ft_artist));
        copy_tag(m->album, d->ft_album, strlen(d->ft_album));
        copy_tag(m->genre, d->ft_genre, strlen(d->ft_genre));
        break;
    case HC_AUDIO_FMT_OGG: {
        stb_vorbis_comment vc = stb_vorbis_get_comment(d->u.ogg);
        for (int i = 0; i < vc.comment_list_length; i++)
            if (vc.comment_list[i])
                apply_vorbis_comment(vc.comment_list[i], (int)strlen(vc.comment_list[i]), m->title, m->artist,
                                     m->album, m->genre);
        break;
    }
    case HC_AUDIO_FMT_MP3:
        hc_id3v2_read(d->data, d->len, m);
        break;
    default: break; /* WAV: no standard tags */
    }
}

int hc_audio_probe(const unsigned char *data, size_t len, const char *name_hint, hc_audio_meta *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    hc_decoder *d = hc_decoder_open(data, len, name_hint);
    if (!d) return -1;
    out->fmt = d->fmt;
    out->samplerate = d->samplerate;
    out->channels = d->channels;
    uint64_t frames = hc_decoder_total_frames(d);
    out->duration_ms = (out->samplerate > 0) ? (frames * 1000ull) / out->samplerate : 0;
    if (out->duration_ms > HC_AUDIO_MAX_DURATION_MS) out->duration_ms = HC_AUDIO_MAX_DURATION_MS;
    hc_decoder_read_tags(d, out);
    hc_decoder_close(d);
    return 0;
}

int hc_audio_decode_pcm(const unsigned char *data, size_t len, const char *name_hint, size_t max_bytes,
                        int16_t **out, uint64_t *out_frames, unsigned *out_rate, unsigned *out_channels,
                        int *out_truncated, hc_audio_meta *meta_out)
{
    if (out) *out = NULL;
    if (out_frames) *out_frames = 0;
    if (out_rate) *out_rate = 0;
    if (out_channels) *out_channels = 0;
    if (out_truncated) *out_truncated = 0;
    if (meta_out) memset(meta_out, 0, sizeof *meta_out);
    if (!out || !data || len == 0 || len > HC_AUDIO_MAX_BYTES) return -1;

    hc_decoder *d = hc_decoder_open(data, len, name_hint);
    if (!d) return -1;
    unsigned ch = d->channels, rate = d->samplerate; /* in-bounds by open's validation */

    /* the frame cap so the interleaved s16 output never exceeds max_bytes (ch>=1 here, so no div-by-zero) */
    uint64_t max_frames = (uint64_t)(max_bytes / (ch * sizeof(int16_t)));
    if (max_frames == 0) { /* max_bytes too small even for one frame */
        hc_decoder_close(d);
        return -1;
    }

    int16_t       *buf = NULL;
    uint64_t       cap = 0, n = 0; /* allocated frames, decoded frames */
    int            truncated = 0;
    const uint64_t CK = 8192; /* decode in chunks; grow geometrically */
    for (;;) {
        if (n >= max_frames) { /* reached the cap with the codec still producing — conservatively "truncated" */
            truncated = 1;
            break;
        }
        uint64_t want = CK;
        if (n + want > max_frames) want = max_frames - n;
        if (n + want > cap) {
            uint64_t ncap = cap ? cap * 2 : (CK * 16);
            while (ncap < n + want) ncap *= 2;
            if (ncap > max_frames) ncap = max_frames;
            int16_t *nb = (int16_t *)realloc(buf, (size_t)ncap * ch * sizeof(int16_t)); /* <= max_bytes, no wrap */
            if (!nb) {
                free(buf);
                hc_decoder_close(d);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        uint64_t got = hc_decoder_read_s16(d, buf + n * ch, want);
        if (got == 0) break; /* EOF */
        n += got;
    }
    if (n == 0) { /* a recognized stream that yielded no frames — treat as undecodable */
        hc_decoder_close(d);
        free(buf);
        return -1;
    }
    if (meta_out) { /* fill from the STILL-OPEN decoder (no second open) + the ACTUAL decoded length n */
        meta_out->fmt = hc_decoder_format(d);
        meta_out->samplerate = rate;
        meta_out->channels = ch;
        meta_out->duration_ms = rate ? (n * 1000ull) / rate : 0;
        if (meta_out->duration_ms > HC_AUDIO_MAX_DURATION_MS) meta_out->duration_ms = HC_AUDIO_MAX_DURATION_MS;
        hc_decoder_read_tags(d, meta_out);
    }
    hc_decoder_close(d);

    *out = buf;
    if (out_frames) *out_frames = n;
    if (out_rate) *out_rate = rate;
    if (out_channels) *out_channels = ch;
    if (out_truncated) *out_truncated = truncated;
    return 0;
}
