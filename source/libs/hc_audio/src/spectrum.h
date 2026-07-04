/* spectrum — the in-house audio visualiser transform (hc_audio A.2). PURE: a window of interleaved s16 PCM ->
 * N log-spaced 0..1 magnitude bands, via a tiny radix-2 FFT (no FFT dependency). No device, no allocation, no
 * threading — the player calls it on its stream thread over each decoded chunk and smooths the result. Unit-
 * testable directly. */
#ifndef HC_AUDIO_SPECTRUM_H
#define HC_AUDIO_SPECTRUM_H

#include <stddef.h>
#include <stdint.h>

/* Compute `n_bins` (1..HC_SPECTRUM_MAXBINS) log-spaced magnitude bands in [0,1] from `frames` interleaved s16
 * samples of `channels` (downmixed to mono internally; a short/empty chunk zero-pads). `out` must hold n_bins
 * floats. Deterministic, allocation-free. */
/* Internal cap (headroom for a future bin-count change without an ABI break); the PUBLIC band count the player
 * surfaces is hc_audio.h's HC_AUDIO_SPECTRUM_BINS (48) <= this. */
#define HC_SPECTRUM_MAXBINS 64
void hc_spectrum_compute(const int16_t *interleaved, uint64_t frames, unsigned channels, unsigned samplerate,
                         float *out, int n_bins);

#endif /* HC_AUDIO_SPECTRUM_H */
