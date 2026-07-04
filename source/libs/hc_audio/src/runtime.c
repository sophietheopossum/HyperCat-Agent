/* runtime — the OpenAL streaming playback engine (hc_audio A.2).
 *
 * Purpose:   stream a decoded track to an OpenAL OUTPUT device with transport (play/pause/stop/seek/volume) and
 *            a live spectrum, behind the opaque hc_audio_player.
 * Owns:      one OpenAL device/context/source + NBUF stream buffers, the hc_decoder + its compressed-byte copy,
 *            the decode/stream thread, and the published hc_audio_status. OUTPUT ONLY — no capture device.
 * Threading: the STREAM THREAD is the ONLY thread that calls OpenAL or touches the decoder. Public methods just
 *            post a command (flag + params) under `mu` and signal `cv`; the thread drains commands, decodes +
 *            queues PCM, tracks position, computes the spectrum, and publishes status via the publish_* helpers
 *            (one atomic write each). get_status copies the status under `mu`. No cross-thread AL race, and the
 *            lock is never held across a decode/AL call. `stream_main` is intentionally one ~80-line timed-loop
 *            body (drain/pump/reclaim/position/spectrum) — its steps share decbuf + smooth[] lock-free on the
 *            single thread, so splitting them would only add inter-helper mutable state (the worse tradeoff).
 * Lifetime:  _new opens the device + starts the thread (NULL on no backend); _free signals stop, joins, then
 *            tears down. The loaded compressed bytes are COPIED — the caller may free after load(). */

#include "hc_audio.h"
#include "audio_internal.h"
#include "spectrum.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NBUF 4           /* streaming buffers in flight */
#define CHUNK 2048u      /* frames decoded + queued per buffer */
#define MAXCH HC_AUDIO_MAX_CHANNELS

struct hc_audio_player {
    ALCdevice  *dev;
    ALCcontext *ctx;
    ALuint      source;
    ALuint      bufs[NBUF];

    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             running;

    /* ---- command IN (written by public methods under mu, drained by the thread) ---- */
    unsigned char *pending_data; /* a fresh track to load (player-owned copy); thread takes ownership */
    size_t         pending_len;
    char           pending_hint[64];
    int            pending_is_pcm; /* W-Audio.2: the pending load is already-decoded PCM, not compressed */
    unsigned       pending_pcm_rate, pending_pcm_channels;
    uint64_t       pending_pcm_frames;
    int            cmd_load, cmd_play, cmd_pause, cmd_stop, cmd_seek;
    uint64_t       seek_ms;
    float          volume; /* 0..1, applied by the thread */

    /* ---- thread-owned playback state (no lock needed; only the thread touches these) ---- */
    unsigned char *track;
    size_t         track_len;
    hc_decoder    *dec;
    unsigned       rate, channels;
    uint64_t       total_frames, frames_decoded, played_base;
    int            eof, has_track;
    int16_t       *decbuf; /* CHUNK*MAXCH scratch */
    int16_t       *stbuf;  /* CHUNK*2 stereo downmix scratch */
    uint64_t       last_got; /* frames in the most-recent decoded chunk (the spectrum window size) */
    ALuint         free_ids[NBUF];
    int            n_free;
    uint64_t       qframes[NBUF]; /* FIFO of queued buffers' frame counts (matches the AL queue order) */
    int            q_head, q_count;
    float          smooth[HC_AUDIO_SPECTRUM_BINS];

    /* ---- status OUT (written by the thread under mu, read by get_status) ---- */
    hc_audio_status pub;
};

static void fifo_push(hc_audio_player *p, uint64_t frames)
{
    int t = (p->q_head + p->q_count) % NBUF;
    p->qframes[t] = frames;
    p->q_count++;
}
static uint64_t fifo_pop(hc_audio_player *p)
{
    if (p->q_count == 0) return 0;
    uint64_t v = p->qframes[p->q_head];
    p->q_head = (p->q_head + 1) % NBUF;
    p->q_count--;
    return v;
}

