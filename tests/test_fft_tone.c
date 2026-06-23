/* test_fft_tone.c - FFT correctness and arbitrary-tone blind decoding.
 *
 * Verifies that the spectral estimator locates pure tones to within a couple of
 * Hz, and that the detector decodes the same message rendered at a wide range
 * of pitches *without being told the frequency*. */
#include "morse/fft.h"
#include "morse/morse.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void make_sine(float *x, size_t n, double f, unsigned sr) {
  size_t i;
  for (i = 0; i < n; ++i) {
    x[i] = (float)(0.8 * sin(2.0 * M_PI * f * (double)i / (double)sr));
  }
}

static void test_fft_basics(void) {
  size_t n = 256, i;
  double *re, *im, *r0;
  CHECK(morse_is_pow2(1));
  CHECK(morse_is_pow2(1024));
  CHECK(!morse_is_pow2(0));
  CHECK(!morse_is_pow2(1000));
  CHECK_INT((int)morse_floor_pow2(1000), 512);
  CHECK_INT((int)morse_floor_pow2(1024), 1024);
  CHECK_INT((int)morse_floor_pow2(1), 1);

  re = malloc(n * sizeof(double));
  im = malloc(n * sizeof(double));
  r0 = malloc(n * sizeof(double));
  for (i = 0; i < n; ++i) {
    r0[i] = sin(2.0 * M_PI * 5.0 * (double)i / (double)n) +
            0.3 * cos(2.0 * M_PI * 17.0 * (double)i / (double)n);
    re[i] = r0[i];
    im[i] = 0.0;
  }
  CHECK_INT(morse_fft(re, im, n, 0), 0);
  CHECK_INT(morse_fft(re, im, n, 1), 0);
  for (i = 0; i < n; ++i) {
    CHECK_NEAR(re[i], r0[i], 1e-9);
  }
  free(re);
  free(im);
  free(r0);
}

static void test_tone_estimate(void) {
  unsigned sr = 44100;
  size_t n = 16384;
  float *x = malloc(n * sizeof(float));
  double freqs[6];
  size_t i;
  freqs[0] = 180.0; freqs[1] = 437.0; freqs[2] = 600.0;
  freqs[3] = 853.0; freqs[4] = 1500.0; freqs[5] = 2400.0;
  for (i = 0; i < 6; ++i) {
    double strength = 0.0;
    double est;
    make_sine(x, n, freqs[i], sr);
    est = morse_dominant_tone(x, n, sr, 100.0, 3000.0, 4096, &strength);
    CHECK_NEAR(est, freqs[i], 3.0);
    CHECK(strength > 0.5);
  }
  free(x);
}

static void decode_at_tone(double tone_hz, const char *msg) {
  morse_synth_opts_t so;
  morse_timing_t tm;
  morse_durations_t dur;
  morse_pcm_t pcm;
  morse_detect_opts_t det;
  morse_table_t *table;
  list_t *els;
  ds_str_t *out;
  const char *got;
  double total_ms = 0.0;

  morse_timing_default(&tm);
  tm.wpm = 20.0;
  morse_timing_resolve(&tm, &dur);
  morse_synth_opts_default(&so);
  so.tone_hz = tone_hz;
  so.sample_rate = 44100;

  table = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  els = ds_list_create();
  CHECK_INT(morse_encode_elements(table, msg, &dur, MORSE_UNKNOWN_SKIP, els,
                                  &total_ms),
            MORSE_OK);
  morse_pcm_init(&pcm);
  CHECK_INT(morse_synth_render(els, &so, &pcm), MORSE_OK);
  morse_elements_free(els);

  morse_detect_opts_default(&det); /* tone_hz = 0 => blind auto-detect */
  out = ds_str_create();
  morse_detect_pcm(&pcm, table, &det, out, NULL);
  got = FUNC_str_cstr(out);
  CHECK_STR(got, msg);
  ds_str_destroy(out);
  morse_pcm_free(&pcm);
  morse_table_destroy(table);
}

static void test_blind_decode_many_tones(void) {
  double tones[7];
  size_t i;
  const char *msg = "PARIS DE K1ABC";
  tones[0] = 200.0; tones[1] = 350.0; tones[2] = 600.0; tones[3] = 750.0;
  tones[4] = 1000.0; tones[5] = 1750.0; tones[6] = 2400.0;
  for (i = 0; i < 7; ++i) {
    decode_at_tone(tones[i], msg);
  }
}


typedef struct { char buf[256]; size_t n; } sink_acc;
static void stream_sink(const char *u, void *user) {
  sink_acc *a = (sink_acc *)user;
  while (*u && a->n + 1 < sizeof(a->buf)) a->buf[a->n++] = *u++;
  a->buf[a->n] = 0;
}

static void test_streaming_tracks_tone(void) {
  /* Render at 1500 Hz (far from the 600 Hz live default) and feed it through
   * the streaming detector in small chunks; tracking should lock the pitch. */
  morse_synth_opts_t so; morse_timing_t tm; morse_durations_t dur;
  morse_pcm_t pcm; morse_detect_opts_t det; morse_table_t *table;
  list_t *els; morse_detector_t *d; sink_acc acc; double total_ms = 0.0;
  size_t off; const char *msg = "TEST";
  memset(&acc, 0, sizeof(acc));
  morse_timing_default(&tm); tm.wpm = 18.0; morse_timing_resolve(&tm, &dur);
  morse_synth_opts_default(&so); so.tone_hz = 1500.0; so.sample_rate = 44100;
  table = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  els = ds_list_create();
  CHECK_INT(morse_encode_elements(table, msg, &dur, MORSE_UNKNOWN_SKIP, els, &total_ms), MORSE_OK);
  morse_pcm_init(&pcm);
  CHECK_INT(morse_synth_render(els, &so, &pcm), MORSE_OK);
  morse_elements_free(els);
  morse_detect_opts_default(&det); /* tone 0 => track */
  d = morse_detector_create(table, pcm.sample_rate, &det, stream_sink, &acc);
  CHECK(d != NULL);
  for (off = 0; off < pcm.count; off += 1000) {
    size_t n = (off + 1000 <= pcm.count) ? 1000 : (pcm.count - off);
    morse_detector_process(d, pcm.samples + off, n, NULL);
  }
  morse_detector_destroy(d); /* flushes */
  /* The streamed result should contain the message (allow trailing space). */
  CHECK(strstr(acc.buf, "TEST") != NULL);
  morse_pcm_free(&pcm);
  morse_table_destroy(table);
}

TEST_BEGIN("fft + arbitrary-tone decoding")
test_fft_basics();
test_tone_estimate();
test_blind_decode_many_tones();
test_streaming_tracks_tone();
TEST_END()
