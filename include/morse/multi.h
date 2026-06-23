/*
 * morse/multi.h - Decode several simultaneous CW stations at once.
 *
 * Real on-air recordings (the Titanic distress traffic, a busy contest band)
 * carry many operators keying at the same time on different pitches and at
 * different speeds. A single-tone detector can only follow one of them.
 *
 * This module watches the whole audio band with a rolling FFT, finds the active
 * tone peaks, and dedicates an independent decoder to each one. Every station
 * gets its own frequency lock, its own adaptive threshold, and its own
 * speed-tracking streaming decoder, so they are transcribed in parallel.
 *
 * Internally each "channel" is just a fixed-tone morse_detector_t, so all the
 * tested Goertzel + threshold + decode machinery is reused unchanged; this
 * layer only does peak picking and channel bookkeeping.
 */
#ifndef MORSE_MULTI_H
#define MORSE_MULTI_H

#include "morse/decode.h"
#include "morse/synth.h"
#include "morse/table.h"
#include "morse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct morse_multi_opts {
  double tone_min_hz;     /* low edge of the watched band  (0 => 100 Hz)      */
  double tone_max_hz;     /* high edge of the watched band (0 => 3000 Hz)     */
  unsigned int win;       /* FFT analysis window in samples (0 => auto ~0.2s) */
  unsigned int hop;       /* samples between analyses       (0 => ~0.05s)     */
  int max_channels;       /* hard cap on simultaneous stations (0 => 12)      */
  double peak_ratio;      /* a peak must exceed this * band-mean (0 => 4.0)   */
  double merge_hz;        /* peaks closer than this are one station (0 => 60) */
  double active_seconds;  /* a channel counts as "active" if seen within this */
  double gate_seconds;    /* decode a channel only while peaked within this    */
                          /* window; suppresses cross-frequency leakage        */
                          /* (0 => a small default ~0.12s)                     */
  int min_hits;           /* analyses a new peak must persist before a channel  */
                          /* is created, rejecting transient ghosts (0 => 2)   */
} morse_multi_opts_t;

void morse_multi_opts_default(morse_multi_opts_t *opts);

/* Per-station snapshot. */
typedef struct morse_multi_channel_info {
  int id;          /* stable channel id                                    */
  double tone_hz;  /* locked centre frequency                              */
  double wpm;      /* current speed estimate                               */
  double snr;      /* most recent peak strength (peak / band-mean)         */
  int active;      /* non-zero if a peak was seen within active_seconds    */
} morse_multi_channel_info_t;

/* Called whenever a channel decodes text. `channel_id` and `tone_hz` identify
 * the station; `utf8` is the freshly decoded fragment. */
typedef void (*morse_multi_sink_fn)(int channel_id, double tone_hz,
                                    const char *utf8, void *user);

typedef struct morse_multi_detector morse_multi_detector_t; /* opaque */

morse_multi_detector_t *morse_multi_create(const morse_table_t *table,
                                           unsigned int sample_rate,
                                           const morse_multi_opts_t *opts,
                                           morse_multi_sink_fn sink, void *user);
void morse_multi_destroy(morse_multi_detector_t *d);

/* Feed mono float samples. Decoded text arrives through the sink, tagged with
 * the channel that produced it. Process the whole signal for offline use, or
 * stream it in chunks for live use. */
morse_status_t morse_multi_process(morse_multi_detector_t *d,
                                   const float *samples, size_t count);

/* Flush any character pending in each channel's decoder (call at end of input). */
void morse_multi_finish(morse_multi_detector_t *d);

/* Current channels (most recently active first). */
size_t morse_multi_channel_count(const morse_multi_detector_t *d);
int morse_multi_get_channel(const morse_multi_detector_t *d, size_t index,
                            morse_multi_channel_info_t *out_info);

/* The full transcript accumulated for a channel so far (NUL-terminated, owned
 * by the detector; valid until the next call that mutates it). NULL if the
 * index is out of range. */
const char *morse_multi_channel_text(const morse_multi_detector_t *d,
                                     size_t index);

/* ---- timeline -----------------------------------------------------------
 *
 * Every decoded fragment is also recorded, in order, with the time it was
 * produced, so a caller can show what was sent before what across all stations.
 */
typedef struct morse_multi_event {
  double t_seconds; /* time from the start of the stream                      */
  int channel_id;   /* which station produced it                              */
  double tone_hz;   /* that station's frequency                               */
  char text[8];     /* the decoded fragment (usually one character)          */
} morse_multi_event_t;

/* Number of recorded events (capped at an internal ring size; oldest dropped). */
size_t morse_multi_event_count(const morse_multi_detector_t *d);

/* Fetch event `index` (0 = oldest available). Returns 0 if out of range. */
int morse_multi_get_event(const morse_multi_detector_t *d, size_t index,
                          morse_multi_event_t *out_event);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_MULTI_H */
