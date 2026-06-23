/*
 * morse.c - Top-level convenience entry points and diagnostics hook.
 *
 * These wrap the lower layers into one-call "text on disk as a WAV" and "WAV on
 * disk as text" operations for scripts and the CLI, and expose the process-wide
 * allocation counters that the GUI's diagnostics panel reads. Nothing here adds
 * new behaviour; it is glue over table/encode/synth/wav and table/wav/detect.
 */
#include "morse/morse.h"

#include "morse_alloc.h"

#include <stdio.h>

/* ---- version ------------------------------------------------------------- */

const char *morse_version_string(void) {
  /* Compile-time assembled "MAJOR.MINOR.PATCH". */
#define MORSE_STR2(x) #x
#define MORSE_STR(x) MORSE_STR2(x)
  return MORSE_STR(MORSE_VERSION_MAJOR) "." MORSE_STR(
      MORSE_VERSION_MINOR) "." MORSE_STR(MORSE_VERSION_PATCH);
#undef MORSE_STR
#undef MORSE_STR2
}

/* ---- text -> WAV --------------------------------------------------------- */

morse_status_t morse_text_to_wav(const char *text, morse_variant_t variant,
                                 const morse_timing_t *timing,
                                 const morse_synth_opts_t *synth,
                                 const char *path) {
  morse_table_t *table;
  morse_timing_t tm;
  morse_durations_t dur;
  morse_synth_opts_t so;
  list_t *elements;
  morse_pcm_t pcm;
  double total_ms = 0.0;
  morse_status_t st;

  if (text == NULL || path == NULL) {
    return MORSE_ERR_NULL;
  }

  table = morse_table_create(variant);
  if (table == NULL) {
    return MORSE_ERR_ALLOC;
  }

  if (timing != NULL) {
    tm = *timing;
  } else {
    morse_timing_default(&tm);
  }
  st = morse_timing_resolve(&tm, &dur);
  if (st != MORSE_OK) {
    morse_table_destroy(table);
    return st;
  }

  if (synth != NULL) {
    so = *synth;
  } else {
    morse_synth_opts_default(&so);
  }

  elements = ds_list_create();
  if (elements == NULL) {
    morse_table_destroy(table);
    return MORSE_ERR_ALLOC;
  }

  st = morse_encode_elements(table, text, &dur, MORSE_UNKNOWN_SKIP, elements,
                             &total_ms);
  if (st != MORSE_OK) {
    morse_elements_free(elements);
    morse_table_destroy(table);
    return st;
  }

  morse_pcm_init(&pcm);
  st = morse_synth_render(elements, &so, &pcm);
  if (st != MORSE_OK) {
    morse_pcm_free(&pcm);
    morse_elements_free(elements);
    morse_table_destroy(table);
    return st;
  }

  st = morse_wav_write(path, &pcm);

  morse_pcm_free(&pcm);
  morse_elements_free(elements);
  morse_table_destroy(table);
  return st;
}

/* ---- WAV -> text --------------------------------------------------------- */

morse_status_t morse_wav_to_text(const char *path, morse_variant_t variant,
                                 const morse_detect_opts_t *detect,
                                 ds_str_t *out_text) {
  morse_table_t *table;
  morse_detect_opts_t dop;
  morse_pcm_t pcm;
  morse_status_t st;

  if (path == NULL || out_text == NULL) {
    return MORSE_ERR_NULL;
  }

  morse_pcm_init(&pcm);
  st = morse_wav_read(path, &pcm);
  if (st != MORSE_OK) {
    morse_pcm_free(&pcm);
    return st;
  }

  table = morse_table_create(variant);
  if (table == NULL) {
    morse_pcm_free(&pcm);
    return MORSE_ERR_ALLOC;
  }

  if (detect != NULL) {
    dop = *detect;
  } else {
    morse_detect_opts_default(&dop);
  }

  /* Decode without keeping the envelope here (callers that want it use the
   * detect API directly). */
  st = morse_detect_pcm(&pcm, table, &dop, out_text, NULL);

  morse_table_destroy(table);
  morse_pcm_free(&pcm);
  return st;
}

/* ---- diagnostics --------------------------------------------------------- */

void morse_diagnostics_enable(void) { morse_alloc_enable_diagnostics(); }

int morse_diagnostics_get(morse_alloc_stats_t *out) {
  morse_alloc_counters_t c;
  if (!morse_alloc_diagnostics_enabled()) {
    return 0;
  }
  morse_alloc_get_counters(&c);
  if (out != NULL) {
    out->allocations = c.allocations;
    out->frees = c.frees;
    out->reallocations = c.reallocations;
    out->failed_allocations = c.failed_allocations;
    out->bytes_live = c.bytes_live;
    out->bytes_peak = c.bytes_peak;
    out->bytes_total = c.bytes_total;
  }
  return 1;
}
