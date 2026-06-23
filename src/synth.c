/*
 * morse/synth.c - Element stream -> mono float PCM.
 *
 * Each mark is a sine tone shaped by a raised-cosine (Hann) envelope so the
 * keyed waveform has no hard edges. Hard gating of a sinusoid splatters energy
 * across the spectrum ("key clicks"); a few milliseconds of cosine taper on
 * each end confines the signal to a narrow band, the same trick a real CW
 * transmitter uses. Phase is carried continuously across marks so there is
 * never a discontinuity at a tone onset.
 */
#include "morse/synth.h"
#include "morse_alloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- options ---------------------------------------------------------- */

void morse_synth_opts_default(morse_synth_opts_t *opts) {
  if (!opts) {
    return;
  }
  opts->sample_rate = 44100u;
  opts->tone_hz = 600.0;
  opts->amplitude = 0.7;
  opts->ramp_ms = 5.0;
  opts->add_noise = 0;
  opts->noise_amplitude = 0.05;
}

/* ---- PCM buffer ------------------------------------------------------- */

void morse_pcm_init(morse_pcm_t *pcm) {
  if (!pcm) {
    return;
  }
  pcm->samples = NULL;
  pcm->count = 0;
  pcm->capacity = 0;
  pcm->sample_rate = 0;
}

void morse_pcm_free(morse_pcm_t *pcm) {
  if (!pcm) {
    return;
  }
  morse_xfree(pcm->samples);
  pcm->samples = NULL;
  pcm->count = 0;
  pcm->capacity = 0;
  pcm->sample_rate = 0;
}

double morse_pcm_duration_sec(const morse_pcm_t *pcm) {
  if (!pcm || pcm->sample_rate == 0) {
    return 0.0;
  }
  return (double)pcm->count / (double)pcm->sample_rate;
}

/* Ensure room for at least `extra` more samples. */
static morse_status_t pcm_reserve(morse_pcm_t *pcm, size_t extra) {
  size_t need = pcm->count + extra;
  if (need <= pcm->capacity) {
    return MORSE_OK;
  }
  {
    size_t cap = pcm->capacity ? pcm->capacity : 1024;
    float *grown;
    while (cap < need) {
      cap += cap / 2 + 1; /* ~1.5x growth */
    }
    grown = (float *)morse_xrealloc(pcm->samples, cap * sizeof(float));
    if (!grown) {
      return MORSE_ERR_ALLOC;
    }
    pcm->samples = grown;
    pcm->capacity = cap;
  }
  return MORSE_OK;
}

/* ---- noise ------------------------------------------------------------ */

/* Tiny xorshift32 so noise is self-contained and doesn't disturb rand(). */
static unsigned int g_noise_state = 0x1234567u;
static double noise_unit(void) {
  unsigned int x = g_noise_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_noise_state = x;
  /* map to [-1, 1] */
  return ((double)x / 2147483648.0) - 1.0;
}

static float clampf(float v) {
  if (v > 1.0f) {
    return 1.0f;
  }
  if (v < -1.0f) {
    return -1.0f;
  }
  return v;
}

/* ---- core renderer ---------------------------------------------------- */

/* Append `n` samples for one symbol. Marks emit an enveloped tone advancing
 * *phase; gaps emit silence. Noise (if enabled) is mixed across every sample,
 * including silences, which is what a real noisy channel looks like. */
static morse_status_t render_symbol(morse_pcm_t *pcm,
                                    const morse_synth_opts_t *o, double dur_ms,
                                    morse_symbol_t symbol, double *phase) {
  size_t n, i, ramp;
  int is_mark = morse_symbol_is_mark(symbol);
  double sr = (double)o->sample_rate;
  double w = 2.0 * M_PI * o->tone_hz / sr; /* radians per sample */
  morse_status_t st;

  if (dur_ms < 0.0) {
    dur_ms = 0.0;
  }
  n = (size_t)(dur_ms * sr / 1000.0 + 0.5);
  if (n == 0) {
    return MORSE_OK;
  }
  st = pcm_reserve(pcm, n);
  if (st != MORSE_OK) {
    return st;
  }

  ramp = (size_t)(o->ramp_ms * sr / 1000.0 + 0.5);
  if (ramp * 2 > n) {
    ramp = n / 2; /* short element: ramp meets in the middle */
  }

  for (i = 0; i < n; ++i) {
    float s = 0.0f;
    if (is_mark) {
      double env = 1.0;
      if (ramp > 0) {
        if (i < ramp) {
          env = 0.5 * (1.0 - cos(M_PI * (double)i / (double)ramp));
        } else if (i >= n - ramp) {
          size_t k = n - 1 - i; /* counts down through the fall */
          env = 0.5 * (1.0 - cos(M_PI * (double)k / (double)ramp));
        }
      }
      s = (float)(o->amplitude * env * sin(*phase));
      *phase += w;
      if (*phase >= 2.0 * M_PI) {
        *phase -= 2.0 * M_PI;
      }
    }
    if (o->add_noise) {
      s += (float)(o->noise_amplitude * noise_unit());
    }
    pcm->samples[pcm->count++] = clampf(s);
  }
  return MORSE_OK;
}

morse_status_t morse_synth_render(const list_t *elements,
                                  const morse_synth_opts_t *opts,
                                  morse_pcm_t *pcm) {
  morse_synth_opts_t defaults;
  list_node_t *it;
  double phase = 0.0;

  if (!elements || !pcm) {
    return MORSE_ERR_NULL;
  }
  if (!opts) {
    morse_synth_opts_default(&defaults);
    opts = &defaults;
  }
  if (opts->sample_rate == 0) {
    return MORSE_ERR_RANGE;
  }

  morse_pcm_free(pcm);
  morse_pcm_init(pcm);
  pcm->sample_rate = opts->sample_rate;

  for (it = elements->head; it != elements->nil; it = it->next) {
    const morse_element_t *e = (const morse_element_t *)it;
    morse_status_t st =
        render_symbol(pcm, opts, e->duration_ms, e->symbol, &phase);
    if (st != MORSE_OK) {
      return st;
    }
  }
  return MORSE_OK;
}

morse_status_t morse_synth_append_symbol(morse_pcm_t *pcm,
                                         const morse_synth_opts_t *opts,
                                         const morse_durations_t *durations,
                                         morse_symbol_t symbol, double *phase) {
  morse_synth_opts_t defaults;
  double dur;

  if (!pcm || !durations || !phase) {
    return MORSE_ERR_NULL;
  }
  if (!opts) {
    morse_synth_opts_default(&defaults);
    opts = &defaults;
  }
  if (opts->sample_rate == 0) {
    return MORSE_ERR_RANGE;
  }
  if (pcm->sample_rate == 0) {
    pcm->sample_rate = opts->sample_rate;
  }

  dur = morse_duration_of(durations, symbol);
  return render_symbol(pcm, opts, dur, symbol, phase);
}
