/*
 * morse/detect.c - Recover Morse text from raw audio.
 *
 *   PCM --[Goertzel @ tone]--> per-block tone power
 *       --[normalize + threshold with hysteresis]--> mark/space runs
 *       --[ds queue staging]--> morse_stream_decoder --> text
 *
 * Goertzel is a single-bin DFT evaluated by a second-order recurrence; for one
 * frequency it costs one multiply-add per sample, far less than an FFT.
 *
 * The offline path (morse_detect_pcm) makes two passes so it can normalize
 * against the true peak power of the whole clip - robust for files. The live
 * path (morse_detector) cannot see the future, so it tracks a decaying peak
 * online, and uses a ds circular buffer to regroup the arbitrarily-sized
 * sample chunks delivered by an audio callback into fixed analysis blocks.
 */
#include "morse/detect.h"
#include "morse/fft.h"
#include "morse_alloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- options ---------------------------------------------------------- */

void morse_detect_opts_default(morse_detect_opts_t *opts) {
  if (!opts) {
    return;
  }
  opts->tone_hz = 0.0;          /* auto-detect                               */
  opts->block_size = 256u;      /* ~5.8 ms at 44.1 kHz                        */
  opts->threshold = 0.0;        /* adaptive (half of peak)                   */
  opts->hysteresis = 0.6;       /* lower trip at 0.6x the upper trip         */
  opts->noise_floor_decay = 0.999; /* peak decay per block for the live path */
  opts->tone_min_hz = 100.0;    /* CW tones are commonly 300-1000, but allow */
  opts->tone_max_hz = 4000.0;  /* wide: any common CW pitch (and then some) */
  opts->track_tone = 1;         /* follow pitch changes on the live path     */
  opts->squelch_snr = 4.5;      /* reject marks weaker than ~6.5 dB over noise */
}

/* ---- envelope --------------------------------------------------------- */

void morse_envelope_init(morse_envelope_t *env) {
  if (!env) {
    return;
  }
  env->power = NULL;
  env->count = 0;
  env->block_size = 0;
  env->sample_rate = 0;
  env->tone_hz = 0.0;
}

void morse_envelope_free(morse_envelope_t *env) {
  if (!env) {
    return;
  }
  morse_xfree(env->power);
  morse_envelope_init(env);
}

/* ---- Goertzel --------------------------------------------------------- */

/* Squared magnitude of the tone bin over a block, given the precomputed
 * coefficient 2*cos(2*pi*f/fs). */
static double goertzel_block(const float *x, size_t n, double coeff) {
  double s_prev = 0.0, s_prev2 = 0.0;
  size_t i;
  for (i = 0; i < n; ++i) {
    double s = (double)x[i] + coeff * s_prev - s_prev2;
    s_prev2 = s_prev;
    s_prev = s;
  }
  /* power = |X_k|^2, normalized by block length so it is comparable across n */
  {
    double power = s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2;
    return power / ((double)n * (double)n);
  }
}

static double coeff_for(double tone_hz, unsigned int sample_rate) {
  return 2.0 * cos(2.0 * M_PI * tone_hz / (double)sample_rate);
}

/* Pick the strongest tone in a (wide) band using an averaged FFT spectrum with
 * sub-bin interpolation. Robust to arbitrary pitch; falls back to 600 Hz only
 * if the signal is effectively silent. */
static double autopick_tone(const morse_pcm_t *pcm, double fmin, double fmax) {
  double strength = 0.0;
  double f = morse_dominant_tone(pcm->samples, pcm->count, pcm->sample_rate,
                                 fmin, fmax, 4096, &strength);
  if (f <= 0.0 || strength <= 0.0) {
    return 600.0;
  }
  return f;
}

/* ---- run staging via ds queue ----------------------------------------- */

typedef struct run_event {
  ds_queue_node_t node; /* intrusive: must be first */
  int is_mark;
  double duration_ms;
} run_event_t;

static void run_event_free(ds_queue_node_t *n) { morse_xfree(n); }

