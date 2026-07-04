/* test_hc_audio_player (Phase A.2) — the engine + spectrum gate. Forces the OpenAL "null" backend
 * (ALSOFT_DRIVERS=null) so it exercises the streaming pipeline + the decode/stream thread + transport with NO
 * audible output and tolerance of a headless box (player_new -> NULL just skips the playback assertions). Runs
 * under ASan/UBSan; the thread create/join + the mutex/cond discipline are the TSan-relevant surface. */

#include "hc_audio.h"
#include "../src/spectrum.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail = 0;
#define CHECK(c, m)                                                                                            \
    do {                                                                                                       \
        if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); g_fail++; }                                              \
    } while (0)

static void put_u32(unsigned char *p, uint32_t v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24); }
static void put_u16(unsigned char *p, uint16_t v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }

/* a 1-second 440 Hz mono sine WAV at 44100 Hz */
static unsigned char *make_sine(size_t *len_out, uint32_t *frames_out)
{
    uint32_t rate = 44100, frames = 44100;
    uint32_t data_sz = frames * 2u;
    size_t   n = 44u + data_sz;
    unsigned char *b = (unsigned char *)malloc(n);
    if (!b) return NULL;
    memcpy(b, "RIFF", 4); put_u32(b + 4, 36u + data_sz); memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4); put_u32(b + 16, 16); put_u16(b + 20, 1); put_u16(b + 22, 1);
    put_u32(b + 24, rate); put_u32(b + 28, rate * 2u); put_u16(b + 32, 2); put_u16(b + 34, 16);
    memcpy(b + 36, "data", 4); put_u32(b + 40, data_sz);
    for (uint32_t i = 0; i < frames; i++) {
        double s = sin(2.0 * M_PI * 440.0 * i / rate);
        put_u16(b + 44 + i * 2u, (uint16_t)(int16_t)(s * 20000.0));
    }
    *len_out = n; *frames_out = frames;
    return b;
}

static void nap_ms(long ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }

