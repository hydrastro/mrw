/*
 * morse/wav.h - Minimal RIFF/WAVE read & write, no external dependencies.
 *
 * Writes mono 16-bit PCM (the universally compatible CW recording format) from
 * our float buffer, and reads back PCM WAV (8/16/24/32-bit int or 32-bit
 * float, any channel count, downmixed to mono) into a float buffer for the
 * detector. Anything ffmpeg can read can be turned into such a WAV by the GUI's
 * ffmpeg bridge, so this is enough to cover "decode this mp3" too.
 */
#ifndef MORSE_WAV_H
#define MORSE_WAV_H

#include "morse/synth.h"
#include "morse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Write `pcm` to `path` as mono 16-bit PCM WAV. */
morse_status_t morse_wav_write(const char *path, const morse_pcm_t *pcm);

/* Read a PCM WAV from `path` into `pcm` (init'd or zeroed; freed first),
 * downmixing to mono float and recording the sample rate. */
morse_status_t morse_wav_read(const char *path, morse_pcm_t *pcm);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_WAV_H */
