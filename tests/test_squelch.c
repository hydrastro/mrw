/* test_squelch.c - the live detector's SNR squelch keeps noise from decoding
 * into stray dits and dashes, while real signals still decode. */
#include "morse/morse.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void sink_collect(const char *utf8, void *user) {
  ds_str_append_cstr((ds_str_t *)user, utf8);
}

static list_t *elements_for(morse_table_t *t, const char *text) {
  morse_timing_t tm;
  morse_durations_t d;
  double total = 0.0;
  list_t *els = ds_list_create();
  morse_timing_default(&tm);
  morse_timing_resolve(&tm, &d);
  morse_encode_elements(t, text, &d, MORSE_UNKNOWN_SKIP, els, &total);
  return els;
}

/* Feed a buffer to a fresh fixed-tone live detector (as the multi decoder
 * configures it) and collect the decoded text. */
static void decode_live(morse_table_t *t, const float *buf, size_t n,
                        double tone, ds_str_t *out) {
  morse_detect_opts_t o;
  morse_detector_t *det;
  size_t i;
  morse_detect_opts_default(&o);
  o.tone_hz = tone;
  o.track_tone = 0;
  o.block_size = 512;
  det = morse_detector_create(t, 44100, &o, sink_collect, out);
  for (i = 0; i < n; i += 2048) {
    size_t step = n - i < 2048 ? n - i : 2048;
    morse_detector_process(det, buf + i, step, NULL);
  }
  morse_detector_destroy(det); /* flushes the trailing character */
}

static float *render(morse_table_t *t, const char *text, double tone,
                     size_t *out_n) {
  morse_synth_opts_t so;
  morse_pcm_t pcm;
  list_t *els = elements_for(t, text);
  float *buf;
  morse_synth_opts_default(&so);
  so.tone_hz = tone;
  morse_pcm_init(&pcm);
  morse_synth_render(els, &so, &pcm);
  buf = (float *)malloc(pcm.count * sizeof(float));
  memcpy(buf, pcm.samples, pcm.count * sizeof(float));
  *out_n = pcm.count;
  morse_pcm_free(&pcm);
  morse_elements_free(els);
  return buf;
}

TEST_BEGIN("squelch");
{
  const unsigned int rate = 44100;
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  unsigned int seed = 2463534242u;
  size_t i;

  /* 1) Three seconds of pure white noise must not decode into a flood of
   *    characters. Without the squelch the decaying-peak detector eventually
   *    reads the noise as marks and produces E/T/I soup. */
  {
    size_t n = rate * 3;
    float *noise = (float *)malloc(n * sizeof(float));
    ds_str_t *out = ds_str_create();
    for (i = 0; i < n; ++i) {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      noise[i] = 0.25f * ((float)(seed & 0xFFFF) / 32768.0f - 1.0f);
    }
    decode_live(t, noise, n, 700.0, out);
    CHECK(strlen(FUNC_str_cstr(out)) <= 2);
    ds_str_destroy(out);
    free(noise);
  }

  /* 2) A clean signal still decodes exactly. */
  {
    size_t n = 0;
    float *sig = render(t, "PARIS", 700.0, &n);
    ds_str_t *out = ds_str_create();
    decode_live(t, sig, n, 700.0, out);
    CHECK(strcmp(FUNC_str_cstr(out), "PARIS") == 0);
    ds_str_destroy(out);
    free(sig);
  }

  /* 3) A real word followed by a long noisy tail decodes the word without a
   *    trailing run of phantom characters. */
  {
    size_t sn = 0, tail = rate * 2, total, k;
    float *sig = render(t, "TEST", 700.0, &sn);
    float *mix = (float *)malloc((sn + tail) * sizeof(float));
    ds_str_t *out = ds_str_create();
    const char *txt;
    total = sn + tail;
    memcpy(mix, sig, sn * sizeof(float));
    for (k = 0; k < tail; ++k) {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      mix[sn + k] = 0.2f * ((float)(seed & 0xFFFF) / 32768.0f - 1.0f);
    }
    decode_live(t, mix, total, 700.0, out);
    txt = FUNC_str_cstr(out);
    CHECK(strncmp(txt, "TEST", 4) == 0); /* word recovered */
    CHECK(strlen(txt) <= 6);             /* no soup in the tail */
    ds_str_destroy(out);
    free(mix);
    free(sig);
  }

  morse_table_destroy(t);
}
TEST_END()