static morse_status_t stage_run(ds_queue_t *q, int is_mark, double dur_ms) {
  run_event_t *ev = (run_event_t *)morse_xmalloc(sizeof(run_event_t));
  if (!ev) {
    return MORSE_ERR_ALLOC;
  }
  ev->is_mark = is_mark;
  ev->duration_ms = dur_ms;
  ds_queue_enqueue(q, &ev->node);
  return MORSE_OK;
}

/* Drain every staged run into the stream decoder. */
static void drain_runs(ds_queue_t *q, morse_stream_decoder_t *dec) {
  while (!ds_queue_is_empty(q)) {
    ds_queue_node_t *n = ds_queue_dequeue(q);
    run_event_t *ev = (run_event_t *)n;
    morse_stream_decoder_push(dec, ev->is_mark, ev->duration_ms);
    morse_xfree(ev);
  }
}

/* ---- offline detection (two-pass) ------------------------------------- */

/* Sink used by the offline decoder: append each fragment onto a ds_str. */
static void sink_to_dsstr(const char *utf8, void *user) {
  ds_str_t *s = (ds_str_t *)user;
  if (s && utf8) {
    ds_str_append_cstr(s, utf8);
  }
}

morse_status_t morse_detect_pcm(const morse_pcm_t *pcm,
                                const morse_table_t *table,
                                const morse_detect_opts_t *opts,
                                ds_str_t *out_text, morse_envelope_t *out_env) {
  morse_detect_opts_t defaults;
  unsigned int block;
  double tone, coeff, block_ms, peak;
  size_t nblocks, bi, off;
  double *power = NULL;
  double thr_hi, thr_lo;
  int state; /* current mark/space state */
  size_t run_blocks;
  ds_queue_t *q = NULL;
  morse_stream_decoder_t *dec = NULL;
  morse_status_t st = MORSE_OK;

  if (!pcm || !table || !out_text) {
    return MORSE_ERR_NULL;
  }
  if (!opts) {
    morse_detect_opts_default(&defaults);
    opts = &defaults;
  }
  if (pcm->sample_rate == 0 || pcm->count == 0) {
    return MORSE_ERR_RANGE;
  }

  block = opts->block_size ? opts->block_size : 256u;
  block_ms = 1000.0 * (double)block / (double)pcm->sample_rate;
  {
    double fmin = opts->tone_min_hz > 0.0 ? opts->tone_min_hz : 100.0;
    double fmax = opts->tone_max_hz > 0.0 ? opts->tone_max_hz : 3000.0;
    tone = opts->tone_hz > 0.0 ? opts->tone_hz : autopick_tone(pcm, fmin, fmax);
  }
  coeff = coeff_for(tone, pcm->sample_rate);
  nblocks = pcm->count / block;
  if (nblocks == 0) {
    return MORSE_ERR_RANGE;
  }

  /* pass 1: tone power per block + peak */
  power = (double *)morse_xmalloc(nblocks * sizeof(double));
  if (!power) {
    return MORSE_ERR_ALLOC;
  }
  peak = 0.0;
  for (bi = 0, off = 0; bi < nblocks; ++bi, off += block) {
    double p = goertzel_block(pcm->samples + off, block, coeff);
    power[bi] = p;
    if (p > peak) {
      peak = p;
    }
  }
  if (peak <= 0.0) {
    peak = 1e-12; /* silence: avoid div-by-zero, everything reads as space */
  }

  /* optional envelope output (normalized) */
  if (out_env) {
    morse_envelope_free(out_env);
    morse_envelope_init(out_env);
    out_env->power = (float *)morse_xmalloc(nblocks * sizeof(float));
    if (!out_env->power) {
      morse_xfree(power);
      return MORSE_ERR_ALLOC;
    }
    out_env->count = nblocks;
    out_env->block_size = block;
    out_env->sample_rate = pcm->sample_rate;
    out_env->tone_hz = tone;
    for (bi = 0; bi < nblocks; ++bi) {
      out_env->power[bi] = (float)(power[bi] / peak);
    }
  }

  /* pass 2: threshold with hysteresis -> runs -> decoder */
  thr_hi = opts->threshold > 0.0 ? opts->threshold : 0.5;
  thr_lo = thr_hi * (opts->hysteresis > 0.0 ? opts->hysteresis : 0.6);

  q = ds_queue_create_alloc(morse_xmalloc, morse_xfree);
  dec = morse_stream_decoder_create(table, /* initial dit guess; adapts */ 60.0,
                                    sink_to_dsstr, out_text);
  if (!q || !dec) {
    st = MORSE_ERR_ALLOC;
    goto done;
  }

  state = 0;       /* start in space */
  run_blocks = 0;  /* leading silence accumulates here, harmlessly */
  for (bi = 0; bi < nblocks; ++bi) {
    double norm = power[bi] / peak;
    int next = state;
    if (state == 0 && norm > thr_hi) {
      next = 1;
    } else if (state == 1 && norm < thr_lo) {
      next = 0;
    }
    if (next != state) {
      /* close the current run */
      st = stage_run(q, state, (double)run_blocks * block_ms);
      if (st != MORSE_OK) {
        goto done;
      }
      drain_runs(q, dec);
      state = next;
      run_blocks = 1;
    } else {
      run_blocks++;
    }
  }
  /* final run + flush */
  if (run_blocks > 0) {
    st = stage_run(q, state, (double)run_blocks * block_ms);
    if (st != MORSE_OK) {
      goto done;
    }
    drain_runs(q, dec);
  }
  morse_stream_decoder_finish(dec);

done:
  if (dec) {
    morse_stream_decoder_destroy(dec);
  }
  if (q) {
    ds_queue_destroy(q, run_event_free);
  }
  morse_xfree(power);
  return st;
}

