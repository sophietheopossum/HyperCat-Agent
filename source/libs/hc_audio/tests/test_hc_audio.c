/* test_hc_audio (Phase A.1) — the offline gate for decode + probe: build a tiny PCM WAV in memory, probe its
 * format/rate/channels/duration, decode it back and check the samples round-trip, and confirm garbage / empty /
 * oversized input is rejected (never crashes). Pure — no device, no file I/O. Runs under ASan/UBSan. */

#include "hc_audio.h"
#include "../src/audio_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            fprintf(stderr, "FAIL: %s\n", msg);                                                                \
            g_fail++;                                                                                          \
        }                                                                                                      \
    } while (0)

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}
static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
}

/* Build a mono 16-bit PCM WAV of `frames` samples (a ramp). Caller frees. */
static unsigned char *make_wav(uint32_t frames, uint32_t rate, size_t *len_out)
{
    uint32_t       data_sz = frames * 2u;
    size_t         n = 44u + data_sz;
    unsigned char *b = (unsigned char *)malloc(n);
    if (!b) return NULL;
    memcpy(b, "RIFF", 4);
    put_u32(b + 4, 36u + data_sz);
    memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4);
    put_u32(b + 16, 16);
    put_u16(b + 20, 1);            /* PCM */
    put_u16(b + 22, 1);            /* mono */
    put_u32(b + 24, rate);
    put_u32(b + 28, rate * 2u);    /* byte rate */
    put_u16(b + 32, 2);            /* block align */
    put_u16(b + 34, 16);           /* bits */
    memcpy(b + 36, "data", 4);
    put_u32(b + 40, data_sz);
    for (uint32_t i = 0; i < frames; i++)
        put_u16(b + 44 + i * 2u, (uint16_t)(int16_t)(i * 7)); /* a deterministic ramp */
    *len_out = n;
    return b;
}