/* ---- narrow status publishers: ONE atomic write under mu, never holding the lock across AL/decode ---- */
static void publish_state(hc_audio_player *p, hc_audio_play_state s)
{
    pthread_mutex_lock(&p->mu); p->pub.state = s; pthread_mutex_unlock(&p->mu);
}
static void publish_state_pos(hc_audio_player *p, hc_audio_play_state s, uint64_t pos_ms)
{
    pthread_mutex_lock(&p->mu); p->pub.state = s; p->pub.pos_ms = pos_ms; pthread_mutex_unlock(&p->mu);
}
static void publish_pos(hc_audio_player *p, uint64_t pos_ms)
{
    pthread_mutex_lock(&p->mu); p->pub.pos_ms = pos_ms; pthread_mutex_unlock(&p->mu);
}

/* Stop the source + fully drain its queue (PROCESSED then QUEUED) and return to "nothing queued, all free".
 * The single drain path used by drop_track + the stop/seek commands (no divergent duplicates). Thread context. */
static void flush_al_queue(hc_audio_player *p)
{
    alSourceStop(p->source);
    ALint proc = 0;
    alGetSourcei(p->source, AL_BUFFERS_PROCESSED, &proc);
    while (proc-- > 0) { ALuint id = 0; alSourceUnqueueBuffers(p->source, 1, &id); }
    ALint queued = 0;
    alGetSourcei(p->source, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0) { ALuint id = 0; alSourceUnqueueBuffers(p->source, 1, &id); }
    p->n_free = NBUF;
    for (int i = 0; i < NBUF; i++) p->free_ids[i] = p->bufs[i];
    p->q_head = p->q_count = 0;
}

/* Tear down the current track (thread context). */
static void drop_track(hc_audio_player *p)
{
    flush_al_queue(p);
    if (p->dec) { hc_decoder_close(p->dec); p->dec = NULL; }
    free(p->track); p->track = NULL; p->track_len = 0;
    p->frames_decoded = p->played_base = p->total_frames = 0;
    p->last_got = 0;
    p->eof = 0; p->has_track = 0;
    p->rate = 0; p->channels = 0;
}

