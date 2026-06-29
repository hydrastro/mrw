/* filter.c - RBJ-cookbook biquad cascade (high-pass + low-pass = band-pass). */
#include "morse/filter.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void morse_filter_init(morse_filter_t *f, unsigned int sample_rate) {
  if (f == NULL) {
    return;
  }
  memset(f, 0, sizeof(*f));
  f->rate = sample_rate;
  f->n = 0;
}

void morse_filter_reset(morse_filter_t *f) {
  int i;
  if (f == NULL) {
    return;
  }
  for (i = 0; i < f->n; ++i) {
    f->section[i].x1 = f->section[i].x2 = 0.0;
    f->section[i].y1 = f->section[i].y2 = 0.0;
  }
}

/* Butterworth pole Q for section k of an order-`order` filter. */
static double butter_q(int order, int k) {
  return 1.0 / (2.0 * cos(M_PI * (2.0 * k + 1.0) / (2.0 * order)));
}

static void set_lowpass(morse_biquad_t *s, double fc, double q,
                        unsigned int rate) {
  double w0 = 2.0 * M_PI * fc / (double)rate;
  double c = cos(w0), sn = sin(w0);
  double alpha = sn / (2.0 * q);
  double a0 = 1.0 + alpha;
  s->b0 = ((1.0 - c) / 2.0) / a0;
  s->b1 = (1.0 - c) / a0;
  s->b2 = ((1.0 - c) / 2.0) / a0;
  s->a1 = (-2.0 * c) / a0;
  s->a2 = (1.0 - alpha) / a0;
  s->x1 = s->x2 = s->y1 = s->y2 = 0.0;
}

static void set_highpass(morse_biquad_t *s, double fc, double q,
                         unsigned int rate) {
  double w0 = 2.0 * M_PI * fc / (double)rate;
  double c = cos(w0), sn = sin(w0);
  double alpha = sn / (2.0 * q);
  double a0 = 1.0 + alpha;
  s->b0 = ((1.0 + c) / 2.0) / a0;
  s->b1 = (-(1.0 + c)) / a0;
  s->b2 = ((1.0 + c) / 2.0) / a0;
  s->a1 = (-2.0 * c) / a0;
  s->a2 = (1.0 - alpha) / a0;
  s->x1 = s->x2 = s->y1 = s->y2 = 0.0;
}

int morse_filter_bandpass(morse_filter_t *f, double f_lo, double f_hi,
                          int order) {
  int sections_per_edge, i, n = 0;
  double nyq;
  if (f == NULL || f->rate == 0) {
    return -1;
  }
  nyq = (double)f->rate * 0.5;
  if (order < 2) {
    order = 2;
  }
  if (order > 8) {
    order = 8;
  }
  order &= ~1; /* force even */
  sections_per_edge = order / 2;

  /* clamp cutoffs into a sane range */
  if (f_lo < 0.0) {
    f_lo = 0.0;
  }
  if (f_hi > nyq * 0.98) {
    f_hi = nyq * 0.98;
  }
  if (f_hi > 0.0 && f_lo >= f_hi) {
    f_lo = 0.0; /* invalid band: fall back to low-pass only */
  }

  /* high-pass sections at f_lo */
  if (f_lo > 1.0) {
    for (i = 0; i < sections_per_edge && n < MORSE_FILTER_MAX_SECTIONS; ++i) {
      set_highpass(&f->section[n++], f_lo, butter_q(order, i), f->rate);
    }
  }
  /* low-pass sections at f_hi */
  if (f_hi > 1.0) {
    for (i = 0; i < sections_per_edge && n < MORSE_FILTER_MAX_SECTIONS; ++i) {
      set_lowpass(&f->section[n++], f_hi, butter_q(order, i), f->rate);
    }
  }
  f->n = n;
  return 0;
}

double morse_filter_tick(morse_filter_t *f, double x) {
  int i;
  for (i = 0; i < f->n; ++i) {
    morse_biquad_t *s = &f->section[i];
    double y = s->b0 * x + s->b1 * s->x1 + s->b2 * s->x2 - s->a1 * s->y1 -
               s->a2 * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    x = y;
  }
  return x;
}

void morse_filter_process(morse_filter_t *f, const float *in, float *out,
                          size_t n) {
  size_t i;
  if (f == NULL || in == NULL || out == NULL) {
    return;
  }
  if (f->n == 0) {
    if (in != out) {
      memmove(out, in, n * sizeof(float));
    }
    return;
  }
  for (i = 0; i < n; ++i) {
    out[i] = (float)morse_filter_tick(f, (double)in[i]);
  }
}
