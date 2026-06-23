/*
 * morse/fft.h - A small radix-2 FFT and spectral helpers.
 *
 * This exists to make Morse decoding robust to *any* tone: rather than assume
 * a fixed pitch, the detector estimates the dominant CW frequency from the
 * signal itself using an averaged (Welch) magnitude spectrum with parabolic
 * peak interpolation. The same routines back the GUI's spectrum display.
 *
 * The transform is a textbook iterative Cooley-Tukey radix-2 FFT, so input
 * lengths must be powers of two. Helpers are provided to window real audio and
 * to estimate the strongest frequency within a band.
 */
#ifndef MORSE_FFT_H
#define MORSE_FFT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if n is a non-zero power of two. */
int morse_is_pow2(size_t n);

/* Largest power of two <= n (0 if n == 0). */
size_t morse_floor_pow2(size_t n);

/*
 * In-place complex FFT of n points (n must be a power of two). `re` and `im`
 * hold the real and imaginary parts. `inverse` != 0 computes the inverse
 * transform (scaled by 1/n). Returns 0 on success, non-zero on bad input.
 */
int morse_fft(double *re, double *im, size_t n, int inverse);

/*
 * Magnitude spectrum of a real signal. Applies a Hann window to x[0..n-1]
 * (n a power of two), runs the FFT, and writes n/2 magnitudes to out_mag.
 * Returns 0 on success.
 */
int morse_real_spectrum(const float *x, size_t n, double *out_mag);

/*
 * Estimate the dominant frequency (Hz) in [fmin,fmax] from real samples using
 * Welch-averaged magnitude spectra over overlapping windows of size `win`
 * (rounded down to a power of two; pass 0 for an automatic choice). Parabolic
 * interpolation refines the peak to sub-bin resolution. Returns 0.0 if no
 * usable peak is found (e.g. silence). `out_strength`, if non-NULL, receives a
 * 0..1 confidence (peak magnitude over mean magnitude, normalized).
 */
double morse_dominant_tone(const float *x, size_t count,
                           unsigned int sample_rate, double fmin, double fmax,
                           size_t win, double *out_strength);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_FFT_H */
