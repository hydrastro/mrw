/*
 * morse/encode.h - Text to Morse.
 *
 * Two output shapes are offered:
 *   1. a human readable pattern string  ("... --- ..."), and
 *   2. an intrusive ds list of timed morse_element_t, ready for the synth or
 *      for an oscilloscope/light renderer.
 *
 * Input is interpreted as UTF-8. ASCII is handled directly; a small set of
 * accented Latin code points is decoded from UTF-8 so that the extended
 * variant's accented entries are reachable from real text.
 */
#ifndef MORSE_ENCODE_H
#define MORSE_ENCODE_H

#include "morse/table.h"
#include "morse/timing.h"
#include "morse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rendering knobs for the string form. */
typedef struct morse_encode_opts {
  morse_unknown_policy_t unknown;  /* how to treat uncodeable characters      */
  char dit;                        /* glyph for a dit  (default '.')          */
  char dah;                        /* glyph for a dah  (default '-')          */
  const char *element_sep;         /* between elements of a char (default "") */
  const char *letter_sep;          /* between letters           (default " ") */
  const char *word_sep;            /* between words             (default " / ")*/
} morse_encode_opts_t;

void morse_encode_opts_default(morse_encode_opts_t *opts);

/*
 * Encode UTF-8 `text` to a pattern string written into ds_str `out` (which the
 * caller creates and owns; this function only appends). Prosigns may be written
 * inline as <NAME> tokens, e.g. "<SOS>" or "<AR>", and are emitted with no
 * internal letter gaps.
 */
morse_status_t morse_encode_string(const morse_table_t *table,
                                   const char *text,
                                   const morse_encode_opts_t *opts,
                                   ds_str_t *out);

/*
 * Encode UTF-8 `text` into a list of timed elements appended to `out_list`
 * (a ds list the caller created). Each node is a heap-allocated
 * morse_element_t; free them by destroying the list with
 * morse_element_list_free as the callback, or call morse_elements_free.
 *
 * `total_ms_out` (optional) receives the full stream duration.
 */
morse_status_t morse_encode_elements(const morse_table_t *table,
                                     const char *text,
                                     const morse_durations_t *durations,
                                     morse_unknown_policy_t unknown,
                                     list_t *out_list, double *total_ms_out);

/* Free every element a previous morse_encode_elements call appended. */
void morse_elements_free(list_t *list);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_ENCODE_H */