/* Decode one chunk and queue it; returns 1 if a buffer was queued, 0 if none free / EOF. */
static int pump_one(hc_audio_player *p)
{
    if (p->n_free == 0 || p->eof || !p->dec) return 0;
    uint64_t got = hc_decoder_read_s16(p->dec, p->decbuf, CHUNK);
    if (got == 0) { p->eof = 1; return 0; }
    p->frames_decoded += got;
    p->last_got = got; /* the spectrum window over the freshest chunk (may be < CHUNK on the final read) */

    const int16_t *src = p->decbuf;
    ALenum  fmt = (p->channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    unsigned al_ch = (p->channels == 1) ? 1u : 2u;
    if (p->channels > 2) { /* downmix >2ch -> stereo (front L/R) */
        for (uint64_t i = 0; i < got; i++) {
            p->stbuf[i * 2] = p->decbuf[i * p->channels];
            p->stbuf[i * 2 + 1] = p->decbuf[i * p->channels + 1];
        }
        src = p->stbuf;
    }
    ALuint id = p->free_ids[--p->n_free];
    alBufferData(id, fmt, src, (ALsizei)(got * al_ch * sizeof(int16_t)), (ALsizei)p->rate);
    alSourceQueueBuffers(p->source, 1, &id);
    fifo_push(p, got);
    return 1;
}

static void apply_commands(hc_audio_player *p)
{
    pthread_mutex_lock(&p->mu);
    int load = p->cmd_load, play = p->cmd_play, pause = p->cmd_pause, stop = p->cmd_stop, seek = p->cmd_seek;
    uint64_t seek_ms = p->seek_ms;
    float vol = p->volume;
    unsigned char *ndata = p->pending_data;
    size_t nlen = p->pending_len;
    int is_pcm = p->pending_is_pcm;
    unsigned pcm_rate = p->pending_pcm_rate, pcm_ch = p->pending_pcm_channels;
    uint64_t pcm_frames = p->pending_pcm_frames;
    char hint[64];
    memcpy(hint, p->pending_hint, sizeof hint);
    p->cmd_load = p->cmd_play = p->cmd_pause = p->cmd_stop = p->cmd_seek = 0;
    if (load) { p->pending_data = NULL; p->pending_len = 0; p->pending_is_pcm = 0; }
    pthread_mutex_unlock(&p->mu);

    alSourcef(p->source, AL_GAIN, vol < 0 ? 0 : (vol > 1 ? 1 : vol));

    if (load) {
        drop_track(p);
        if (ndata && nlen) {
            p->dec = is_pcm ? hc_decoder_open_pcm((const int16_t *)ndata, pcm_frames, pcm_rate, pcm_ch)
                            : hc_decoder_open(ndata, nlen, hint);
            if (p->dec) {
                p->track = ndata; p->track_len = nlen;
                p->rate = hc_decoder_samplerate(p->dec);
                p->channels = hc_decoder_channels(p->dec);
                p->total_frames = hc_decoder_total_frames(p->dec);
                p->has_track = 1;
            } else {
                free(ndata); /* unrecognized — leave cleared */
            }
        }
        publish_state(p, HC_AUDIO_PAUSED); /* loaded, awaiting play() */
    }
    if (stop && p->has_track) {
        flush_al_queue(p);
        hc_decoder_seek_frame(p->dec, 0);
        p->frames_decoded = p->played_base = 0; p->eof = 0;
        publish_state_pos(p, HC_AUDIO_STOPPED, 0);
    }
    if (seek && p->has_track && p->rate) {
        uint64_t frame = (seek_ms * p->rate) / 1000u;
        if (p->total_frames && frame > p->total_frames) frame = p->total_frames;
        flush_al_queue(p);
        hc_decoder_seek_frame(p->dec, frame);
        p->frames_decoded = p->played_base = frame; p->eof = 0;
        publish_pos(p, (frame * 1000u) / p->rate);
    }
    if (pause) { alSourcePause(p->source); publish_state(p, HC_AUDIO_PAUSED); }
    if (play && p->has_track) publish_state(p, HC_AUDIO_PLAYING);
}

static void *stream_main(void *arg)
{
    hc_audio_player *p = (hc_audio_player *)arg;
    alcMakeContextCurrent(p->ctx);
    alGenSources(1, &p->source);
    alGenBuffers(NBUF, p->bufs);
    p->n_free = NBUF;
    for (int i = 0; i < NBUF; i++) p->free_ids[i] = p->bufs[i];

    for (;;) {
        pthread_mutex_lock(&p->mu);
        if (!p->running) { pthread_mutex_unlock(&p->mu); break; }
        hc_audio_play_state st = p->pub.state;
        pthread_mutex_unlock(&p->mu);

        apply_commands(p);

        pthread_mutex_lock(&p->mu);
        st = p->pub.state;
        pthread_mutex_unlock(&p->mu);

        if (st == HC_AUDIO_PLAYING && p->has_track) {
            /* reclaim processed buffers -> advance played_base + free their ids */
            ALint proc = 0;
            alGetSourcei(p->source, AL_BUFFERS_PROCESSED, &proc);
            while (proc-- > 0) {
                ALuint id = 0;
                alSourceUnqueueBuffers(p->source, 1, &id);
                p->played_base += fifo_pop(p);
                if (p->n_free < NBUF) p->free_ids[p->n_free++] = id;
            }
            while (pump_one(p)) { /* top up the queue */ }

            ALint srcst = 0;
            alGetSourcei(p->source, AL_SOURCE_STATE, &srcst);
            if (srcst != AL_PLAYING && p->q_count > 0) alSourcePlay(p->source);

            if (p->eof && p->q_count == 0) { /* track finished */
                hc_decoder_seek_frame(p->dec, 0);
                p->frames_decoded = p->played_base = 0; p->eof = 0; p->last_got = 0;
                publish_state_pos(p, HC_AUDIO_STOPPED, 0);
            }

            /* audible position = fully-played frames + offset into the current buffer */
            ALint off = 0;
            alGetSourcei(p->source, AL_SAMPLE_OFFSET, &off);
            uint64_t pos_frames = p->played_base + (off > 0 ? (uint64_t)off : 0);

            /* spectrum over the FRESHEST chunk (its true frame count, not the CHUNK ceiling), peak-hold + decay */
            float bins[HC_AUDIO_SPECTRUM_BINS];
            hc_spectrum_compute(p->decbuf, p->last_got, p->channels, p->rate, bins, HC_AUDIO_SPECTRUM_BINS);
            for (int i = 0; i < HC_AUDIO_SPECTRUM_BINS; i++)
                p->smooth[i] = bins[i] > p->smooth[i] ? bins[i] : p->smooth[i] * 0.86f;

            pthread_mutex_lock(&p->mu);
            p->pub.pos_ms = p->rate ? (pos_frames * 1000u) / p->rate : 0;
            p->pub.dur_ms = p->rate ? (p->total_frames * 1000u) / p->rate : 0;
            p->pub.n_spectrum = HC_AUDIO_SPECTRUM_BINS;
            memcpy(p->pub.spectrum, p->smooth, sizeof p->smooth);
            pthread_mutex_unlock(&p->mu);
        } else {
            /* decay the spectrum to rest while paused/stopped */
            pthread_mutex_lock(&p->mu);
            for (int i = 0; i < HC_AUDIO_SPECTRUM_BINS; i++) { p->smooth[i] *= 0.80f; p->pub.spectrum[i] = p->smooth[i]; }
            p->pub.n_spectrum = HC_AUDIO_SPECTRUM_BINS;
            pthread_mutex_unlock(&p->mu);
        }

        /* wait ~15ms or until a command arrives */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 15 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&p->mu);
        if (p->running) pthread_cond_timedwait(&p->cv, &p->mu, &ts);
        pthread_mutex_unlock(&p->mu);
    }

    drop_track(p);
    alDeleteSources(1, &p->source);
    alDeleteBuffers(NBUF, p->bufs);
    alcMakeContextCurrent(NULL);
    return NULL;
}

