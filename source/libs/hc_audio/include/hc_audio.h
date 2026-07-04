/* hc_audio — HyperCat's audio primitive (Music Player + future notifications). A reusable C core, stable
 * extern-"C" ABI, mirroring hc_image: it turns UNTRUSTED compressed audio bytes into PCM + bounded metadata,
 * and (in runtime.c) streams that PCM to an OpenAL output device. This header is the WHOLE public surface.
 *
 * The decode/probe primitive and the playback engine are bundled DELIBERATELY (the accepted bend per
 * 14-engineering-policies §"When a rule must bend"): the engine is inseparable from the decode primitive for the
 * reuse target (notifications want both), and splitting them into two libs would add a link-dependency edge for
 * no isolation gain. The two sub-responsibilities, both defensive:
 *   - DECODE / PROBE (decode.c): WAV/MP3/FLAC/OGG bytes -> interleaved 16-bit PCM (streamed) and a bounded
 *     metadata record (title/artist/album/genre + duration/samplerate/channels). The decoders are the vendored
 *     single-headers dr_wav/dr_mp3/dr_flac + stb_vorbis, each compiled with no stdio + asserts disabled + the
 *     caps below enforced BEFORE allocation — the hc_image_stb.c posture for hostile input. The caller owns the
 *     compressed buffer and keeps it alive for the decoder's life (no copy here).
 *   - PLAYBACK (runtime.c): an opaque hc_audio_player owns ONE OpenAL device/context/source + a decode/stream
 *     thread + transport + a spectrum ring. OUTPUT ONLY — it never opens a capture device.
 *
 * Ownership: every `*_new`/`*_open` has a matching `*_free`/`*_close`; documented per function. Strings in
 * hc_audio_meta are FIXED-size NUL-terminated UTF-8 buffers (no caller free). Threading: the decode/probe
 * functions are reentrant over caller-owned buffers; the player owns its thread and its public methods are
 * called from one owner thread (the snapshot getter is the one cross-thread read, internally locked). */
#ifndef HC_AUDIO_H
#define HC_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- bounds (enforced before allocation; a hostile file is rejected, never aborts the host) ---- */
#define HC_AUDIO_MAX_BYTES       (256u * 1024u * 1024u) /* compressed input cap (256 MiB)            */
#define HC_AUDIO_MAX_CHANNELS    8u                     /* reject an absurd channel count            */
#define HC_AUDIO_MAX_SAMPLERATE  384000u                /* reject an absurd sample rate              */
#define HC_AUDIO_MAX_DURATION_MS (6u * 60u * 60u * 1000u) /* 6 h — reject an absurd declared length  */
#define HC_AUDIO_META_STR        256u                   /* fixed metadata string buffer (UTF-8, NUL) */
#define HC_AUDIO_MAX_PCM_BYTES   (256u * 1024u * 1024u) /* decoded-PCM cap for the one-shot full decode (W-Audio) */

/* The detected container/codec (also drives the format label). */
typedef enum {
    HC_AUDIO_FMT_UNKNOWN = 0,
    HC_AUDIO_FMT_WAV,
    HC_AUDIO_FMT_MP3,
    HC_AUDIO_FMT_FLAC,
    HC_AUDIO_FMT_OGG,
} hc_audio_fmt;

/* A bounded, copy-free metadata record. Tag strings are best-effort (empty when the file carries none); the
 * format/rate/channels/duration are authoritative. All strings are NUL-terminated, control bytes stripped at
 * the source — but the HOST still defangs them before display or model injection (untrusted tags). */
typedef struct {
    hc_audio_fmt fmt;
    unsigned     samplerate;  /* Hz                                   */
    unsigned     channels;    /* 1..HC_AUDIO_MAX_CHANNELS             */
    uint64_t     duration_ms; /* 0 if the decoder cannot report it    */
    char         title[HC_AUDIO_META_STR];
    char         artist[HC_AUDIO_META_STR];
    char         album[HC_AUDIO_META_STR];
    char         genre[HC_AUDIO_META_STR];
} hc_audio_meta;

/* Probe `data`/`len` (a complete in-memory file) into `*out`. `name_hint` (a filename or extension, may be
 * NULL) only breaks ties when the magic bytes are ambiguous — the magic is authoritative. Returns 0 on success
 * (a recognized, in-bounds stream) and fills `*out`; non-zero on an unrecognized/oversized/corrupt input
 * (`*out` is zeroed). Does NOT decode the audio — cheap (header + tag scan + a frame-count query). */
int hc_audio_probe(const unsigned char *data, size_t len, const char *name_hint, hc_audio_meta *out);