/* ---- live streaming detector ------------------------------------------ */

typedef struct sample_node {
  ds_circular_buffer_node_t node; /* intrusive: must be first */
  float value;
} sample_node_t;

struct morse_detector {
  const morse_table_t *table;
  unsigned int sample_rate;
  unsigned int block;
  double tone_hz;   /* current analysis frequency (may drift with tracking) */
  double coeff;
  double block_ms;
  double thr_hi, thr_lo, decay;
  double peak;
  double last_norm; /* most recent normalized block power, for the meter */

  /* SNR squelch: a mark is accepted only when the tone bin stands well above
   * both its tracked noise floor and the neighbouring off-tone bins. This is
   * what stops noise and broadband audio from decoding into stray dits/dahs. */
  double coeff_lo, coeff_hi; /* Goertzel coeffs for the off-tone reference bins */
  double nfloor;             /* tracked in-bin noise-floor power */
  double snr_on, snr_off;    /* squelch open / close thresholds (linear power) */
  int on_pending;            /* consecutive above-threshold blocks (debounce) */

  /* wideband pitch tracking */
  int track;
  int locked;       /* have we adopted a confident tone estimate yet */
  double tone_min, tone_max;
  float *history;       /* rolling recent samples for re-estimation         */
  size_t hist_cap;      /* power of two                                     */
  size_t hist_len;      /* valid samples in history                         */
  size_t hist_head;     /* next write index (ring)                          */
  size_t blocks_seen;   /* counts blocks to schedule re-estimation          */

  ds_circular_buffer_t *window; /* regroups input into fixed blocks      */
  sample_node_t *pool;          /* backing storage for window nodes      */
  size_t pool_idx;
  ds_queue_t *runs;             /* staged run events                     */
  morse_stream_decoder_t *dec;

  int state;        /* current mark/space */
  size_t run_blocks;
  int started;      /* have we seen the first mark yet (suppresses lead) */
  int silence_emitted; /* delivered the current silence as a gap already   */
};