hc_audio_player *hc_audio_player_new(void)
{
    ALCdevice *dev = alcOpenDevice(NULL);
    if (!dev) return NULL;
    ALCcontext *ctx = alcCreateContext(dev, NULL);
    if (!ctx) { alcCloseDevice(dev); return NULL; }

    hc_audio_player *p = (hc_audio_player *)calloc(1, sizeof *p);
    if (!p) { alcDestroyContext(ctx); alcCloseDevice(dev); return NULL; }
    p->dev = dev; p->ctx = ctx; p->volume = 1.0f; p->running = 1;
    p->pub.state = HC_AUDIO_STOPPED; p->pub.volume = 1.0f;
    p->decbuf = (int16_t *)malloc(CHUNK * MAXCH * sizeof(int16_t));
    p->stbuf = (int16_t *)malloc(CHUNK * 2u * sizeof(int16_t));
    if (!p->decbuf || !p->stbuf) { free(p->decbuf); free(p->stbuf); free(p); alcDestroyContext(ctx); alcCloseDevice(dev); return NULL; }
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    if (pthread_create(&p->thread, NULL, stream_main, p) != 0) {
        pthread_mutex_destroy(&p->mu); pthread_cond_destroy(&p->cv);
        free(p->decbuf); free(p->stbuf); free(p);
        alcDestroyContext(ctx); alcCloseDevice(dev);
        return NULL;
    }
    return p;
}

void hc_audio_player_free(hc_audio_player *p)
{
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->running = 0;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
    pthread_join(p->thread, NULL);
    free(p->pending_data);
    free(p->decbuf);
    free(p->stbuf);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    alcDestroyContext(p->ctx);
    alcCloseDevice(p->dev);
    free(p);
}

