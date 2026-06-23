/* test_multi.c - Decode two simultaneous stations on different pitches. */
#include "morse/morse.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Render `text` at `wpm`/`tone` into a freshly sized float buffer. */
static float *render(const char *text, double wpm, double tone,
                     unsigned int rate, size_t *out_n) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  morse_timing_t tm;
  morse_durations_t dur;
  morse_synth_opts_t so;
  list_t *els;
  double total_ms = 0.0;
  morse_pcm_t pcm;
  float *buf;

  morse_timing_default(&tm);
  tm.wpm = wpm;
  morse_timing_resolve(&tm, &dur);
  morse_synth_opts_default(&so);
  so.sample_rate = rate;
  so.tone_hz = tone;
  so.amplitude = 0.6;

  els = ds_list_create();
  morse_encode_elements(t, text, &dur, MORSE_UNKNOWN_SKIP, els, &total_ms);
  morse_pcm_init(&pcm);
  morse_synth_render(els, &so, &pcm);
  morse_elements_free(els);
  morse_table_destroy(t);

  *out_n = pcm.count;
  buf = (float *)malloc(sizeof(float) * pcm.count);
  memcpy(buf, pcm.samples, sizeof(float) * pcm.count);
  morse_pcm_free(&pcm);
  return buf;
}

/* Does `hay` contain `needle` ignoring spaces? */
static int contains_squashed(const char *hay, const char *needle) {
  char a[512], b[256];
  size_t i, j = 0;
  if (!hay) {
    return 0;
  }
  for (i = 0; hay[i] && j < sizeof(a) - 1; ++i) {
    if (hay[i] != ' ') {
      a[j++] = hay[i];
    }
  }
  a[j] = 0;
  j = 0;
  for (i = 0; needle[i] && j < sizeof(b) - 1; ++i) {
    if (needle[i] != ' ') {
      b[j++] = needle[i];
    }
  }
  b[j] = 0;
  return strstr(a, b) != NULL;
}

TEST_BEGIN("multi-station");
{
  const unsigned int rate = 44100;
  const char *textA = "PARIS";
  const char *textB = "TEST";
  size_t nA = 0, nB = 0, n, i;
  float *a, *b, *mix;
  morse_table_t *table;
  morse_multi_detector_t *md;
  morse_multi_opts_t opts;
  size_t nch, ch;
  int foundA = 0, foundB = 0, near600 = 0, near900 = 0;

  a = render(textA, 20.0, 600.0, rate, &nA);
  b = render(textB, 18.0, 900.0, rate, &nB);
  n = nA > nB ? nA : nB;
  mix = (float *)calloc(n, sizeof(float));
  for (i = 0; i < nA; ++i) {
    mix[i] += 0.5f * a[i];
  }
  for (i = 0; i < nB; ++i) {
    mix[i] += 0.5f * b[i];
  }

  table = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  morse_multi_opts_default(&opts);
  opts.tone_min_hz = 300.0;
  opts.tone_max_hz = 1500.0;
  md = morse_multi_create(table, rate, &opts, NULL, NULL);
  CHECK(md != NULL);

  for (i = 0; i < n; i += 2048) {
    size_t step = n - i < 2048 ? n - i : 2048;
    morse_multi_process(md, mix + i, step);
  }
  morse_multi_finish(md);

  nch = morse_multi_channel_count(md);
  CHECK(nch >= 2);

  for (ch = 0; ch < nch; ++ch) {
    morse_multi_channel_info_t info;
    const char *txt = morse_multi_channel_text(md, ch);
    if (!morse_multi_get_channel(md, ch, &info) || txt == NULL) {
      continue;
    }
    if (fabs(info.tone_hz - 600.0) < 40.0) {
      near600 = 1;
      if (contains_squashed(txt, textA)) {
        foundA = 1;
      }
    }
    if (fabs(info.tone_hz - 900.0) < 40.0) {
      near900 = 1;
      if (contains_squashed(txt, textB)) {
        foundB = 1;
      }
    }
  }

  CHECK(near600);
  CHECK(near900);
  CHECK(foundA);
  CHECK(foundB);

  morse_multi_destroy(md);
  morse_table_destroy(table);
  free(a);
  free(b);
  free(mix);
}

/* Scenario 2: stations transmit one after another on different pitches. The
 * idle channel must not fill with leakage garbage while the other transmits,
 * and the timeline must order the earlier station's text before the later's. */
{
  const unsigned int rate = 44100;
  size_t nB = 0, nA = 0, gap, total, i;
  float *b, *a, *seq;
  morse_table_t *table;
  morse_multi_detector_t *md;
  morse_multi_opts_t opts;
  size_t nch, ch, ne;
  int ch900_clean = 0, ch600_ok = 0, seen900 = 0, seen600 = 0;
  double last900 = -1.0, first600 = 1e9;

  b = render("TEST", 18.0, 900.0, rate, &nB); /* first */
  a = render("PARIS", 20.0, 600.0, rate, &nA); /* later */
  gap = rate / 2;                              /* 0.5 s silence between them */
  total = nB + gap + nA + gap;
  seq = (float *)calloc(total, sizeof(float));
  for (i = 0; i < nB; ++i) {
    seq[i] = 0.6f * b[i];
  }
  for (i = 0; i < nA; ++i) {
    seq[nB + gap + i] = 0.7f * a[i]; /* A is the stronger, later station */
  }

  table = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  morse_multi_opts_default(&opts);
  opts.tone_min_hz = 300.0;
  opts.tone_max_hz = 1500.0;
  md = morse_multi_create(table, rate, &opts, NULL, NULL);

  for (i = 0; i < total; i += 2048) {
    size_t step = total - i < 2048 ? total - i : 2048;
    morse_multi_process(md, seq + i, step);
  }
  morse_multi_finish(md);

  nch = morse_multi_channel_count(md);
  for (ch = 0; ch < nch; ++ch) {
    morse_multi_channel_info_t info;
    const char *txt = morse_multi_channel_text(md, ch);
    if (!morse_multi_get_channel(md, ch, &info) || txt == NULL) {
      continue;
    }
    if (fabs(info.tone_hz - 900.0) < 40.0) {
      /* TEST is 4 chars; allow a trailing space but no garbage characters */
      ch900_clean = contains_squashed(txt, "TEST") && strlen(txt) <= 6;
    }
    if (fabs(info.tone_hz - 600.0) < 40.0) {
      ch600_ok = contains_squashed(txt, "PARIS");
    }
  }
  CHECK(ch900_clean); /* idle channel stayed clean while A transmitted */
  CHECK(ch600_ok);

  /* timeline ordering: every 900 Hz event precedes every 600 Hz event */
  ne = morse_multi_event_count(md);
  CHECK(ne > 0);
  for (i = 0; i < ne; ++i) {
    morse_multi_event_t e;
    if (!morse_multi_get_event(md, i, &e)) {
      continue;
    }
    if (fabs(e.tone_hz - 900.0) < 40.0) {
      seen900 = 1;
      if (e.t_seconds > last900) {
        last900 = e.t_seconds;
      }
    }
    if (fabs(e.tone_hz - 600.0) < 40.0) {
      seen600 = 1;
      if (e.t_seconds < first600) {
        first600 = e.t_seconds;
      }
    }
  }
  CHECK(seen900 && seen600);
  CHECK(last900 < first600); /* B (900) fully precedes A (600) in the timeline */

  morse_multi_destroy(md);
  morse_table_destroy(table);
  free(b);
  free(a);
  free(seq);
}
TEST_END()