int main(void)
{
    /* belt-and-suspenders: also set the null driver in-process for a direct run (ctest sets it via ENVIRONMENT) */
    setenv("ALSOFT_DRIVERS", "null", 1);

    /* --- spectrum: a 440 Hz sine has bounded, finite, non-zero energy --- */
    {
        int16_t sine[2048];
        for (int i = 0; i < 2048; i++) sine[i] = (int16_t)(sin(2.0 * M_PI * 440.0 * i / 44100.0) * 20000.0);
        float bins[HC_AUDIO_SPECTRUM_BINS];
        hc_spectrum_compute(sine, 2048, 1, 44100, bins, HC_AUDIO_SPECTRUM_BINS);
        float sum = 0;
        int   in_range = 1;
        for (int i = 0; i < HC_AUDIO_SPECTRUM_BINS; i++) {
            if (!(bins[i] >= 0.0f && bins[i] <= 1.0f)) in_range = 0; /* also catches NaN */
            sum += bins[i];
        }
        CHECK(in_range, "spectrum bins are all in [0,1], finite");
        CHECK(sum > 0.0f, "a sine produces non-zero spectrum energy");
        /* silence -> ~zero */
        int16_t zero[2048];
        memset(zero, 0, sizeof zero);
        hc_spectrum_compute(zero, 2048, 1, 44100, bins, HC_AUDIO_SPECTRUM_BINS);
        float zsum = 0;
        for (int i = 0; i < HC_AUDIO_SPECTRUM_BINS; i++) zsum += bins[i];
        CHECK(zsum < 1.0f, "silence produces near-zero spectrum");
    }

    /* --- the engine --- */
    hc_audio_player *p = hc_audio_player_new();
    if (!p) {
        printf("test_hc_audio_player: no audio backend — engine assertions skipped (graceful null path)\n");
        return g_fail ? 1 : 0;
    }

    size_t   len = 0;
    uint32_t frames = 0;
    unsigned char *wav = make_sine(&len, &frames);
    CHECK(wav != NULL, "sine wav alloc");

    hc_audio_meta meta;
    CHECK(hc_audio_player_load(p, wav, len, "sine.wav", &meta) == 0, "load a valid WAV");
    CHECK(meta.fmt == HC_AUDIO_FMT_WAV && meta.duration_ms == 1000, "loaded metadata (WAV, 1000 ms)");
    free(wav); /* the player copied the bytes */

    hc_audio_player_set_volume(p, 0.0f); /* silent even if a real device sneaks in */
    hc_audio_player_play(p);

    /* let the stream thread run; the null backend advances the clock in real time */
    uint64_t maxpos = 0;
    for (int i = 0; i < 20; i++) {
        nap_ms(25);
        hc_audio_status s;
        hc_audio_player_get_status(p, &s);
        if (s.pos_ms > maxpos) maxpos = s.pos_ms;
        CHECK(s.dur_ms == 1000, "status reports the 1000 ms duration");
        CHECK(s.pos_ms <= 1100, "position stays within the track (no runaway)");
    }
    CHECK(maxpos > 0, "playback position advanced past 0");

    /* transport: pause / seek / stop / re-play all without crashing the thread */
    hc_audio_player_pause(p);
    nap_ms(20);
    hc_audio_player_seek_ms(p, 500);
    nap_ms(20);
    hc_audio_player_play(p);
    nap_ms(30);
    hc_audio_player_stop(p);
    nap_ms(20);
    {
        hc_audio_status s;
        hc_audio_player_get_status(p, &s);
        CHECK(s.state == HC_AUDIO_STOPPED, "stop -> STOPPED");
        CHECK(s.pos_ms == 0, "stop rewinds to 0");
    }

    /* loading garbage is rejected and leaves the player healthy */
    unsigned char junk[100];
    memset(junk, 0x5A, sizeof junk);
    CHECK(hc_audio_player_load(p, junk, sizeof junk, "x.mp3", NULL) != 0, "garbage load is rejected");

    /* --- W-Audio.2: load ALREADY-decoded PCM directly (the confined-helper playback path bypasses the decoder) --- */
    {
        unsigned rate = 44100, ch = 1;
        uint64_t pf = 22050; /* 0.5 s */
        int16_t *pcm = (int16_t *)malloc((size_t)pf * ch * sizeof(int16_t));
        CHECK(pcm != NULL, "pcm alloc");
        for (uint64_t i = 0; i < pf; i++) pcm[i] = (int16_t)(sin(2.0 * M_PI * 440.0 * i / rate) * 18000.0);
        hc_audio_meta pm;
        memset(&pm, 0, sizeof pm);
        pm.fmt = HC_AUDIO_FMT_MP3; /* a display label only (the original format) */
        snprintf(pm.title, sizeof pm.title, "pcm-track");
        CHECK(hc_audio_player_load_pcm(p, pcm, pf, rate, ch, &pm) == 0, "load_pcm accepts valid PCM");
        CHECK(hc_audio_player_load_pcm(p, pcm, pf, 0, ch, &pm) != 0, "load_pcm rejects rate=0");
        CHECK(hc_audio_player_load_pcm(p, pcm, pf, rate, 0, &pm) != 0, "load_pcm rejects channels=0");
        CHECK(hc_audio_player_load_pcm(p, NULL, pf, rate, ch, &pm) != 0, "load_pcm rejects null PCM");
        free(pcm); /* the player copied it */
        hc_audio_player_set_volume(p, 0.0f);
        hc_audio_player_play(p);
        uint64_t pmax = 0;
        for (int i = 0; i < 16; i++) {
            nap_ms(25);
            hc_audio_status s;
            hc_audio_player_get_status(p, &s);
            if (s.pos_ms > pmax) pmax = s.pos_ms;
            CHECK(s.dur_ms == 500, "load_pcm status reports the 500 ms duration");
        }
        CHECK(pmax > 0, "load_pcm playback advanced past 0");
        hc_audio_player_stop(p);
    }

    hc_audio_player_free(p);

    if (g_fail) { fprintf(stderr, "test_hc_audio_player: %d check(s) failed\n", g_fail); return 1; }
    printf("test_hc_audio_player: all checks passed\n");
    return 0;
}
