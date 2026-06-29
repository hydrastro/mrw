/*
 * morse/filter.h - Small biquad audio filter chain for cleaning CW audio.
 *
 * Real recordings carry low-frequency rumble, mains hum, broadband hiss and
 * out-of-band signals that all hurt decoding. Running the audio through a
 * band-pass that keeps only the CW passband removes most of it before the
 * detector ever sees it, which sharpens the spectrum, the waterfall and the
 * decode alike.
 *
 * The filter is a cascade of RBJ-cookbook biquads (Butterworth-aligned Qs), so
 * it is cheap, stable, and runs sample-by-sample for live streaming.
 */
#ifndef MORSE_FILTER_H
#define MORSE_FILTER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One second-order section (Direct Form I). */
typedef struct morse_biquad {
  double b0, b1, b2, a1, a2; /* normalized coefficients (a0 == 1) */
  double x1, x2, y1, y2;     /* state */
} morse_biquad_t;

#define MORSE_FILTER_MAX_SECTIONS 8

/* A cascade of biquad sections. */
typedef struct morse_filter {
  morse_biquad_t section[MORSE_FILTER_MAX_SECTIONS];
  int n;
  unsigned int rate;
} morse_filter_t;

/* Initialise an empty (pass-through) filter. */
void morse_filter_init(morse_filter_t *f, unsigned int sample_rate);

/* Configure `f` as a band-pass keeping [f_lo, f_hi] Hz. `order` is the rolloff
 * order of each edge (2, 4, 6, or 8; rounded to even, clamped). Higher is
 * steeper. Either edge may be 0 to disable it (low-pass-only or high-pass-only).
 * Resets filter state. Returns 0 on success. */
int morse_filter_bandpass(morse_filter_t *f, double f_lo, double f_hi,
                          int order);

/* Clear the filter's internal state (call before processing a new stream). */
void morse_filter_reset(morse_filter_t *f);

/* Filter `n` samples. `in` and `out` may alias (in-place is fine). */
void morse_filter_process(morse_filter_t *f, const float *in, float *out,
                          size_t n);

/* Process one sample. */
double morse_filter_tick(morse_filter_t *f, double x);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_FILTER_H */
