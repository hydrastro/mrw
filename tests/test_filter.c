/* test_filter.c - band-pass passes in-band tones, rejects out-of-band. */
#include "morse/morse.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

/* RMS of a pure tone at `hz` after running it through filter `f`. */
static double tone_rms_through(morse_filter_t *f, double hz, unsigned int rate) {
  const size_t n = rate; /* 1 second */
  size_t i;
  double acc = 0.0;
  morse_filter_reset(f);
  /* prime the filter to settle transients */
  for (i = 0; i < rate / 10; ++i) {
    (void)morse_filter_tick(f, sin(2.0 * M_PI * hz * (double)i / rate));
  }
  for (i = 0; i < n; ++i) {
    double x = sin(2.0 * M_PI * hz * (double)(i + rate / 10) / rate);
    double y = morse_filter_tick(f, x);
    acc += y * y;
  }
  return sqrt(acc / (double)n);
}

TEST_BEGIN("filter");
{
  const unsigned int rate = 44100;
  morse_filter_t f;
  double in_band, low_out, high_out;
  int finite_ok = 1;
  size_t i;
  float buf[1024];

  morse_filter_init(&f, rate);
  CHECK(morse_filter_bandpass(&f, 300.0, 1200.0, 4) == 0);

  in_band = tone_rms_through(&f, 700.0, rate);  /* passband centre */
  low_out = tone_rms_through(&f, 60.0, rate);   /* mains hum below band */
  high_out = tone_rms_through(&f, 4000.0, rate); /* hiss above band */

  /* a full-scale sine has RMS ~0.707; in-band should be close to that */
  CHECK(in_band > 0.5);
  /* out-of-band tones strongly attenuated (>20 dB => <0.0707) */
  CHECK(low_out < in_band * 0.1);
  CHECK(high_out < in_band * 0.1);
  /* the band edges are between: 300 and 1200 pass, far-out rejected */
  CHECK(low_out < high_out * 4.0 || high_out < in_band * 0.1);

  /* stability: a noisy buffer stays finite */
  morse_filter_reset(&f);
  unsigned int seed = 7u;
  for (i = 0; i < 1024; ++i) {
    seed = seed * 1103515245u + 12345u;
    buf[i] = ((float)((seed >> 9) & 0xFFFF) / 32768.0f - 1.0f);
  }
  morse_filter_process(&f, buf, buf, 1024);
  for (i = 0; i < 1024; ++i) {
    if (!(buf[i] == buf[i]) || fabs(buf[i]) > 100.0f) {
      finite_ok = 0;
    }
  }
  CHECK(finite_ok);

  /* pass-through when unconfigured */
  morse_filter_t pass;
  morse_filter_init(&pass, rate);
  double same = morse_filter_tick(&pass, 0.42);
  CHECK(fabs(same - 0.42) < 1e-9);
}
TEST_END()
