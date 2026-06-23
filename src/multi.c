/* multi.c - Multi-station CW decoder: one fixed-tone detector per peak. */
#include "morse/multi.h"

#include "morse/detect.h"
#include "morse/fft.h"

#include "morse_alloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MORSE_MULTI_MAX 32
#define MORSE_MULTI_PENDING 32
#define MORSE_MULTI_EVENTS 4096

typedef struct {
  int used;
  int id;
  double tone_hz;
  double snr;
  long long last_active; /* sample index of the most recent matched peak */
  morse_detector_t *det;
  ds_str_t *text;
  struct morse_multi_detector *parent; /* for the per-channel sink trampoline */
} multi_channel_t;

/* A peak that has been seen but not yet promoted to a channel. */
typedef struct {
  int used;
  double tone_hz;
  int hits;
  long long last_seen;
} multi_pending_t;

struct morse_multi_detector {
  const morse_table_t *table;
  unsigned int rate;
  morse_multi_opts_t opts;
  morse_multi_sink_fn sink;
  void *user;

  multi_channel_t ch[MORSE_MULTI_MAX]; /* fixed so channel addresses are stable */
  multi_pending_t pending[MORSE_MULTI_PENDING];
  int next_id;

  float *abuf; /* rolling analysis window, capacity = win */
  unsigned int win;
  unsigned int abuf_len;
  unsigned int hop;
  long long total;
  long long last_analysis;
  long long gate_samples; /* how long a channel stays "open" after a peak */
  double *mag;            /* scratch, win/2 doubles */
  float *silence;         /* zero buffer, length hop, for gating closed channels */

  /* timeline */
  morse_multi_event_t events[MORSE_MULTI_EVENTS];
  size_t event_head; /* next write index */
  size_t event_n;    /* number stored (<= MORSE_MULTI_EVENTS) */
};

void morse_multi_opts_default(morse_multi_opts_t *o) {
  if (o == NULL) {
    return;
  }
  o->tone_min_hz = 100.0;
  o->tone_max_hz = 3000.0;
  o->win = 0;
  o->hop = 0;
  o->max_channels = 12;
  o->peak_ratio = 5.0;
  o->merge_hz = 100.0;
  o->active_seconds = 1.5;
  o->gate_seconds = 0.0; /* auto */
  o->min_hits = 2;
}

/* Per-channel decode sink: append to the channel transcript and forward to the
 * user sink, tagged with the channel id and frequency. `user` is the channel. */
static void channel_sink(const char *utf8, void *user) {
  multi_channel_t *c = (multi_channel_t *)user;
  morse_multi_detector_t *d;
  if (utf8 == NULL || c == NULL) {
    return;
  }
  d = c->parent;
  if (c->text != NULL) {
    ds_str_append_cstr(c->text, utf8);
  }
  /* record a timeline event (ring buffer) */
  if (d != NULL) {
    morse_multi_event_t *e = &d->events[d->event_head];
    e->t_seconds = d->rate ? (double)d->total / (double)d->rate : 0.0;
    e->channel_id = c->id;
    e->tone_hz = c->tone_hz;
    strncpy(e->text, utf8, sizeof(e->text) - 1);
    e->text[sizeof(e->text) - 1] = '\0';
    d->event_head = (d->event_head + 1) % MORSE_MULTI_EVENTS;
    if (d->event_n < MORSE_MULTI_EVENTS) {
      d->event_n++;
    }
    if (d->sink != NULL) {
      d->sink(c->id, c->tone_hz, utf8, d->user);
    }
  }
}

