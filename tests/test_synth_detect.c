/* test_synth_detect.c - Audio synthesis envelope + Goertzel detection. */
#include "morse/detect.h"
#include "morse/encode.h"
#include "morse/synth.h"
#include "morse/table.h"
#include "morse/timing.h"
#include "test_util.h"

static list_t *elements_for(morse_table_t *t, const char *text, double *total) {
  morse_timing_t tm;
  morse_durations_t d;
  list_t *els = ds_list_create();
  morse_timing_default(&tm);
  morse_timing_resolve(&tm, &d);
  morse_encode_elements(t, text, &d, MORSE_UNKNOWN_SKIP, els, total);
  return els;
}

static void test_synth_shape(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  morse_synth_opts_t so;
  morse_pcm_t pcm;
  double total = 0.0;
  list_t *els;
  size_t i;
  float peak = 0.0f;

  morse_synth_opts_default(&so);
  so.sample_rate = 8000u;
  so.tone_hz = 600.0;
  so.amplitude = 0.7;
  els = elements_for(t, "PARIS", &total);
  morse_pcm_init(&pcm);
  CHECK_INT(morse_synth_render(els, &so, &pcm), MORSE_OK);

  /* Sample count is consistent with the element stream total duration. */
  CHECK_INT(pcm.sample_rate, 8000);
  CHECK(pcm.count > 0);
  CHECK_NEAR((double)pcm.count, total / 1000.0 * 8000.0,
             /* within a few samples of rounding */ 8.0);

  /* Envelope never exceeds the requested amplitude (plus tiny epsilon) and the
   * signal actually reaches near it somewhere. */
  for (i = 0; i < pcm.count; i++) {
    float a = pcm.samples[i] < 0 ? -pcm.samples[i] : pcm.samples[i];
    if (a > peak) {
      peak = a;
    }
    CHECK(a <= 0.7f + 1e-3f);
  }
  CHECK(peak > 0.6f);

  /* First and last samples sit on the envelope ramp, so they start near zero
   * (click-free keying). */
  CHECK_NEAR((double)pcm.samples[0], 0.0, 0.05);

  morse_pcm_free(&pcm);
  morse_elements_free(els);
  morse_table_destroy(t);
}

static void test_duration_helper(void) {
  morse_pcm_t pcm;
  morse_pcm_init(&pcm);
  pcm.sample_rate = 1000u;
  pcm.count = 2500u;
  pcm.samples = NULL; /* duration must not touch samples */
  CHECK_NEAR(morse_pcm_duration_sec(&pcm), 2.5, 1e-9);
}

static void detect_text(const char *text, double tone, int noise) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  morse_synth_opts_t so;
  morse_detect_opts_t det;
  morse_pcm_t pcm;
  morse_envelope_t env;
  ds_str_t *out = ds_str_create();
  double total = 0.0;
  list_t *els = elements_for(t, text, &total);

  morse_synth_opts_default(&so);
  so.tone_hz = tone;
  so.add_noise = noise;
  so.noise_amplitude = noise ? 0.12 : 0.0;

  morse_pcm_init(&pcm);
  CHECK_INT(morse_synth_render(els, &so, &pcm), MORSE_OK);

  morse_detect_opts_default(&det);
  morse_envelope_init(&env);
  CHECK_INT(morse_detect_pcm(&pcm, t, &det, out, &env), MORSE_OK);
  CHECK_STR(FUNC_str_cstr(out), text);
  /* Envelope was populated. */
  CHECK(env.count > 0);
  CHECK(env.tone_hz > 0.0);

  morse_envelope_free(&env);
  ds_str_destroy(out);
  morse_pcm_free(&pcm);
  morse_elements_free(els);
  morse_table_destroy(t);
}

static void test_detect_clean(void) {
  detect_text("HELLO WORLD", 600.0, 0);
  detect_text("CQ DE K1ABC", 700.0, 0);
  detect_text("PARIS", 550.0, 0);
}

static void test_detect_noise(void) {
  /* Modest additive noise should still decode at our SNR. */
  detect_text("HELLO WORLD", 600.0, 1);
}

static void test_auto_tone(void) {
  /* With threshold/tone on auto, a 800 Hz signal is found and decoded. */
  detect_text("SOS", 800.0, 0);
}

TEST_BEGIN("synth_detect")
test_synth_shape();
test_duration_helper();
test_detect_clean();
test_detect_noise();
test_auto_tone();
TEST_END()
