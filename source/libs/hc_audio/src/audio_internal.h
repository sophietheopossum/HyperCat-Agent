/* audio_internal — the PRIVATE streaming-decoder seam shared by decode.c (probe) and runtime.c (the OpenAL
 * player). One uniform pull interface over the four vendored decoders so the player streams any format the same
 * way. NOT installed — internal to libs/hc_audio. The decoder does NOT own the compressed buffer: the caller
 * keeps `data` alive for the decoder's whole life (open..close). All functions are reentrant over distinct
 * decoders; a single decoder is used from one thread at a time. */
#ifndef HC_AUDIO_INTERNAL_H
#define HC_AUDIO_INTERNAL_H

#include "hc_audio.h"

#include <stddef.h>
#include <stdint.h>

typedef struct hc_decoder hc_decoder; /* opaque streaming decoder */

/* Open a decoder over the in-memory file (format detected by magic, `name_hint` breaks ties). Returns NULL on
 * an unrecognized/oversized/corrupt stream or an out-of-bounds rate/channels. The caller keeps `data` alive
 * until hc_decoder_close. */
hc_decoder *hc_decoder_open(const unsigned char *data, size_t len, const char *name_hint);

/* Open a PASSTHROUGH decoder over already-decoded interleaved s16 PCM: read_s16 copies from the buffer, seek
 * moves a cursor, total_frames/samplerate/channels report the given values. Lets runtime.c play raw PCM through
 * the EXACT same streaming path as a compressed codec (W-Audio.2's load_pcm), with NO change to the stream loop.
 * The PCM buffer is BORROWED — the caller (the player) keeps `interleaved` alive until hc_decoder_close. Returns
 * NULL on a bad arg (null buffer, zero/over-cap rate or channels). */
hc_decoder *hc_decoder_open_pcm(const int16_t *interleaved, uint64_t frames, unsigned rate, unsigned channels);
void        hc_decoder_close(hc_decoder *);

hc_audio_fmt hc_decoder_format(const hc_decoder *);
unsigned     hc_decoder_samplerate(const hc_decoder *);
unsigned     hc_decoder_channels(const hc_decoder *);
uint64_t     hc_decoder_total_frames(const hc_decoder *); /* 0 if the codec cannot report it */

/* Read up to `frames` interleaved s16 frames into `out` (must hold frames*channels int16). Returns the frames
 * actually read (0 at end-of-stream). Advances the read cursor. */
uint64_t hc_decoder_read_s16(hc_decoder *, int16_t *out, uint64_t frames);

/* Seek so the next read starts at `frame` (clamped to the stream). Returns 0 on success, non-zero if the codec
 * cannot seek. */
int hc_decoder_seek_frame(hc_decoder *, uint64_t frame);

/* Fill the tag strings (title/artist/album/genre) of `*m` from the stream's metadata, best-effort (leaves a
 * field empty when absent). Bounded + NUL-terminated; control bytes dropped. The fmt/rate/channels/duration
 * fields are filled by the caller (hc_audio_probe) from the decoder accessors. */
void hc_decoder_read_tags(hc_decoder *, hc_audio_meta *m);

/* Parse an ID3v2 tag buffer (the MP3 metadata path) into `*m`'s tag fields. Bounded — never reads past `len`.
 * Exposed (not static) so this hand-written UNTRUSTED-input parser is unit-tested DIRECTLY with hostile buffers;
 * in production it is reached only behind dr_mp3's init gate. Caller zeroes `*m` first. */
void hc_id3v2_read(const unsigned char *data, size_t len, hc_audio_meta *m);

#endif /* HC_AUDIO_INTERNAL_H */