static int spawn_channel(morse_multi_detector_t *d, double freq, double snr) {
  int slot = -1;
  int i;
  for (i = 0; i < MORSE_MULTI_MAX && i < d->opts.max_channels; ++i) {
    if (!d->ch[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    /* Full: evict the longest-idle channel if it is past the active window. */
    long long oldest = d->total + 1;
    int victim = -1;
    for (i = 0; i < MORSE_MULTI_MAX && i < d->opts.max_channels; ++i) {
      if (d->ch[i].used && d->ch[i].last_active < oldest) {
        oldest = d->ch[i].last_active;
        victim = i;
      }
    }
    if (victim < 0) {
      return -1;
    }
    if ((double)(d->total - oldest) <= d->opts.active_seconds * d->rate) {
      return -1; /* everyone is still active; do not clobber a live station */
    }
    if (d->ch[victim].det != NULL) {
      morse_detector_destroy(d->ch[victim].det);
    }
    if (d->ch[victim].text != NULL) {
      ds_str_destroy(d->ch[victim].text);
    }
    memset(&d->ch[victim], 0, sizeof(d->ch[victim]));
    slot = victim;
  }

  {
    morse_detect_opts_t o;
    multi_channel_t *c = &d->ch[slot];
    morse_detect_opts_default(&o);
    o.tone_hz = freq;   /* lock this channel to the detected pitch */
    o.track_tone = 0;   /* keep stations from drifting into each other */
    o.block_size = 512; /* narrower Goertzel => less cross-frequency leakage */
    c->parent = d;
    c->det = morse_detector_create(d->table, d->rate, &o, channel_sink, c);
    if (c->det == NULL) {
      memset(c, 0, sizeof(*c));
      return -1;
    }
    c->text = ds_str_create();
    c->used = 1;
    c->id = d->next_id++;
    c->tone_hz = freq;
    c->snr = snr;
    c->last_active = d->total;
    /* Catch the channel up on the audio already in the analysis window so the
     * station's opening characters are not lost to detection latency. */
    if (d->abuf_len > 0) {
      morse_detector_process(c->det, d->abuf, d->abuf_len, NULL);
    }
  }
  return slot;
}

static void analyze(morse_multi_detector_t *d) {
  double binHz, mean;
  size_t kmin, kmax, k, half;
  int matched;
  if (d->abuf_len < d->win) {
    return;
  }
  if (morse_real_spectrum(d->abuf, d->win, d->mag) != 0) {
    return;
  }
  half = d->win / 2;
  binHz = (double)d->rate / (double)d->win;
  kmin = (size_t)(d->opts.tone_min_hz / binHz);
  if (kmin < 1) {
    kmin = 1;
  }
  kmax = (size_t)(d->opts.tone_max_hz / binHz);
  if (kmax > half - 2) {
    kmax = half - 2;
  }
  if (kmax <= kmin) {
    return;
  }

  mean = 0.0;
  for (k = kmin; k <= kmax; ++k) {
    mean += d->mag[k];
  }
  mean /= (double)(kmax - kmin + 1);
  if (mean <= 0.0) {
    return;
  }

  /* Collect local-maximum candidates above the ratio gate. */
  {
    double cand_f[64];
    double cand_s[64];
    int ncand = 0;
    double guard = d->opts.merge_hz; /* NMS guard band in Hz */
    int i, j;

    for (k = kmin + 1; k < kmax && ncand < 64; ++k) {
      double a, b, g, denom, p, freq, strength;
      b = d->mag[k];
      if (b < d->opts.peak_ratio * mean) {
        continue;
      }
      a = d->mag[k - 1];
      g = d->mag[k + 1];
      if (!(b >= a && b >= g)) {
        continue; /* not a local maximum */
      }
      denom = a - 2.0 * b + g;
      p = denom != 0.0 ? 0.5 * (a - g) / denom : 0.0;
      if (p > 0.5) {
        p = 0.5;
      } else if (p < -0.5) {
        p = -0.5;
      }
      freq = ((double)k + p) * binHz;
      strength = b / mean;
      cand_f[ncand] = freq;
      cand_s[ncand] = strength;
      ncand++;
    }

    /* Sort candidates strongest-first (small n, simple selection sort). */
    for (i = 0; i < ncand - 1; ++i) {
      for (j = i + 1; j < ncand; ++j) {
        if (cand_s[j] > cand_s[i]) {
          double tf = cand_f[i], ts = cand_s[i];
          cand_f[i] = cand_f[j];
          cand_s[i] = cand_s[j];
          cand_f[j] = tf;
          cand_s[j] = ts;
        }
      }
    }

    /* Non-maximum suppression: accept a peak only if it is not within the
     * guard band of an already-accepted (stronger) peak. This removes the
     * spectral-leakage skirts that surround a strong carrier and would
     * otherwise spawn phantom stations. */
    for (i = 0; i < ncand; ++i) {
      int suppressed = 0;
      for (j = 0; j < i; ++j) {
        if (cand_s[j] >= cand_s[i] &&
            fabs(cand_f[j] - cand_f[i]) < guard) {
          suppressed = 1;
          break;
        }
      }
      if (suppressed) {
        continue;
      }
      /* match to an existing channel, else require persistence before spawning */
      matched = 0;
      {
        int c;
        for (c = 0; c < MORSE_MULTI_MAX && c < d->opts.max_channels; ++c) {
          if (d->ch[c].used &&
              fabs(d->ch[c].tone_hz - cand_f[i]) <= d->opts.merge_hz) {
            d->ch[c].last_active = d->total;
            d->ch[c].snr = cand_s[i];
            d->ch[c].tone_hz = 0.9 * d->ch[c].tone_hz + 0.1 * cand_f[i];
            matched = 1;
            break;
          }
        }
      }
      if (!matched) {
        /* Find or create a pending candidate; spawn only once it has been seen
         * in enough consecutive analyses to be a real station, not a glitch. */
        int p, slot = -1, free_slot = -1;
        for (p = 0; p < MORSE_MULTI_PENDING; ++p) {
          if (d->pending[p].used &&
              fabs(d->pending[p].tone_hz - cand_f[i]) <= d->opts.merge_hz) {
            slot = p;
            break;
          }
          if (!d->pending[p].used && free_slot < 0) {
            free_slot = p;
          }
        }
        if (slot < 0) {
          slot = free_slot;
          if (slot >= 0) {
            d->pending[slot].used = 1;
            d->pending[slot].tone_hz = cand_f[i];
            d->pending[slot].hits = 0;
          }
        }
        if (slot >= 0) {
          d->pending[slot].hits++;
          d->pending[slot].last_seen = d->total;
          d->pending[slot].tone_hz =
              0.7 * d->pending[slot].tone_hz + 0.3 * cand_f[i];
          if (d->pending[slot].hits >= d->opts.min_hits) {
            if (spawn_channel(d, d->pending[slot].tone_hz, cand_s[i]) >= 0) {
              d->pending[slot].used = 0;
            }
          }
        }
      }
    }

    /* Age out pending candidates that stopped appearing. */
    {
      int p;
      for (p = 0; p < MORSE_MULTI_PENDING; ++p) {
        if (d->pending[p].used &&
            (double)(d->total - d->pending[p].last_seen) >
                2.0 * (double)d->hop) {
          d->pending[p].used = 0;
        }
      }
    }
  }
}

morse_multi_detector_t *morse_multi_create(const morse_table_t *table,
                                           unsigned int sample_rate,
                                           const morse_multi_opts_t *opts,
                                           morse_multi_sink_fn sink,
                                           void *user) {
  morse_multi_detector_t *d;
  if (table == NULL || sample_rate == 0) {
    return NULL;
  }
  d = (morse_multi_detector_t *)morse_xcalloc(1, sizeof(*d));
  if (d == NULL) {
    return NULL;
  }
  d->table = table;
  d->rate = sample_rate;
  morse_multi_opts_default(&d->opts);
  if (opts != NULL) {
    d->opts = *opts;
    if (d->opts.tone_min_hz <= 0.0) {
      d->opts.tone_min_hz = 100.0;
    }
    if (d->opts.tone_max_hz <= 0.0) {
      d->opts.tone_max_hz = 3000.0;
    }
    if (d->opts.max_channels <= 0 || d->opts.max_channels > MORSE_MULTI_MAX) {
      d->opts.max_channels = 12;
    }
    if (d->opts.peak_ratio <= 1.0) {
      d->opts.peak_ratio = 4.0;
    }
    if (d->opts.merge_hz <= 0.0) {
      d->opts.merge_hz = 60.0;
    }
    if (d->opts.active_seconds <= 0.0) {
      d->opts.active_seconds = 1.5;
    }
  }
  d->sink = sink;
  d->user = user;
  d->next_id = 1;

  /* A short analysis window: ~50 ms gives good time resolution for gating while
   * its FFT (≈20 Hz bins) still separates stations cleanly. */
  d->win = d->opts.win;
  if (d->win == 0) {
    d->win = (unsigned int)morse_floor_pow2((size_t)(sample_rate * 0.05));
  }
  if (d->win < 1024) {
    d->win = 1024;
  }
  if (d->win > 8192) {
    d->win = 8192;
  }
  d->win = (unsigned int)morse_floor_pow2(d->win);
  d->hop = d->opts.hop;
  if (d->hop == 0) {
    d->hop = d->win / 4; /* 75% overlap => responsive gating */
  }
  if (d->hop == 0) {
    d->hop = 256;
  }

  /* How long a channel keeps decoding after its last peak. Long enough to
   * bridge the gaps *within* a transmission (so an active station is never
   * chopped), but finite so a channel that has truly gone quiet stops decoding
   * before a strong neighbour can leak into it. */
  {
    double gs = d->opts.gate_seconds > 0.0 ? d->opts.gate_seconds : 0.8;
    d->gate_samples = (long long)(gs * sample_rate);
    if (d->gate_samples < (long long)d->hop * 2) {
      d->gate_samples = (long long)d->hop * 2;
    }
  }

  d->abuf = (float *)morse_xmalloc(sizeof(float) * d->win);
  d->mag = (double *)morse_xmalloc(sizeof(double) * (d->win / 2));
  d->silence = (float *)morse_xcalloc(d->hop, sizeof(float));
  if (d->abuf == NULL || d->mag == NULL || d->silence == NULL) {
    morse_xfree(d->abuf);
    morse_xfree(d->mag);
    morse_xfree(d->silence);
    morse_xfree(d);
    return NULL;
  }
  d->abuf_len = 0;
  d->total = 0;
  d->last_analysis = 0;
  return d;
}

void morse_multi_destroy(morse_multi_detector_t *d) {
  int i;
  if (d == NULL) {
    return;
  }
  for (i = 0; i < MORSE_MULTI_MAX; ++i) {
    if (d->ch[i].used) {
      if (d->ch[i].det != NULL) {
        morse_detector_destroy(d->ch[i].det);
      }
      if (d->ch[i].text != NULL) {
        ds_str_destroy(d->ch[i].text);
      }
    }
  }
  morse_xfree(d->abuf);
  morse_xfree(d->mag);
  morse_xfree(d->silence);
  morse_xfree(d);
}

morse_status_t morse_multi_process(morse_multi_detector_t *d,
                                   const float *samples, size_t count) {
  size_t i;
  if (d == NULL || (samples == NULL && count > 0)) {
    return MORSE_ERR_NULL;
  }
  i = 0;
  while (i < count) {
    unsigned int step = d->hop;
    int c;
    if ((size_t)step > count - i) {
      step = (unsigned int)(count - i);
    }

    /* roll the analysis window */
    if (step >= d->win) {
      memcpy(d->abuf, samples + i + (step - d->win), sizeof(float) * d->win);
      d->abuf_len = d->win;
    } else {
      if (d->abuf_len + step > d->win) {
        unsigned int drop = d->abuf_len + step - d->win;
        memmove(d->abuf, d->abuf + drop, sizeof(float) * (d->abuf_len - drop));
        d->abuf_len -= drop;
      }
      memcpy(d->abuf + d->abuf_len, samples + i, sizeof(float) * step);
      d->abuf_len += step;
    }

    /* Feed each live channel this chunk, but only while it is "open" - i.e. it
     * had a peak within the gate window. A channel that has gone quiet is fed
     * silence instead, so a strong station on another frequency cannot bleed
     * through its (necessarily wide) Goertzel filter and produce garbage. */
    for (c = 0; c < MORSE_MULTI_MAX && c < d->opts.max_channels; ++c) {
      if (d->ch[c].used && d->ch[c].det != NULL) {
        int open = (d->total - d->ch[c].last_active) <= d->gate_samples;
        if (open) {
          morse_detector_process(d->ch[c].det, samples + i, step, NULL);
        } else {
          morse_detector_process(d->ch[c].det, d->silence, step, NULL);
        }
      }
    }

    d->total += step;
    if (d->abuf_len >= d->win &&
        d->total - d->last_analysis >= (long long)d->hop) {
      analyze(d);
      d->last_analysis = d->total;
    }
    i += step;
  }
  return MORSE_OK;
}

void morse_multi_finish(morse_multi_detector_t *d) {
  /* Feed each channel a second of silence so trailing characters flush. */
  static const unsigned int kChunk = 1024;
  float silence[1024];
  unsigned int total, n;
  int c;
  if (d == NULL) {
    return;
  }
  memset(silence, 0, sizeof(silence));
  for (c = 0; c < MORSE_MULTI_MAX && c < d->opts.max_channels; ++c) {
    if (!d->ch[c].used || d->ch[c].det == NULL) {
      continue;
    }
    for (total = 0; total < d->rate; total += n) {
      n = d->rate - total < kChunk ? d->rate - total : kChunk;
      morse_detector_process(d->ch[c].det, silence, n, NULL);
    }
  }
}

/* Build an array of in-use channel indices ordered by most-recent activity. */
static int order_channels(const morse_multi_detector_t *d, int *order) {
  int n = 0, i, j;
  for (i = 0; i < MORSE_MULTI_MAX && i < d->opts.max_channels; ++i) {
    if (d->ch[i].used) {
      order[n++] = i;
    }
  }
  for (i = 0; i < n - 1; ++i) {
    for (j = i + 1; j < n; ++j) {
      if (d->ch[order[j]].last_active > d->ch[order[i]].last_active) {
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
      }
    }
  }
  return n;
}

size_t morse_multi_channel_count(const morse_multi_detector_t *d) {
  int order[MORSE_MULTI_MAX];
  if (d == NULL) {
    return 0;
  }
  return (size_t)order_channels(d, order);
}

int morse_multi_get_channel(const morse_multi_detector_t *d, size_t index,
                            morse_multi_channel_info_t *out) {
  int order[MORSE_MULTI_MAX];
  int n;
  const multi_channel_t *c;
  if (d == NULL || out == NULL) {
    return 0;
  }
  n = order_channels(d, order);
  if ((int)index >= n) {
    return 0;
  }
  c = &d->ch[order[index]];
  out->id = c->id;
  out->tone_hz = c->tone_hz;
  out->snr = c->snr;
  out->wpm = c->det != NULL ? morse_detector_wpm(c->det) : 0.0;
  out->active =
      (double)(d->total - c->last_active) <= d->opts.active_seconds * d->rate;
  return 1;
}

const char *morse_multi_channel_text(const morse_multi_detector_t *d,
                                     size_t index) {
  int order[MORSE_MULTI_MAX];
  int n;
  if (d == NULL) {
    return NULL;
  }
  n = order_channels(d, order);
  if ((int)index >= n) {
    return NULL;
  }
  return FUNC_str_cstr(d->ch[order[index]].text);
}

/* ---- timeline ------------------------------------------------------------ */

size_t morse_multi_event_count(const morse_multi_detector_t *d) {
  return d != NULL ? d->event_n : 0;
}

int morse_multi_get_event(const morse_multi_detector_t *d, size_t index,
                          morse_multi_event_t *out) {
  size_t start;
  if (d == NULL || out == NULL || index >= d->event_n) {
    return 0;
  }
  /* event_head points just past the newest; oldest is head - event_n. */
  start = (d->event_head + MORSE_MULTI_EVENTS - d->event_n) % MORSE_MULTI_EVENTS;
  *out = d->events[(start + index) % MORSE_MULTI_EVENTS];
  return 1;
}