/* Decode a COMPLETE in-memory compressed file to interleaved 16-bit PCM — the WHOLE track, bounded by `max_bytes`
 * of output. On success returns 0 and sets `*out` (a malloc'd interleaved s16 buffer the CALLER must free),
 * `*out_frames`, `*out_rate`, `*out_channels`, and `*out_truncated` (1 if the decode stopped at `max_bytes` — a
 * track longer than the cap). `*meta_out` (optional) gets the track's metadata. Returns non-zero on an
 * unrecognized/oversized/corrupt stream (`*out` left NULL). This is the one-shot full-decode primitive the
 * confined audio helper uses so untrusted playback bytes are decoded OUT of the host (W-Audio.2); the player's
 * own streaming decoder is the separate in-process path for already-trusted PCM. All out-params are optional
 * except `out` (the PCM itself). */
int hc_audio_decode_pcm(const unsigned char *data, size_t len, const char *name_hint, size_t max_bytes,
                        int16_t **out, uint64_t *out_frames, unsigned *out_rate, unsigned *out_channels,
                        int *out_truncated, hc_audio_meta *meta_out);

/* The short human label for a format ("WAV"/"MP3"/"FLAC"/"OGG"/"?"). Static string, never freed. */
const char *hc_audio_fmt_label(hc_audio_fmt f);

/* ====================================================================================================
 * Playback engine (runtime.c) — an opaque player over ONE OpenAL output source + a decode/stream thread.
 * OUTPUT ONLY (never opens a capture device). The player owns its thread; every public method below is
 * safe to call from one owner thread, and `get_status` is a thread-safe snapshot read. All OpenAL calls
 * happen on the internal stream thread (commands are funneled to it), so there is no cross-thread AL race.
 * ==================================================================================================== */

#define HC_AUDIO_SPECTRUM_BINS 48 /* visualiser band count surfaced in the status */

typedef struct hc_audio_player hc_audio_player; /* opaque playback engine handle */

typedef enum {
    HC_AUDIO_STOPPED = 0,
    HC_AUDIO_PLAYING,
    HC_AUDIO_PAUSED,
} hc_audio_play_state;

/* A thread-safe snapshot of the player (filled by hc_audio_player_get_status). Strings are bounded + NUL. */
typedef struct {
    hc_audio_play_state state;
    uint64_t            pos_ms;  /* audible playback position */
    uint64_t            dur_ms;  /* track length (0 if unknown) */
    float               volume;  /* 0..1 */
    hc_audio_fmt        fmt;
    int                 n_spectrum; /* valid bins in spectrum[]: HC_AUDIO_SPECTRUM_BINS once decoding has begun,
                                      * 0 before the first decode — iterate [0, n_spectrum), not the array size */
    float               spectrum[HC_AUDIO_SPECTRUM_BINS]; /* 0..1 magnitudes (smoothed) */
    char                title[HC_AUDIO_META_STR];         /* now-playing (from the loaded track's probe) */
    char                artist[HC_AUDIO_META_STR];
} hc_audio_status;

/* Create the player (opens the default OpenAL device + a context + one streaming source + the stream thread).
 * Returns NULL if no audio backend is available — the caller is expected to fall back to silent operation. */
hc_audio_player *hc_audio_player_new(void);
void             hc_audio_player_free(hc_audio_player *);

/* Load a track from a COMPLETE in-memory compressed file (the player COPIES the bytes — the caller may free
 * `data` after). `name_hint` disambiguates the format. Replaces the current track and starts PAUSED (call
 * play()). `meta_out` (optional) receives the probed metadata. Returns 0 on success, non-zero on an
 * unrecognized/oversized/corrupt stream (the previous track is left cleared). */
int  hc_audio_player_load(hc_audio_player *, const unsigned char *data, size_t len, const char *name_hint,
                          hc_audio_meta *meta_out);
/* Load ALREADY-DECODED interleaved 16-bit PCM directly (the player COPIES it — the caller may free after). This
 * bypasses the in-process decoder entirely: the untrusted compressed bytes were decoded out-of-process by the
 * confined audio helper (W-Audio.2), so the host plays only raw samples + integer rate/channels it has clamped.
 * `meta` (optional) supplies the now-playing title/artist/fmt for display. Replaces the current track and starts
 * PAUSED (call play()). Returns 0 on success, non-zero on a bad arg / allocation failure. */
int  hc_audio_player_load_pcm(hc_audio_player *, const int16_t *interleaved, uint64_t frames, unsigned rate,
                              unsigned channels, const hc_audio_meta *meta);
void hc_audio_player_play(hc_audio_player *);
void hc_audio_player_pause(hc_audio_player *);
void hc_audio_player_stop(hc_audio_player *); /* stop + rewind to the start */
void hc_audio_player_seek_ms(hc_audio_player *, uint64_t ms);
void hc_audio_player_set_volume(hc_audio_player *, float v01); /* clamped to [0,1] */
void hc_audio_player_get_status(hc_audio_player *, hc_audio_status *out);

#ifdef __cplusplus
}
#endif

#endif /* HC_AUDIO_H */
