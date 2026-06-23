/*
 * morse/timing.h - Converting speeds into concrete element durations.
 *
 * We follow the PARIS standard: the reference word "PARIS " is exactly 50 dit
 * units long, so at W words-per-minute one dit lasts 1200/W milliseconds.
 *
 * Farnsworth timing keeps the *characters* crisp (sent at a higher character
 * speed) while stretching the inter-character and inter-word gaps so the
 * overall throughput matches a slower target speed - the standard way to learn
 * Morse without ingraining sloppy character formation. We use the ARRL formula
 * (see "A Standard for Morse Timing Using the Farnsworth Technique").
 */
#ifndef MORSE_TIMING_H
#define MORSE_TIMING_H

#include "morse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct morse_timing {
  double wpm;            /* overall / Farnsworth target speed (words/min)     */
  double char_wpm;       /* character speed; <= wpm disables Farnsworth       */
  double weight;         /* dah/dit weighting, 0.5 = standard 3:1 ratio       */
} morse_timing_t;

/* Concrete millisecond durations after resolving a timing spec. */
typedef struct morse_durations {
  double dit_ms;
  double dah_ms;
  double intra_gap_ms;   /* between elements within a character (1 unit)      */
  double char_gap_ms;    /* between characters (3 units, Farnsworth-stretched)*/
  double word_gap_ms;    /* between words (7 units, Farnsworth-stretched)     */
} morse_durations_t;

/* Sensible defaults: 20 WPM, no Farnsworth, standard weighting. */
void morse_timing_default(morse_timing_t *timing);

/* Resolve a timing spec into per-symbol durations. Clamps insane inputs:
 * wpm and char_wpm are forced into [1, 120]; weight into [0.25, 0.75]. */
morse_status_t morse_timing_resolve(const morse_timing_t *timing,
                                    morse_durations_t *out);

/* Duration of a single symbol given resolved durations. */
double morse_duration_of(const morse_durations_t *d, morse_symbol_t symbol);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_TIMING_H */
