/*
 * morse/morse.h - Umbrella header for the morse-deluxe core library.
 *
 * Include this and you get the whole public surface: types, codebook, timing,
 * encode, decode, synth, detect and WAV I/O, plus a couple of top-level
 * convenience entry points and the optional ds-allocator diagnostics hook.
 */
#ifndef MORSE_MORSE_H
#define MORSE_MORSE_H

#include "morse/types.h"
#include "morse/table.h"
#include "morse/timing.h"
#include "morse/encode.h"
#include "morse/decode.h"
#include "morse/synth.h"
#include "morse/detect.h"
#include "morse/fft.h"
#include "morse/filter.h"
#include "morse/multi.h"
#include "morse/wav.h"
#include "morse/cw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MORSE_VERSION_MAJOR 1
#define MORSE_VERSION_MINOR 0
#define MORSE_VERSION_PATCH 0

const char *morse_version_string(void);

/*
 * One-call convenience: encode UTF-8 text straight to a 16-bit PCM WAV file
 * using default rendering. Handy from scripts and the CLI.
 */
morse_status_t morse_text_to_wav(const char *text, morse_variant_t variant,
                                 const morse_timing_t *timing,
                                 const morse_synth_opts_t *synth,
                                 const char *path);

/*
 * One-call convenience: decode a (PCM) WAV file straight to text written into a
 * caller-owned ds_str.
 */
morse_status_t morse_wav_to_text(const char *path, morse_variant_t variant,
                                 const morse_detect_opts_t *detect,
                                 ds_str_t *out_text);

/* ---- optional allocation diagnostics ---------------------------------- */

/*
 * The core normally uses ds's default context (malloc/free). For the GUI's
 * "diagnostics" panel we expose a process-wide debug allocator that wraps the
 * default context and tallies allocations. Enable once at startup; query any
 * time. This is purely observational and thread-unsafe, intended for the
 * single-threaded GUI/CLI use here.
 */
void morse_diagnostics_enable(void);
typedef struct morse_alloc_stats {
  size_t allocations;
  size_t frees;
  size_t reallocations;
  size_t failed_allocations;
  size_t bytes_live;
  size_t bytes_peak;
  size_t bytes_total;
} morse_alloc_stats_t;
int morse_diagnostics_get(morse_alloc_stats_t *out); /* 1 if enabled, else 0 */

#ifdef __cplusplus
}
#endif

#endif /* MORSE_MORSE_H */
