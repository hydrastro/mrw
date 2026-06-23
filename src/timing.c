/* timing.c - speeds to concrete millisecond durations (PARIS + Farnsworth). */
#include "morse/timing.h"

static double clampd(double v, double lo, double hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

void morse_timing_default(morse_timing_t *timing) {
  if (timing == NULL) {
    return;
  }
  timing->wpm = 20.0;
  timing->char_wpm = 0.0; /* 0 => no Farnsworth */
  timing->weight = 0.5;   /* standard 3:1 dah:dit */
}

morse_status_t morse_timing_resolve(const morse_timing_t *timing,
                                    morse_durations_t *out) {
  double wpm;
  double cwpm;
  double weight;
  double unit;     /* dit unit at the element speed (ms) */
  double w2;       /* weighting multiplier */

  if (timing == NULL || out == NULL) {
    return MORSE_ERR_NULL;
  }
  wpm = clampd(timing->wpm, 1.0, 120.0);
  weight = clampd(timing->weight, 0.25, 0.75);

  /* Character speed: anything <= overall speed disables Farnsworth. */
  cwpm = timing->char_wpm;
  if (cwpm <= wpm) {
    cwpm = wpm;
  }
  cwpm = clampd(cwpm, 1.0, 120.0);

  unit = 1200.0 / cwpm; /* element-speed dit, ms */

  /* Weighting redistributes the mark/space balance while preserving the total
   * dit+dah+gap budget: weight 0.5 keeps the classic 1/3 and 1-unit gap. We
   * scale the mark elements by w2 and the intra gap inversely so a dit+gap pair
   * still spans 2 units. */
  w2 = weight / 0.5;

  out->dit_ms = unit * w2;
  out->dah_ms = 3.0 * unit * w2;
  out->intra_gap_ms = unit * (2.0 - w2);

  if (timing->char_wpm > wpm) {
    /* ARRL Farnsworth: stretch only the inter-character and inter-word gaps so
     * the overall throughput is `wpm` while characters stay at `cwpm`.
     *   total delay per standard word ta = (60*C - 37.2*S)/(C*S) seconds
     * distributed over 19 units (4 char-gaps * 3u + 1 word-gap * 7u). */
    double ta_ms = ((60.0 * cwpm - 37.2 * wpm) / (cwpm * wpm)) * 1000.0;
    double unit_delay;
    if (ta_ms < 0.0) {
      ta_ms = 0.0;
    }
    unit_delay = ta_ms / 19.0;
    out->char_gap_ms = 3.0 * unit_delay;
    out->word_gap_ms = 7.0 * unit_delay;
  } else {
    out->char_gap_ms = 3.0 * unit;
    out->word_gap_ms = 7.0 * unit;
  }
  return MORSE_OK;
}

double morse_duration_of(const morse_durations_t *d, morse_symbol_t symbol) {
  if (d == NULL) {
    return 0.0;
  }
  switch (symbol) {
  case MORSE_SYM_DIT:
    return d->dit_ms;
  case MORSE_SYM_DAH:
    return d->dah_ms;
  case MORSE_SYM_INTRA_GAP:
    return d->intra_gap_ms;
  case MORSE_SYM_CHAR_GAP:
    return d->char_gap_ms;
  case MORSE_SYM_WORD_GAP:
    return d->word_gap_ms;
  case MORSE_SYM_COUNT:
  default:
    return 0.0;
  }
}