morse_detector_t *morse_detector_create(const morse_table_t *table,
                                        unsigned int sample_rate,
                                        const morse_detect_opts_t *opts,
                                        morse_decode_sink_fn sink, void *user) {
  morse_detect_opts_t defaults;
  morse_detector_t *det;
  double tone;

  if (!table || !sink || sample_rate == 0) {
    return NULL;
  }
  if (!opts) {
    morse_detect_opts_default(&defaults);
    opts = &defaults;
  }
  det = (morse_detector_t *)morse_xcalloc(1, sizeof(*det));
  if (!det) {
    return NULL;
  }
  det->table = table;
  det->sample_rate = sample_rate;
  det->block = opts->block_size ? opts->block_size : 256u;
  tone = opts->tone_hz > 0.0 ? opts->tone_hz : 600.0; /* refined by tracking */
  det->tone_hz = tone;
  det->coeff = coeff_for(tone, sample_rate);
  det->block_ms = 1000.0 * (double)det->block / (double)sample_rate;
  det->thr_hi = opts->threshold > 0.0 ? opts->threshold : 0.5;
  det->thr_lo = det->thr_hi * (opts->hysteresis > 0.0 ? opts->hysteresis : 0.6);
  det->decay = opts->noise_floor_decay > 0.0 ? opts->noise_floor_decay : 0.999;
  det->peak = 1e-9;

  /* SNR squelch setup: reference bins sit ~200 Hz either side of the tone,
   * just outside the Goertzel main lobe, to gauge the local broadband level. */
  {
    double nyq = (double)sample_rate * 0.5;
    double flo = tone - 200.0, fhi = tone + 200.0;
    if (flo < 50.0) {
      flo = 50.0;
    }
    if (fhi > nyq * 0.9) {
      fhi = nyq * 0.9;
    }
    det->coeff_lo = coeff_for(flo, sample_rate);
    det->coeff_hi = coeff_for(fhi, sample_rate);
  }
  det->nfloor = 1e-9;
  det->snr_on = opts->squelch_snr > 0.0 ? opts->squelch_snr
                : (opts->squelch_snr < 0.0 ? 0.0 : 4.5);
  det->snr_off = det->snr_on * 0.5;
  det->on_pending = 0;

  /* If no fixed tone was given, enable wideband tracking by default. */
  det->track = opts->tone_hz > 0.0 ? 0 : (opts->track_tone ? 1 : 0);
  det->locked = 0;
  det->tone_min = opts->tone_min_hz > 0.0 ? opts->tone_min_hz : 100.0;
  det->tone_max = opts->tone_max_hz > 0.0 ? opts->tone_max_hz : 3000.0;
  det->hist_cap = 8192u; /* ~186 ms at 44.1 kHz; power of two for the FFT */
  det->hist_len = 0;
  det->hist_head = 0;
  det->blocks_seen = 0;
  det->history = NULL;
  if (det->track) {
    det->history = (float *)morse_xmalloc(det->hist_cap * sizeof(float));
  }

  det->window =
      ds_circular_buffer_create_alloc(det->block, false, morse_xmalloc, morse_xfree);
  det->pool = (sample_node_t *)morse_xmalloc(det->block * sizeof(sample_node_t));
  det->runs = ds_queue_create_alloc(morse_xmalloc, morse_xfree);
  det->dec = morse_stream_decoder_create(table, 60.0, sink, user);
  if (!det->window || !det->pool || !det->runs || !det->dec ||
      (det->track && !det->history)) {
    morse_detector_destroy(det);
    return NULL;
  }
  det->pool_idx = 0;
  det->state = 0;
  det->run_blocks = 0;
  det->started = 0;
  return det;
}

void morse_detector_destroy(morse_detector_t *det) {
  if (!det) {
    return;
  }
  /* Stage the final, still-open run (the clip/stream may end on a mark or in
   * silence right after the last mark) so the trailing character is emitted,
   * then push one word gap to force the decoder to flush its letter buffer. */
  if (det->dec && det->runs && det->started && det->run_blocks > 0) {
    stage_run(det->runs, det->state,
              (double)det->run_blocks * det->block_ms);
    if (det->state == 1) {
      /* ended on a mark: follow it with a word-sized gap to close the char */
      double dit = morse_stream_decoder_dit_ms(det->dec);
      if (dit <= 0.0) {
        dit = 60.0;
      }
      stage_run(det->runs, 0, 8.0 * dit);
    }
    drain_runs(det->runs, det->dec);
  }
  if (det->dec) {
    morse_stream_decoder_finish(det->dec);
    morse_stream_decoder_destroy(det->dec);
  }
  if (det->runs) {
    ds_queue_destroy(det->runs, run_event_free);
  }
  if (det->window) {
    /* nodes are pool-backed, not individually owned: no per-node free */
    ds_circular_buffer_destroy(det->window, NULL);
  }
  morse_xfree(det->history);
  morse_xfree(det->pool);
  morse_xfree(det);
}

