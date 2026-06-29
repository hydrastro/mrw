/*
 * morse/detect.h - Recovering Morse from raw audio.
 *
 * The pipeline is the textbook one for narrowband CW reception:
 *
 *   PCM --[Goertzel @ tone_hz]--> tone-power envelope
 *       --[adaptive threshold + hysteresis]--> mark/space runs
 *       --[morse_stream_decoder]--> text
 *
 * The Goertzel algorithm is a single-bin DFT: far cheaper than a full FFT when
 * you only care about power at one frequency, which is exactly the CW case.
 *
 * Internally a ds circular buffer holds the current analysis block and a ds
 * queue stages detected run events, demonstrating those structures on a real
 * signal-processing task.
 */
#ifndef MORSE_DETECT_H
#define MORSE_DETECT_H

#include "morse/decode.h"
#include "morse/synth.h"
#include "morse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct morse_detect_opts {
  double tone_hz;          /* expected tone; 0 => auto-detect from the signal  */
  unsigned int block_size; /* Goertzel block length in samples (e.g. 256)     */
  double threshold;        /* 0 => adaptive; else fixed normalized 0..1       */
  double hysteresis;       /* fraction of threshold for the lower trip (0..1) */
  double noise_floor_decay;/* EMA factor for the adaptive floor (0..1)        */
  double tone_min_hz;      /* low edge of the auto-detect band (0 => 100 Hz)  */
  double tone_max_hz;      /* high edge of the auto-detect band (0 => 3000 Hz)*/
  int track_tone;          /* live path: re-estimate pitch and follow drift   */
  double squelch_snr;      /* live path: in-band SNR (linear) needed to accept */
                           /* a mark, measured against the noise floor and the */
                           /* off-tone bins. 0 => default (~4.5); <0 disables. */
                           /* This is what rejects noise decoded as E/T soup.  */
} morse_detect_opts_t;

void morse_detect_opts_default(morse_detect_opts_t *opts);

/* One envelope sample produced per Goertzel block, for plotting. */
typedef struct morse_envelope {
  float *power;            /* normalized tone power per block, 0..1           */
  size_t count;
  unsigned int block_size;
  unsigned int sample_rate;
  double tone_hz;          /* the frequency actually analysed                 */
} morse_envelope_t;

void morse_envelope_init(morse_envelope_t *env);
void morse_envelope_free(morse_envelope_t *env);

/*
 * Analyse `pcm`, decoding into ds_str `out_text` (caller-owned, appended to).
 * If `out_env` is non-NULL it is filled with the per-block tone-power envelope
 * (caller frees with morse_envelope_free). Suitable for offline file decode.
 */
morse_status_t morse_detect_pcm(const morse_pcm_t *pcm,
                                const morse_table_t *table,
                                const morse_detect_opts_t *opts,
                                ds_str_t *out_text, morse_envelope_t *out_env);

/* ---- streaming front-end (for live microphone input) ------------------ */

typedef struct morse_detector morse_detector_t; /* opaque                    */

morse_detector_t *morse_detector_create(const morse_table_t *table,
                                        unsigned int sample_rate,
                                        const morse_detect_opts_t *opts,
                                        morse_decode_sink_fn sink, void *user);
void morse_detector_destroy(morse_detector_t *det);

/* Feed interleaved-free mono float samples; decoded text arrives via the sink.
 * If `out_power` is non-NULL, it receives the latest normalized tone power so a
 * live meter can be drawn. */
morse_status_t morse_detector_process(morse_detector_t *det,
                                      const float *samples, size_t count,
                                      float *out_power);

/* Current speed estimate (WPM) of the detector's adaptive decoder. */
double morse_detector_wpm(const morse_detector_t *det);

/* The frequency (Hz) the detector is currently analysing (follows drift when
 * tone tracking is enabled). */
double morse_detector_tone_hz(const morse_detector_t *det);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_DETECT_H */
