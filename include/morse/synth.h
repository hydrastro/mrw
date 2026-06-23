/*
 * morse/synth.h - Turning a stream of elements into audio samples.
 *
 * Marks are rendered as a sine tone gated by a raised-cosine (Hann) envelope.
 * The envelope ramp matters a great deal: hard on/off switching of a sinusoid
 * produces wideband "key clicks". A few milliseconds of cosine rise/fall keeps
 * the keyed signal clean, exactly as a real CW transmitter shapes its envelope.
 *
 * Output is mono 32-bit float in [-1, 1]. The WAV writer (morse/wav.h) can
 * persist it; the GUI feeds it straight to miniaudio.
 */
#ifndef MORSE_SYNTH_H
#define MORSE_SYNTH_H

#include "morse/timing.h"
#include "morse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct morse_synth_opts {
  unsigned int sample_rate; /* Hz, e.g. 44100                                 */
  double tone_hz;           /* sidetone frequency, e.g. 600                   */
  double amplitude;         /* peak amplitude in [0,1], e.g. 0.7              */
  double ramp_ms;           /* envelope rise/fall time, e.g. 5                */
  int add_noise;            /* if nonzero, mix white noise (for decoder tests)*/
  double noise_amplitude;   /* peak noise amplitude when add_noise set        */
} morse_synth_opts_t;

void morse_synth_opts_default(morse_synth_opts_t *opts);

/* A growable mono float buffer. */
typedef struct morse_pcm {
  float *samples;
  size_t count;             /* number of valid samples                        */
  size_t capacity;
  unsigned int sample_rate;
} morse_pcm_t;

void morse_pcm_init(morse_pcm_t *pcm);
void morse_pcm_free(morse_pcm_t *pcm);
double morse_pcm_duration_sec(const morse_pcm_t *pcm);

/* Render a ds list of morse_element_t into freshly allocated PCM.
 * `pcm` must be init'd (or zeroed); existing contents are freed first. */
morse_status_t morse_synth_render(const list_t *elements,
                                  const morse_synth_opts_t *opts,
                                  morse_pcm_t *pcm);

/* Convenience: render a single symbol's worth of audio appended to `pcm`,
 * advancing an internal phase carried in *phase (radians) so successive marks
 * stay phase-continuous. Used by the streaming/live synthesiser. */
morse_status_t morse_synth_append_symbol(morse_pcm_t *pcm,
                                         const morse_synth_opts_t *opts,
                                         const morse_durations_t *durations,
                                         morse_symbol_t symbol, double *phase);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_SYNTH_H */