/* Periodically re-estimate the tone from recent history and ease the Goertzel
 * frequency toward it, so the detector follows pitch changes and drift. */
static void detector_retrack(morse_detector_t *det) {
  float *lin;
  size_t i, n, min_hist, cadence_blocks;
  double strength, f;

  if (!det->track || !det->history) {
    return;
  }

  /* Before the first confident lock, try to estimate as early and often as
   * possible (every block, from ~23 ms of audio) so the detector latches onto
   * the tone quickly. After locking, re-estimate about every 90 ms and ease in
   * changes to follow drift without jitter. */
  if (!det->locked) {
    min_hist = 1024;
    cadence_blocks = 1;
  } else {
    min_hist = det->hist_cap / 2;
    cadence_blocks = (size_t)(0.090 / (det->block_ms / 1000.0) + 1.0);
  }
  if (det->hist_len < min_hist) {
    return;
  }
  if (cadence_blocks > 1 && (det->blocks_seen % cadence_blocks) != 0) {
    return;
  }

  n = det->hist_len;
  lin = (float *)morse_xmalloc(n * sizeof(float));
  if (!lin) {
    return;
  }
  if (det->hist_len < det->hist_cap) {
    for (i = 0; i < n; ++i) {
      lin[i] = det->history[i];
    }
  } else {
    size_t idx = det->hist_head;
    for (i = 0; i < n; ++i) {
      lin[i] = det->history[idx];
      idx = (idx + 1) % det->hist_cap;
    }
  }

  f = morse_dominant_tone(lin, n, det->sample_rate, det->tone_min, det->tone_max,
                          4096, &strength);
  morse_xfree(lin);

  if (f > 0.0 && strength > 0.30) {
    if (!det->locked) {
      /* adopt the first confident estimate outright */
      det->tone_hz = f;
      det->locked = 1;
    } else {
      /* ease toward subsequent estimates */
      det->tone_hz = det->tone_hz + 0.5 * (f - det->tone_hz);
    }
    det->coeff = coeff_for(det->tone_hz, det->sample_rate);
    {
      double nyq = (double)det->sample_rate * 0.5;
      double flo = det->tone_hz - 200.0, fhi = det->tone_hz + 200.0;
      if (flo < 50.0) {
        flo = 50.0;
      }
      if (fhi > nyq * 0.9) {
        fhi = nyq * 0.9;
      }
      det->coeff_lo = coeff_for(flo, det->sample_rate);
      det->coeff_hi = coeff_for(fhi, det->sample_rate);
    }
  }
}

