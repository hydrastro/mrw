/*
 * morse/wav.c - Minimal, dependency-free RIFF/WAVE reader and writer.
 *
 * The writer emits the lowest-common-denominator CW recording format: mono,
 * 16-bit signed PCM, little-endian. The reader is deliberately liberal: it
 * accepts 8/16/24/32-bit signed PCM and 32-bit IEEE float, in any channel
 * count, WAVE_FORMAT_EXTENSIBLE wrappers included, and downmixes everything to
 * a single mono float track for the detector. Between the two, "render a tone
 * to disk" and "decode this recording" are both covered without pulling in a
 * codec library; anything more exotic (mp3/ogg/flac) is first transcoded to
 * WAV by the GUI's ffmpeg bridge and then handed here.
 *
 * All multi-byte fields in RIFF are little-endian on disk. We read and write
 * them byte-by-byte so the code is correct regardless of host endianness.
 */
#include "morse/wav.h"
#include "morse_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- little-endian byte helpers ------------------------------------------ */

static void put_u16le(unsigned char *p, unsigned int v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_u32le(unsigned char *p, unsigned long v) {
  p[0] = (unsigned char)(v & 0xFFul);
  p[1] = (unsigned char)((v >> 8) & 0xFFul);
  p[2] = (unsigned char)((v >> 16) & 0xFFul);
  p[3] = (unsigned char)((v >> 24) & 0xFFul);
}

static unsigned int get_u16le(const unsigned char *p) {
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned long get_u32le(const unsigned char *p) {
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
         ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* ---- writer -------------------------------------------------------------- */

morse_status_t morse_wav_write(const char *path, const morse_pcm_t *pcm) {
  FILE *f;
  unsigned char hdr[44];
  size_t i;
  unsigned long data_bytes;
  unsigned long byte_rate;
  unsigned int sr;

  if (path == NULL || pcm == NULL) {
    return MORSE_ERR_NULL;
  }
  if (pcm->count != 0 && pcm->samples == NULL) {
    return MORSE_ERR_NULL;
  }

  sr = pcm->sample_rate ? pcm->sample_rate : 44100u;
  data_bytes = (unsigned long)pcm->count * 2ul; /* mono, 2 bytes/sample */
  byte_rate = (unsigned long)sr * 2ul;          /* sr * channels * bytes */

  /* RIFF header */
  memcpy(hdr + 0, "RIFF", 4);
  put_u32le(hdr + 4, 36ul + data_bytes); /* file size - 8 */
  memcpy(hdr + 8, "WAVE", 4);
  /* fmt chunk */
  memcpy(hdr + 12, "fmt ", 4);
  put_u32le(hdr + 16, 16ul); /* PCM fmt chunk size */
  put_u16le(hdr + 20, 1u);   /* audio format: PCM */
  put_u16le(hdr + 22, 1u);   /* channels: mono */
  put_u32le(hdr + 24, (unsigned long)sr);
  put_u32le(hdr + 28, byte_rate);
  put_u16le(hdr + 32, 2u);  /* block align: channels * bytes/sample */
  put_u16le(hdr + 34, 16u); /* bits per sample */
  /* data chunk */
  memcpy(hdr + 36, "data", 4);
  put_u32le(hdr + 40, data_bytes);

  f = fopen(path, "wb");
  if (f == NULL) {
    return MORSE_ERR_IO;
  }
  if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) {
    fclose(f);
    return MORSE_ERR_IO;
  }

  for (i = 0; i < pcm->count; i++) {
    float s = pcm->samples[i];
    long v;
    unsigned char out[2];
    if (s > 1.0f) {
      s = 1.0f;
    } else if (s < -1.0f) {
      s = -1.0f;
    }
    /* Symmetric scaling into 16-bit signed range. */
    v = (long)(s * 32767.0f + (s >= 0.0f ? 0.5f : -0.5f));
    if (v > 32767l) {
      v = 32767l;
    } else if (v < -32768l) {
      v = -32768l;
    }
    put_u16le(out, (unsigned int)(v & 0xFFFFl));
    if (fwrite(out, 1, 2, f) != 2) {
      fclose(f);
      return MORSE_ERR_IO;
    }
  }

  fclose(f);
  return MORSE_OK;
}

/* ---- reader -------------------------------------------------------------- */

/* Decode one interleaved frame's worth of bytes at `p` for channel `ch`,
 * returning a float in roughly [-1, 1]. `fmt` is 1 (int PCM) or 3 (float). */
static float sample_to_float(const unsigned char *p, unsigned int fmt,
                             unsigned int bits, unsigned int ch) {
  const unsigned char *s = p + (size_t)ch * (bits / 8u);

  if (fmt == 3u && bits == 32u) {
    /* IEEE 754 little-endian float. Assemble then type-pun via memcpy. */
    unsigned long u = get_u32le(s);
    float out;
    memcpy(&out, &u, sizeof out);
    return out;
  }

  if (bits == 8u) {
    /* 8-bit PCM is unsigned, centred on 128. */
    return ((float)s[0] - 128.0f) / 128.0f;
  }
  if (bits == 16u) {
    int v = (int)get_u16le(s);
    if (v >= 32768) {
      v -= 65536;
    }
    return (float)v / 32768.0f;
  }
  if (bits == 24u) {
    long v = (long)s[0] | ((long)s[1] << 8) | ((long)s[2] << 16);
    if (v >= 0x800000l) {
      v -= 0x1000000l;
    }
    return (float)v / 8388608.0f;
  }
  if (bits == 32u) {
    /* 32-bit signed integer PCM. */
    unsigned long u = get_u32le(s);
    long v = (long)u;
    return (float)((double)v / 2147483648.0);
  }
  return 0.0f;
}

morse_status_t morse_wav_read(const char *path, morse_pcm_t *pcm) {
  FILE *f;
  unsigned char riff[12];
  unsigned char chunk[8];
  unsigned int fmt_tag = 0, channels = 0, bits = 0;
  unsigned long sample_rate = 0;
  int have_fmt = 0;
  unsigned char *data = NULL;
  unsigned long data_len = 0;
  size_t frames, fr, ch;
  unsigned int frame_bytes;

  if (path == NULL || pcm == NULL) {
    return MORSE_ERR_NULL;
  }

  f = fopen(path, "rb");
  if (f == NULL) {
    return MORSE_ERR_IO;
  }

  if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0 ||
      memcmp(riff + 8, "WAVE", 4) != 0) {
    fclose(f);
    return MORSE_ERR_FORMAT;
  }

  /* Walk chunks until we have both fmt and data. */
  while (fread(chunk, 1, 8, f) == 8) {
    unsigned long csz = get_u32le(chunk + 4);

    if (memcmp(chunk, "fmt ", 4) == 0) {
      unsigned char fb[40];
      unsigned long want = csz < sizeof fb ? csz : sizeof fb;
      if (fread(fb, 1, want, f) != want) {
        morse_xfree(data);
        fclose(f);
        return MORSE_ERR_FORMAT;
      }
      fmt_tag = get_u16le(fb + 0);
      channels = get_u16le(fb + 2);
      sample_rate = get_u32le(fb + 4);
      bits = get_u16le(fb + 14);
      /* WAVE_FORMAT_EXTENSIBLE: real format lives in the subformat GUID. */
      if (fmt_tag == 0xFFFEu && csz >= 26ul) {
        fmt_tag = get_u16le(fb + 24);
      }
      have_fmt = 1;
      /* Skip any extra fmt bytes we didn't read. */
      if (csz > want) {
        fseek(f, (long)(csz - want), SEEK_CUR);
      }
    } else if (memcmp(chunk, "data", 4) == 0) {
      morse_xfree(data); /* in the unlikely event of a second data chunk */
      data = (unsigned char *)morse_xmalloc(csz ? csz : 1);
      if (data == NULL) {
        fclose(f);
        return MORSE_ERR_ALLOC;
      }
      if (fread(data, 1, csz, f) != csz) {
        morse_xfree(data);
        fclose(f);
        return MORSE_ERR_FORMAT;
      }
      data_len = csz;
    } else {
      /* Unknown chunk: skip it. */
      fseek(f, (long)csz, SEEK_CUR);
    }
    /* RIFF chunks are word-aligned: skip a pad byte after odd sizes. */
    if (csz & 1ul) {
      fseek(f, 1, SEEK_CUR);
    }
  }
  fclose(f);

  if (!have_fmt || data == NULL) {
    morse_xfree(data);
    return MORSE_ERR_FORMAT;
  }
  if (channels == 0 || bits == 0 || (bits % 8u) != 0u) {
    morse_xfree(data);
    return MORSE_ERR_FORMAT;
  }
  if (fmt_tag != 1u && fmt_tag != 3u) {
    morse_xfree(data); /* a-law/mu-law/adpcm etc.: out of scope, transcode first */
    return MORSE_ERR_UNSUPPORTED;
  }
  if (fmt_tag == 3u && bits != 32u) {
    morse_xfree(data);
    return MORSE_ERR_UNSUPPORTED;
  }

  frame_bytes = channels * (bits / 8u);
  frames = frame_bytes ? (size_t)(data_len / frame_bytes) : 0;

  morse_pcm_free(pcm);
  morse_pcm_init(pcm);
  pcm->sample_rate = (unsigned int)sample_rate;
  if (frames == 0) {
    morse_xfree(data);
    return MORSE_OK; /* empty but valid */
  }

  pcm->samples = (float *)morse_xmalloc(frames * sizeof(float));
  if (pcm->samples == NULL) {
    morse_xfree(data);
    return MORSE_ERR_ALLOC;
  }
  pcm->capacity = frames;
  pcm->count = frames;

  for (fr = 0; fr < frames; fr++) {
    const unsigned char *p = data + (size_t)fr * frame_bytes;
    float acc = 0.0f;
    for (ch = 0; ch < channels; ch++) {
      acc += sample_to_float(p, fmt_tag, bits, (unsigned int)ch);
    }
    pcm->samples[fr] = acc / (float)channels;
  }

  morse_xfree(data);
  return MORSE_OK;
}
