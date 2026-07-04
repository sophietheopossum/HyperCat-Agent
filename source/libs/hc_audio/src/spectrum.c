/* spectrum — see spectrum.h. A 1024-point Hann-windowed radix-2 FFT over a mono downmix, grouped into log-
 * spaced bands and mapped to [0,1] on a dB scale. Allocation-free (fixed stack arrays); deterministic. */

#include "spectrum.h"

#include <math.h>
#include <string.h>

#define FFT_N 1024 /* power of two; the analysis window (zero-padded when the chunk is shorter) */

/* In-place iterative radix-2 Cooley–Tukey FFT of size FFT_N over re[]/im[]. */
static void fft1024(float *re, float *im)
{
    /* bit-reversal permutation */
    for (unsigned i = 1, j = 0; i < FFT_N; i++) {
        unsigned bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (unsigned len = 2; len <= FFT_N; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (unsigned i = 0; i < FFT_N; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (unsigned k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

void hc_spectrum_compute(const int16_t *pcm, uint64_t frames, unsigned channels, unsigned samplerate,
                         float *out, int n_bins)
{
    if (!out || n_bins < 1) return;
    if (n_bins > HC_SPECTRUM_MAXBINS) n_bins = HC_SPECTRUM_MAXBINS;
    for (int i = 0; i < n_bins; i++) out[i] = 0.0f;
    if (!pcm || frames == 0 || channels == 0) return;
    if (samplerate == 0) samplerate = 44100;

    float re[FFT_N], im[FFT_N];
    unsigned n = frames < FFT_N ? (unsigned)frames : FFT_N;
    for (unsigned i = 0; i < n; i++) {
        /* downmix to mono in [-1,1] */
        long acc = 0;
        for (unsigned c = 0; c < channels; c++) acc += pcm[i * channels + c];
        float s = (float)acc / ((float)channels * 32768.0f);
        float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(FFT_N - 1)); /* Hann */
        re[i] = s * w;
        im[i] = 0.0f;
    }
    for (unsigned i = n; i < FFT_N; i++) { re[i] = 0.0f; im[i] = 0.0f; } /* zero-pad */

    fft1024(re, im);

    /* group the lower half (real spectrum) into log-spaced bands; map magnitude -> dB -> [0,1] */
    const float fmin = 40.0f, fmax = (float)samplerate * 0.5f;
    const float bin_hz = (float)samplerate / (float)FFT_N;
    for (int b = 0; b < n_bins; b++) {
        float lo = fmin * powf(fmax / fmin, (float)b / (float)n_bins);
        float hi = fmin * powf(fmax / fmin, (float)(b + 1) / (float)n_bins);
        unsigned k0 = (unsigned)(lo / bin_hz), k1 = (unsigned)(hi / bin_hz);
        if (k0 < 1) k0 = 1;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > FFT_N / 2) k1 = FFT_N / 2;
        float peak = 0.0f;
        for (unsigned k = k0; k < k1; k++) {
            float mag = sqrtf(re[k] * re[k] + im[k] * im[k]) * 2.0f / (float)FFT_N;
            if (mag > peak) peak = mag;
        }
        float db = 20.0f * log10f(peak + 1e-9f);    /* full-scale ~ 0 dB */
        float v = (db + 60.0f) / 60.0f;              /* map [-60,0] dB -> [0,1] */
        out[b] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
}