/* Process one full analysis block (already copied into `blk`). */
static void detector_consume_block(morse_detector_t *det, const float *blk) {
  double p;
  double norm;
  int next;

  det->blocks_seen++;
  detector_retrack(det);

  p = goertzel_block(blk, det->block, det->coeff);

  /* decaying peak tracker (kept for the live level meter) */
  det->peak = p > det->peak ? p : det->peak * det->decay;
  if (det->peak < 1e-12) {
    det->peak = 1e-12;
  }
  norm = p / det->peak;
  det->last_norm = norm;

  /* While tracking but not yet locked onto the tone, hold off the mark/space
   * state machine: blocks analysed at the wrong frequency would emit spurious
   * transitions. We still warm up the peak tracker above so thresholds are
   * sensible the moment we lock. */
  if (det->track && !det->locked) {
    return;
  }

  /* ---- SNR squelch ---------------------------------------------------- *
   * Decide mark vs space from how far the tone bin rises above the local
   * noise, estimated two ways: a slowly-tracked in-bin noise floor (steady
   * hiss) and the louder of the two off-tone reference bins (broadband bursts,
   * static, speech and music all raise the neighbours too). A real CW tone
   * towers over both; noise does not. This replaces normalising by a decaying
   * peak, which would eventually read plain noise as a string of dits.        */
  if (det->snr_on > 0.0) {
    double rlo = goertzel_block(blk, det->block, det->coeff_lo);
    double rhi = goertzel_block(blk, det->block, det->coeff_hi);
    double off = rlo > rhi ? rlo : rhi; /* the louder neighbour */
    double ref, snr;

    /* track the in-bin noise floor: fall fast toward quiet, rise slowly */
    if (p < det->nfloor) {
      det->nfloor = 0.7 * det->nfloor + 0.3 * p;
    } else {
      det->nfloor = 0.9995 * det->nfloor + 0.0005 * p;
    }
    if (det->nfloor < 1e-12) {
      det->nfloor = 1e-12;
    }
    ref = off > det->nfloor ? off : det->nfloor; /* most conservative floor */
    snr = p / (ref + 1e-12);

    next = det->state;
    if (det->state == 0) {
      if (snr > det->snr_on) {
        if (++det->on_pending >= 2) { /* debounce single-block spikes */
          next = 1;
        }
      } else {
        det->on_pending = 0;
      }
    } else { /* state == 1 (mark) */
      det->on_pending = 0;
      if (snr < det->snr_off) {
        next = 0;
      }
    }
  } else {
    /* squelch disabled: fall back to the peak-relative threshold */
    next = det->state;
    if (det->state == 0 && norm > det->thr_hi) {
      next = 1;
    } else if (det->state == 1 && norm < det->thr_lo) {
      next = 0;
    }
  }

  if (next != det->state) {
    /* Suppress the very first (leading-silence) run so it does not become a
     * spurious word gap before any character. */
    if (det->started || det->state == 1) {
      stage_run(det->runs, det->state, (double)det->run_blocks * det->block_ms);
      drain_runs(det->runs, det->dec);
    }
    det->started = 1;
    det->state = next;
    det->run_blocks = 1;
    det->silence_emitted = 0;
  } else {
    det->run_blocks++;
    /* If we are sitting in silence well past a word gap, deliver it now so the
     * trailing character appears without waiting for the next tone (or for the
     * detector to be torn down). Harmless to re-stage on the eventual flip: the
     * decoder ignores gap runs once its letter buffer is already empty. */
    if (det->state == 0 && det->started && !det->silence_emitted) {
      double dit = morse_stream_decoder_dit_ms(det->dec);
      double dur = (double)det->run_blocks * det->block_ms;
      if (dit > 0.0 && dur > 6.0 * dit) {
        stage_run(det->runs, 0, dur);
        drain_runs(det->runs, det->dec);
        det->silence_emitted = 1;
      }
    }
  }
}

morse_status_t morse_detector_process(morse_detector_t *det,
                                      const float *samples, size_t count,
                                      float *out_power) {
  size_t i;
  if (!det || (!samples && count > 0)) {
    return MORSE_ERR_NULL;
  }

  for (i = 0; i < count; ++i) {
    sample_node_t *sn = &det->pool[det->pool_idx];
    sn->value = samples[i];
    ds_circular_buffer_push(det->window, &sn->node);
    det->pool_idx++;

    /* keep a rolling copy of recent audio for wideband pitch tracking */
    if (det->track && det->history) {
      det->history[det->hist_head] = samples[i];
      det->hist_head = (det->hist_head + 1) % det->hist_cap;
      if (det->hist_len < det->hist_cap) {
        det->hist_len++;
      }
    }

    if (det->pool_idx == det->block) {
      /* a full block is buffered: copy it out in order, then reset */
      float *blk = (float *)morse_xmalloc(det->block * sizeof(float));
      size_t k;
      if (!blk) {
        return MORSE_ERR_ALLOC;
      }
      for (k = 0; k < det->block; ++k) {
        ds_circular_buffer_node_t *n = ds_circular_buffer_pop(det->window);
        blk[k] = ((sample_node_t *)n)->value;
      }
      detector_consume_block(det, blk);
      morse_xfree(blk);
      det->pool_idx = 0;
    }
  }

  if (out_power) {
    *out_power = (float)det->last_norm;
  }
  return MORSE_OK;
}

double morse_detector_wpm(const morse_detector_t *det) {
  if (det == NULL || det->dec == NULL) {
    return 0.0;
  }
  return morse_stream_decoder_wpm(det->dec);
}

double morse_detector_tone_hz(const morse_detector_t *det) {
  return det != NULL ? det->tone_hz : 0.0;
}
