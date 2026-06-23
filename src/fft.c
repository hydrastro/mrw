/*
 * morse/fft.c - radix-2 FFT and spectral tone estimation.
 */
#include "morse/fft.h"
#include "morse_alloc.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int morse_is_pow2(size_t n) { return n != 0 && (n & (n - 1)) == 0; }

size_t morse_floor_pow2(size_t n) {
  size_t p = 1;
  if (n == 0) {
    return 0;
  }
  while ((p << 1) != 0 && (p << 1) <= n) {
    p <<= 1;
  }
  return p;
}

int morse_fft(double *re, double *im, size_t n, int inverse) {
  size_t i, j, len;
  double ang_sign;
  if (!re || !im || !morse_is_pow2(n)) {
    return 1;
  }
  if (n == 1) {
    return 0;
  }

  /* bit-reversal permutation */
  j = 0;
  for (i = 1; i < n; ++i) {
    size_t bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1) {
      j &= ~bit;
    }
    j |= bit;
    if (i < j) {
      double tr = re[i], ti = im[i];
      re[i] = re[j];
      im[i] = im[j];
      re[j] = tr;
      im[j] = ti;
    }
  }

  ang_sign = inverse ? 1.0 : -1.0;
  for (len = 2; len <= n; len <<= 1) {
    double ang = ang_sign * 2.0 * M_PI / (double)len;
    double wlen_r = cos(ang), wlen_i = sin(ang);
    for (i = 0; i < n; i += len) {
      double w_r = 1.0, w_i = 0.0;
      size_t k;
      for (k = 0; k < len / 2; ++k) {
        size_t a = i + k;
        size_t b = i + k + len / 2;
        double u_r = re[a], u_i = im[a];
        double v_r = re[b] * w_r - im[b] * w_i;
        double v_i = re[b] * w_i + im[b] * w_r;
        double nw_r;
        re[a] = u_r + v_r;
        im[a] = u_i + v_i;
        re[b] = u_r - v_r;
        im[b] = u_i - v_i;
        nw_r = w_r * wlen_r - w_i * wlen_i;
        w_i = w_r * wlen_i + w_i * wlen_r;
        w_r = nw_r;
      }
    }
  }

  if (inverse) {
    for (i = 0; i < n; ++i) {
      re[i] /= (double)n;
      im[i] /= (double)n;
    }
  }
  return 0;
}

int morse_real_spectrum(const float *x, size_t n, double *out_mag) {
  double *re = NULL, *im = NULL;
  size_t i;
  int rc;
  if (!x || !out_mag || !morse_is_pow2(n)) {
    return 1;
  }
  re = (double *)morse_xmalloc(n * sizeof(double));
  im = (double *)morse_xmalloc(n * sizeof(double));
  if (!re || !im) {
    morse_xfree(re);
    morse_xfree(im);
    return 2;
  }
  for (i = 0; i < n; ++i) {
    /* Hann window reduces spectral leakage so the peak is sharp. */
    double w = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (double)(n - 1)));
    re[i] = (double)x[i] * w;
    im[i] = 0.0;
  }
  rc = morse_fft(re, im, n, 0);
  if (rc == 0) {
    for (i = 0; i < n / 2; ++i) {
      out_mag[i] = sqrt(re[i] * re[i] + im[i] * im[i]);
    }
  }
  morse_xfree(re);
  morse_xfree(im);
  return rc;
}

double morse_dominant_tone(const float *x, size_t count,
                           unsigned int sample_rate, double fmin, double fmax,
                           size_t win, double *out_strength) {
  size_t n, hop, off, half, k, kmin, kmax, peak_k, nwin;
  double *acc = NULL, *mag = NULL;
  double bin_hz, mean, peak_val, denom, delta, freq;

  if (out_strength) {
    *out_strength = 0.0;
  }
  if (!x || count == 0 || sample_rate == 0) {
    return 0.0;
  }

  /* Choose an analysis window: a power of two, large enough for resolution but
   * not larger than the data. ~4096 gives ~10 Hz bins at 44.1 kHz. */
  if (win == 0) {
    win = 4096;
  }
  n = morse_floor_pow2(win);
  if (n > morse_floor_pow2(count)) {
    n = morse_floor_pow2(count);
  }
  if (n < 64) {
    return 0.0; /* too little signal to estimate reliably */
  }
  half = n / 2;
  bin_hz = (double)sample_rate / (double)n;

  acc = (double *)morse_xcalloc(half, sizeof(double));
  mag = (double *)morse_xmalloc(half * sizeof(double));
  if (!acc || !mag) {
    morse_xfree(acc);
    morse_xfree(mag);
    return 0.0;
  }

  /* Welch averaging: 50% overlapping windows across the whole clip. */
  hop = n / 2;
  nwin = 0;
  for (off = 0; off + n <= count; off += hop) {
    if (morse_real_spectrum(x + off, n, mag) == 0) {
      for (k = 0; k < half; ++k) {
        acc[k] += mag[k];
      }
      ++nwin;
    }
  }
  if (nwin == 0) {
    /* signal shorter than two hops: a single window */
    if (morse_real_spectrum(x, n, mag) == 0) {
      for (k = 0; k < half; ++k) {
        acc[k] = mag[k];
      }
      nwin = 1;
    } else {
      morse_xfree(acc);
      morse_xfree(mag);
      return 0.0;
    }
  }

  /* search band, clamped to the valid bin range (skip DC) */
  kmin = (size_t)(fmin / bin_hz);
  if (kmin < 1) {
    kmin = 1;
  }
  kmax = (size_t)(fmax / bin_hz);
  if (kmax >= half) {
    kmax = half - 1;
  }
  if (kmin >= kmax) {
    morse_xfree(acc);
    morse_xfree(mag);
    return 0.0;
  }

  mean = 0.0;
  for (k = kmin; k <= kmax; ++k) {
    mean += acc[k];
  }
  mean /= (double)(kmax - kmin + 1);

  peak_k = kmin;
  peak_val = acc[kmin];
  for (k = kmin + 1; k <= kmax; ++k) {
    if (acc[k] > peak_val) {
      peak_val = acc[k];
      peak_k = k;
    }
  }

  /* Parabolic interpolation around the peak bin for sub-bin accuracy. */
  delta = 0.0;
  if (peak_k > 0 && peak_k < half - 1) {
    double a = acc[peak_k - 1], b = acc[peak_k], c = acc[peak_k + 1];
    denom = (a - 2.0 * b + c);
    if (fabs(denom) > 1e-12) {
      delta = 0.5 * (a - c) / denom;
      if (delta > 0.5) {
        delta = 0.5;
      } else if (delta < -0.5) {
        delta = -0.5;
      }
    }
  }
  freq = ((double)peak_k + delta) * bin_hz;

  if (out_strength) {
    /* confidence: how far the peak stands above the band mean, squashed to 0..1 */
    double ratio = (mean > 1e-15) ? (peak_val / mean) : 0.0;
    double s = (ratio - 1.0) / 8.0; /* ratio of ~9x -> 1.0 */
    if (s < 0.0) {
      s = 0.0;
    } else if (s > 1.0) {
      s = 1.0;
    }
    *out_strength = s;
  }

  morse_xfree(acc);
  morse_xfree(mag);
  return freq;
}
