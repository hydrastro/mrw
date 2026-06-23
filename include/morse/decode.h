/*
 * morse/decode.h - Morse to text.
 *
 * Two layers:
 *
 *   morse_decode_string : a one-shot decoder for clean ".-" text with explicit
 *                         separators. Walks the table's trie per letter.
 *
 *   morse_stream_decoder: an *adaptive* decoder fed a sequence of timed
 *                         mark/space runs (the output of a keyer or of the
 *                         Goertzel tone detector). It estimates the dit length
 *                         on the fly and classifies runs into dits, dahs and
 *                         the three gap kinds, emitting decoded text as it goes.
 *                         This is what makes "decode audio" and "live key"
 *                         work without the operator declaring their speed.
 */
#ifndef MORSE_DECODE_H
#define MORSE_DECODE_H

#include "morse/table.h"
#include "morse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- one-shot string decode ------------------------------------------- */

typedef struct morse_decode_opts {
  char dit;               /* glyph treated as a dit (default '.')             */
  char dah;               /* glyph treated as a dah (default '-')             */
  /* Any run of characters in `letter_sep` ends a letter; any run in
   * `word_sep` ends a word. Defaults: letter_sep=" ", word_sep="/". A second
   * consecutive space is also treated as a word break. */
  const char *letter_sep;
  const char *word_sep;
  char unknown_replacement; /* glyph emitted for an unresolvable letter; 0 = '#' */
} morse_decode_opts_t;

void morse_decode_opts_default(morse_decode_opts_t *opts);

/* Decode pattern text into ds_str `out` (caller-owned, appended to). */
morse_status_t morse_decode_string(const morse_table_t *table,
                                   const char *pattern,
                                   const morse_decode_opts_t *opts,
                                   ds_str_t *out);

/* ---- adaptive streaming decode ---------------------------------------- */

typedef struct morse_stream_decoder morse_stream_decoder_t; /* opaque        */

/* Called with each freshly decoded UTF-8 fragment (usually a single character
 * or a word-break space). `user` is the pointer passed at creation. */
typedef void (*morse_decode_sink_fn)(const char *utf8, void *user);

/* Create with an initial dit estimate in ms (e.g. 60.0 for ~20 WPM); the
 * decoder adapts from there. The table provides the trie used to resolve the
 * accumulated pattern of each letter. */
morse_stream_decoder_t *morse_stream_decoder_create(const morse_table_t *table,
                                                    double initial_dit_ms,
                                                    morse_decode_sink_fn sink,
                                                    void *user);
void morse_stream_decoder_destroy(morse_stream_decoder_t *dec);

/* Push one timed run. `is_mark` true => tone was on for `duration_ms`; false
 * => silence for `duration_ms`. Decoded output is delivered through the sink. */
morse_status_t morse_stream_decoder_push(morse_stream_decoder_t *dec,
                                         int is_mark, double duration_ms);

/* Flush any buffered letter at end of input (e.g. trailing silence). */
morse_status_t morse_stream_decoder_finish(morse_stream_decoder_t *dec);

/* Current adaptive dit estimate (ms) - handy for a speed readout. */
double morse_stream_decoder_dit_ms(const morse_stream_decoder_t *dec);
/* Implied WPM from the current dit estimate. */
double morse_stream_decoder_wpm(const morse_stream_decoder_t *dec);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_DECODE_H */