int main(void)
{
    const uint32_t frames = 8000, rate = 8000;
    size_t         len = 0;
    unsigned char *wav = make_wav(frames, rate, &len);
    CHECK(wav != NULL, "wav alloc");

    /* --- probe --- */
    hc_audio_meta m;
    CHECK(hc_audio_probe(wav, len, "ramp.wav", &m) == 0, "probe a valid WAV succeeds");
    CHECK(m.fmt == HC_AUDIO_FMT_WAV, "probe reports WAV");
    CHECK(m.samplerate == rate, "probe reports the sample rate");
    CHECK(m.channels == 1, "probe reports mono");
    CHECK(m.duration_ms == 1000, "probe computes 1000 ms (8000 frames @ 8000 Hz)");
    CHECK(strcmp(hc_audio_fmt_label(m.fmt), "WAV") == 0, "the format label is WAV");

    /* --- decode round-trip --- */
    hc_decoder *d = hc_decoder_open(wav, len, "ramp.wav");
    CHECK(d != NULL, "open a decoder over the WAV");
    if (d) {
        CHECK(hc_decoder_channels(d) == 1 && hc_decoder_samplerate(d) == rate, "decoder reports the format");
        CHECK(hc_decoder_total_frames(d) == frames, "decoder reports the frame count");
        int16_t *pcm = (int16_t *)malloc(frames * sizeof(int16_t));
        uint64_t got = hc_decoder_read_s16(d, pcm, frames);
        CHECK(got == frames, "read all frames");
        CHECK(pcm[0] == 0 && pcm[1] == 7 && pcm[10] == 70, "the decoded ramp matches what was written");
        /* seek back to the start and re-read the first sample */
        CHECK(hc_decoder_seek_frame(d, 0) == 0, "seek to frame 0");
        int16_t first = 1234;
        CHECK(hc_decoder_read_s16(d, &first, 1) == 1 && first == 0, "re-read after seek");
        free(pcm);
        hc_decoder_close(d);
    }

    /* --- W-Audio.2: hc_audio_decode_pcm one-shot full decode (the confined helper's decode primitive) --- */
    {
        int16_t      *pcm = NULL;
        uint64_t      pf = 0;
        unsigned      prate = 0, pch = 0;
        int           trunc = 9;
        hc_audio_meta dm;
        CHECK(hc_audio_decode_pcm(wav, len, "ramp.wav", HC_AUDIO_MAX_PCM_BYTES, &pcm, &pf, &prate, &pch, &trunc,
                                  &dm) == 0,
              "decode_pcm of a valid WAV succeeds");
        CHECK(pf == frames && prate == rate && pch == 1, "decode_pcm reports the full frame count + format");
        CHECK(trunc == 0, "decode_pcm not truncated within the cap");
        CHECK(dm.fmt == HC_AUDIO_FMT_WAV, "decode_pcm fills the metadata format");
        CHECK(pcm && pcm[0] == 0 && pcm[1] == 7 && pcm[10] == 70, "decode_pcm samples match the written ramp");
        free(pcm);

        /* the cap truncates: room for only ~50 frames (100 bytes) of an 8000-frame track */
        int16_t *pcm2 = NULL;
        uint64_t pf2 = 0;
        int      trunc2 = 0;
        CHECK(hc_audio_decode_pcm(wav, len, "ramp.wav", 100, &pcm2, &pf2, NULL, NULL, &trunc2, NULL) == 0,
              "decode_pcm with a tiny cap still succeeds");
        CHECK(trunc2 == 1 && pf2 > 0 && pf2 < frames, "decode_pcm truncates at the byte cap");
        free(pcm2);

        /* garbage is rejected and *out is cleared (the caller must not free a stale pointer) */
        unsigned char g[64];
        memset(g, 0xAB, sizeof g);
        int16_t *pcm3 = (int16_t *)0x1; /* sentinel — must be overwritten to NULL */
        CHECK(hc_audio_decode_pcm(g, sizeof g, "x.wav", HC_AUDIO_MAX_PCM_BYTES, &pcm3, NULL, NULL, NULL, NULL,
                                  NULL) != 0 &&
                  pcm3 == NULL,
              "decode_pcm rejects garbage + clears *out");
    }

    /* --- rejection: garbage / empty / NULL never crash, always fail cleanly --- */
    unsigned char junk[64];
    memset(junk, 0xAB, sizeof junk);
    CHECK(hc_audio_probe(junk, sizeof junk, "x.wav", &m) != 0, "garbage is rejected");
    CHECK(hc_audio_probe(NULL, 0, NULL, &m) != 0, "NULL/empty is rejected");
    CHECK(hc_decoder_open(junk, sizeof junk, NULL) == NULL, "decoder refuses garbage");
    CHECK(hc_audio_probe((const unsigned char *)"RIFFxxxxWAVE", 12, NULL, &m) != 0,
          "a truncated WAV header is rejected (no crash)");

    /* --- direct ID3v2 parser test: the hand-written UNTRUSTED parser, bypassing dr_mp3's init gate --- */
    {
        unsigned char id3[128];
        size_t        q = 0;
        memcpy(id3 + q, "ID3", 3); q += 3;
        id3[q++] = 3; id3[q++] = 0; /* v2.3.0 */
        id3[q++] = 0;               /* flags */
        size_t sizepos = q; q += 4; /* syncsafe tag size — filled below */
        size_t body = q;
        memcpy(id3 + q, "TIT2", 4); q += 4;                          /* title frame */
        id3[q++]=0; id3[q++]=0; id3[q++]=0; id3[q++]=5;              /* v2.3 plain size = 5 */
        id3[q++]=0; id3[q++]=0;                                      /* frame flags */
        id3[q++]=0; memcpy(id3 + q, "Song", 4); q += 4;             /* enc Latin-1 + "Song" */
        memcpy(id3 + q, "TPE1", 4); q += 4;                          /* artist frame */
        id3[q++]=0; id3[q++]=0; id3[q++]=0; id3[q++]=5;
        id3[q++]=0; id3[q++]=0;
        id3[q++]=0; memcpy(id3 + q, "Band", 4); q += 4;
        size_t bodylen = q - body;
        id3[sizepos+0]=(unsigned char)((bodylen>>21)&0x7f);
        id3[sizepos+1]=(unsigned char)((bodylen>>14)&0x7f);
        id3[sizepos+2]=(unsigned char)((bodylen>>7)&0x7f);
        id3[sizepos+3]=(unsigned char)(bodylen&0x7f);
        hc_audio_meta m2; memset(&m2, 0, sizeof m2);
        hc_id3v2_read(id3, q, &m2);
        CHECK(strcmp(m2.title, "Song") == 0, "ID3v2 TIT2 (title) extracted");
        CHECK(strcmp(m2.artist, "Band") == 0, "ID3v2 TPE1 (artist) extracted");

        /* hostile: a frame claiming a huge size must not overread (ASan is the oracle) */
        unsigned char bad[40];
        memset(bad, 0, sizeof bad);
        memcpy(bad, "ID3", 3); bad[3]=3; bad[9]=30;            /* tag size 30 */
        memcpy(bad+10, "TIT2", 4);
        bad[14]=0x7f; bad[15]=0xff; bad[16]=0xff; bad[17]=0xff; /* absurd frame size */
        hc_audio_meta m3; memset(&m3, 0, sizeof m3);
        hc_id3v2_read(bad, sizeof bad, &m3);
        CHECK(m3.title[0] == '\0', "ID3v2 oversize frame yields no title, no overread");

        /* every truncated prefix of the good tag — never overread */
        for (size_t pp = 1; pp <= q; pp++) {
            hc_audio_meta m4; memset(&m4, 0, sizeof m4);
            hc_id3v2_read(id3, pp, &m4);
        }
    }

    /* --- FUZZ battery: malformed input must never overread / crash (ASan/UBSan is the oracle) --- */
    {
        /* (a) every truncated prefix of the valid WAV (dense over the header, sparse over the body) */
        for (size_t pfx = 1; pfx < len; pfx += (pfx < 256 ? 1 : 64)) {
            hc_audio_meta mm;
            (void)hc_audio_probe(wav, pfx, "t.wav", &mm); /* must return, never crash */
            hc_decoder *dd = hc_decoder_open(wav, pfx, "t.wav");
            if (dd) {
                int16_t tmp[512];
                (void)hc_decoder_read_s16(dd, tmp, 256);
                (void)hc_decoder_seek_frame(dd, 1000000);
                hc_decoder_read_tags(dd, &mm);
                hc_decoder_close(dd);
            }
        }
        /* (b) random + magic-prefixed garbage through every decoder's error paths */
        uint32_t st = 0x9e3779b9u; /* deterministic xorshift */
        static const char *const magics[] = {"RIFF\0\0\0\0WAVE", "OggS", "fLaC", "ID3\x04\x00\x00", "\xff\xfb\x90\x00"};
        for (int it = 0; it < 4000; it++) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            size_t        n = 8 + (st % 4096);
            unsigned char *b = (unsigned char *)malloc(n);
            for (size_t i = 0; i < n; i++) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; b[i] = (unsigned char)st; }
            const char *mg = magics[it % 5];
            size_t       mglen = (it % 5 == 0) ? 12 : strlen(mg);
            if (mglen <= n) memcpy(b, mg, mglen); /* graft a real magic so detection routes into the parser */
            hc_audio_meta mm;
            (void)hc_audio_probe(b, n, NULL, &mm);
            hc_decoder *dd = hc_decoder_open(b, n, NULL);
            if (dd) {
                int16_t tmp[256];
                for (int r = 0; r < 4; r++) (void)hc_decoder_read_s16(dd, tmp, 128);
                hc_decoder_read_tags(dd, &mm);
                hc_decoder_close(dd);
            }
            free(b);
        }
    }

    free(wav);
    if (g_fail) {
        fprintf(stderr, "test_hc_audio: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_hc_audio: all checks passed (incl. the fuzz battery)\n");
    return 0;
}