int hc_audio_player_load(hc_audio_player *p, const unsigned char *data, size_t len, const char *name_hint,
                         hc_audio_meta *meta_out)
{
    if (!p || !data || len == 0 || len > HC_AUDIO_MAX_BYTES) return -1;
    hc_audio_meta m;
    if (hc_audio_probe(data, len, name_hint, &m) != 0) return -1; /* reject before we copy/queue */
    unsigned char *copy = (unsigned char *)malloc(len);
    if (!copy) return -1;
    memcpy(copy, data, len);

    pthread_mutex_lock(&p->mu);
    free(p->pending_data); /* a load not yet consumed is superseded */
    p->pending_data = copy;
    p->pending_len = len;
    snprintf(p->pending_hint, sizeof p->pending_hint, "%s", name_hint ? name_hint : "");
    p->cmd_load = 1;
    /* publish the now-playing metadata immediately so the UI/conductor see the title before the thread loads */
    snprintf(p->pub.title, sizeof p->pub.title, "%s", m.title);
    snprintf(p->pub.artist, sizeof p->pub.artist, "%s", m.artist);
    p->pub.fmt = m.fmt;
    p->pub.dur_ms = m.duration_ms;
    p->pub.pos_ms = 0;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
    if (meta_out) *meta_out = m;
    return 0;
}

int hc_audio_player_load_pcm(hc_audio_player *p, const int16_t *interleaved, uint64_t frames, unsigned rate,
                             unsigned channels, const hc_audio_meta *meta)
{
    if (!p || !interleaved || frames == 0 || channels == 0 || channels > HC_AUDIO_MAX_CHANNELS || rate == 0 ||
        rate > HC_AUDIO_MAX_SAMPLERATE)
        return -1;
    if (frames > (uint64_t)(SIZE_MAX / (channels * sizeof(int16_t)))) return -1; /* overflow guard before alloc */
    size_t   bytes = (size_t)frames * channels * sizeof(int16_t);
    int16_t *copy = (int16_t *)malloc(bytes);
    if (!copy) return -1;
    memcpy(copy, interleaved, bytes);

    pthread_mutex_lock(&p->mu);
    free(p->pending_data); /* a load not yet consumed is superseded */
    p->pending_data = (unsigned char *)copy;
    p->pending_len = bytes;
    p->pending_is_pcm = 1;
    p->pending_pcm_rate = rate;
    p->pending_pcm_channels = channels;
    p->pending_pcm_frames = frames;
    p->cmd_load = 1;
    /* publish the now-playing metadata immediately so the UI/conductor see the title before the thread loads */
    snprintf(p->pub.title, sizeof p->pub.title, "%s", meta ? meta->title : "");
    snprintf(p->pub.artist, sizeof p->pub.artist, "%s", meta ? meta->artist : "");
    p->pub.fmt = meta ? meta->fmt : HC_AUDIO_FMT_WAV;
    p->pub.dur_ms = rate ? (frames * 1000u) / rate : 0;
    p->pub.pos_ms = 0;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

static void post(hc_audio_player *p, int which)
{
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    switch (which) {
    case 0: p->cmd_play = 1; break;
    case 1: p->cmd_pause = 1; break;
    case 2: p->cmd_stop = 1; break;
    }
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}
void hc_audio_player_play(hc_audio_player *p) { post(p, 0); }
void hc_audio_player_pause(hc_audio_player *p) { post(p, 1); }
void hc_audio_player_stop(hc_audio_player *p) { post(p, 2); }

void hc_audio_player_seek_ms(hc_audio_player *p, uint64_t ms)
{
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->cmd_seek = 1;
    p->seek_ms = ms;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

void hc_audio_player_set_volume(hc_audio_player *p, float v01)
{
    if (!p) return;
    if (v01 < 0) v01 = 0; else if (v01 > 1) v01 = 1;
    pthread_mutex_lock(&p->mu);
    p->volume = v01;
    p->pub.volume = v01;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

void hc_audio_player_get_status(hc_audio_player *p, hc_audio_status *out)
{
    if (!out) return;
    if (!p) { memset(out, 0, sizeof *out); return; }
    pthread_mutex_lock(&p->mu);
    *out = p->pub;
    pthread_mutex_unlock(&p->mu);
}
