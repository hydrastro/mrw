/*
 * morse/decode.c - Morse to text: one-shot string decode and an adaptive,
 * self-clocking streaming decoder.
 *
 * The string decoder is deterministic: it splits a ".-" pattern on explicit
 * separators and resolves each letter through the table's trie.
 *
 * The stream decoder is the interesting one. It is handed a sequence of timed
 * mark/space runs (from a keyer, or from the Goertzel detector downstream) and
 * never told the operator's speed. It maintains a running estimate of the dit
 * length and classifies every run relative to that estimate:
 *
 *     mark  < 2 dits  -> dit          gap < 2 dits  -> intra-character (ignore)
 *     mark >= 2 dits  -> dah          gap < 5 dits  -> letter break (resolve)
 *                                     gap >= 5 dits -> word break (space)
 *
 * The estimate is nudged toward each freshly seen element (a dit contributes
 * its own length, a dah one third of it) with an exponential moving average,
 * so the decoder tracks gradual speed drift.
 */
#include "morse/decode.h"
#include "morse_alloc.h"

#include <stdlib.h>
#include <string.h>

/* ---- shared helpers --------------------------------------------------- */

/* Encode a Unicode scalar value as UTF-8 into out (always NUL terminated).
 * Returns the number of bytes written (excluding the NUL). */
static int cp_to_utf8(unsigned int cp, char out[5]) {
  if (cp < 0x80u) {
    out[0] = (char)cp;
    out[1] = '\0';
    return 1;
  }
  if (cp < 0x800u) {
    out[0] = (char)(0xC0u | (cp >> 6));
    out[1] = (char)(0x80u | (cp & 0x3Fu));
    out[2] = '\0';
    return 2;
  }
  if (cp < 0x10000u) {
    out[0] = (char)(0xE0u | (cp >> 12));
    out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (char)(0x80u | (cp & 0x3Fu));
    out[3] = '\0';
    return 3;
  }
  out[0] = (char)(0xF0u | (cp >> 18));
  out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
  out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
  out[3] = (char)(0x80u | (cp & 0x3Fu));
  out[4] = '\0';
  return 4;
}

static int char_in(const char *set, char c) {
  if (!set) {
    return 0;
  }
  for (; *set; ++set) {
    if (*set == c) {
      return 1;
    }
  }
  return 0;
}

/* ---- one-shot string decode ------------------------------------------- */

void morse_decode_opts_default(morse_decode_opts_t *opts) {
  if (!opts) {
    return;
  }
  opts->dit = '.';
  opts->dah = '-';
  opts->letter_sep = " ";
  opts->word_sep = "/";
  opts->unknown_replacement = '#';
}

/* Resolve the accumulated token and append its character (or the replacement)
 * to `out`. A word break queued in *word_pending is flushed as a single space
 * before the character, but never as a leading space. Returns MORSE_OK or the
 * first hard error encountered (allocation). */
static morse_status_t flush_token(const morse_table_t *table, char *tok,
                                  size_t *toklen, int *word_pending,
                                  char replacement, ds_str_t *out) {
  const morse_entry_t *entry = NULL;
  char utf8[5];

  if (*toklen == 0) {
    return MORSE_OK; /* nothing buffered; separators collapse harmlessly */
  }
  tok[*toklen] = '\0';

  if (*word_pending) {
    /* Only emit the space if we have already produced some text. */
    if (FUNC_str_len(out) > 0u) {
      if (ds_str_append_cstr(out, " ") != 0) {
        return MORSE_ERR_ALLOC;
      }
    }
    *word_pending = 0;
  }

  if (morse_table_decode_pattern(table, tok, &entry) == MORSE_OK && entry) {
    cp_to_utf8(entry->codepoint, utf8);
    if (ds_str_append_cstr(out, utf8) != 0) {
      return MORSE_ERR_ALLOC;
    }
  } else {
    char rep[2];
    rep[0] = replacement ? replacement : '#';
    rep[1] = '\0';
    if (ds_str_append_cstr(out, rep) != 0) {
      return MORSE_ERR_ALLOC;
    }
  }
  *toklen = 0;
  return MORSE_OK;
}

morse_status_t morse_decode_string(const morse_table_t *table,
                                   const char *pattern,
                                   const morse_decode_opts_t *opts,
                                   ds_str_t *out) {
  morse_decode_opts_t defaults;
  char tok[MORSE_MAX_PATTERN];
  size_t toklen = 0;
  int word_pending = 0;
  int last_was_space = 0;
  const char *p;
  morse_status_t st;

  if (!table || !pattern || !out) {
    return MORSE_ERR_NULL;
  }
  if (!opts) {
    morse_decode_opts_default(&defaults);
    opts = &defaults;
  }

  for (p = pattern; *p; ++p) {
    char c = *p;
    if (c == opts->dit || c == opts->dah) {
      char glyph = (c == opts->dit) ? '.' : '-';
      if (toklen < (size_t)(MORSE_MAX_PATTERN - 1)) {
        tok[toklen++] = glyph;
      } /* otherwise let it overflow into an unresolvable (too long) token */
      last_was_space = 0;
      continue;
    }

    /* Separator of some kind: close the current letter first. */
    st = flush_token(table, tok, &toklen, &word_pending,
                     opts->unknown_replacement, out);
    if (st != MORSE_OK) {
      return st;
    }

    if (char_in(opts->word_sep, c)) {
      word_pending = 1;
      last_was_space = 0;
    } else if (c == ' ') {
      if (last_was_space) {
        word_pending = 1; /* a second consecutive space is a word break */
      }
      last_was_space = 1;
    } else {
      /* tabs, newlines, other punctuation in the stream act as letter gaps */
      last_was_space = 0;
    }
  }

  /* Flush trailing letter, if any. */
  return flush_token(table, tok, &toklen, &word_pending,
                     opts->unknown_replacement, out);
}

/* ---- adaptive streaming decode ---------------------------------------- */

struct morse_stream_decoder {
  const morse_table_t *table;
  morse_decode_sink_fn sink;
  void *user;

  double dit_ms;  /* current adaptive estimate of one dit            */
  char tok[MORSE_MAX_PATTERN];
  size_t toklen;
  int in_word;    /* have we emitted any character in the current word? */
  int pending_word_gap; /* a word gap is owed once the next letter lands  */

  int marks_seen;     /* how many marks processed (drives warmup)        */
  double min_mark_ms; /* shortest mark observed during warmup            */
};

#define DIT_EMA_ALPHA 0.15
#define DIT_EMA_ALPHA_WARMUP 0.45 /* faster lock-on for the first marks   */
#define DIT_WARMUP_MARKS 6
#define DIT_MS_MIN 5.0
#define DIT_MS_MAX 2000.0

static double clampd(double v, double lo, double hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

morse_stream_decoder_t *morse_stream_decoder_create(const morse_table_t *table,
                                                    double initial_dit_ms,
                                                    morse_decode_sink_fn sink,
                                                    void *user) {
  morse_stream_decoder_t *dec;
  if (!table || !sink) {
    return NULL;
  }
  dec = (morse_stream_decoder_t *)morse_xcalloc(1, sizeof(*dec));
  if (!dec) {
    return NULL;
  }
  dec->table = table;
  dec->sink = sink;
  dec->user = user;
  dec->dit_ms = clampd(initial_dit_ms > 0.0 ? initial_dit_ms : 60.0, DIT_MS_MIN,
                       DIT_MS_MAX);
  dec->toklen = 0;
  dec->in_word = 0;
  dec->pending_word_gap = 0;
  dec->marks_seen = 0;
  dec->min_mark_ms = 0.0;
  return dec;
}

void morse_stream_decoder_destroy(morse_stream_decoder_t *dec) {
  morse_xfree(dec);
}

/* Resolve the buffered token and deliver it (or the unknown marker) to the
 * sink, honouring a queued word gap. */
static void stream_emit_letter(morse_stream_decoder_t *dec) {
  const morse_entry_t *entry = NULL;
  char utf8[5];

  if (dec->toklen == 0) {
    return;
  }
  dec->tok[dec->toklen] = '\0';

  if (dec->pending_word_gap) {
    if (dec->in_word) {
      dec->sink(" ", dec->user);
    }
    dec->pending_word_gap = 0;
  }

  if (morse_table_decode_pattern(dec->table, dec->tok, &entry) == MORSE_OK &&
      entry) {
    cp_to_utf8(entry->codepoint, utf8);
    dec->sink(utf8, dec->user);
  } else {
    dec->sink("#", dec->user);
  }
  dec->in_word = 1;
  dec->toklen = 0;
}

morse_status_t morse_stream_decoder_push(morse_stream_decoder_t *dec,
                                         int is_mark, double duration_ms) {
  if (!dec) {
    return MORSE_ERR_NULL;
  }
  if (duration_ms <= 0.0) {
    return MORSE_OK; /* ignore degenerate runs */
  }

  if (is_mark) {
    /* Classify and accumulate. Boundary between dit and dah is 2 dits. */
    int warmup = dec->marks_seen < DIT_WARMUP_MARKS;
    double alpha = warmup ? DIT_EMA_ALPHA_WARMUP : DIT_EMA_ALPHA;
    double units = duration_ms / dec->dit_ms;
    double contribution;

    dec->marks_seen++;
    if (dec->min_mark_ms <= 0.0 || duration_ms < dec->min_mark_ms) {
      dec->min_mark_ms = duration_ms;
    }

    if (units < 2.0) {
      if (dec->toklen < (size_t)(MORSE_MAX_PATTERN - 1)) {
        dec->tok[dec->toklen++] = '.';
      }
      contribution = duration_ms; /* a dit is one unit */
    } else {
      if (dec->toklen < (size_t)(MORSE_MAX_PATTERN - 1)) {
        dec->tok[dec->toklen++] = '-';
      }
      contribution = duration_ms / 3.0; /* a dah is ~3 units */
    }

    dec->dit_ms = clampd(dec->dit_ms * (1.0 - alpha) + contribution * alpha,
                         DIT_MS_MIN, DIT_MS_MAX);

    /* While locking on, the shortest mark yet seen is an upper bound on the
     * true dit length (a dit is never longer than any mark). So if our estimate
     * sits above that bound it must be too high - pull it down. We never pull
     * it *up* toward min_mark, because the first marks may all be dahs, which
     * would otherwise inflate the estimate and merge letters together. */
    if (warmup && dec->min_mark_ms > 0.0 && dec->dit_ms > dec->min_mark_ms) {
      dec->dit_ms = clampd(0.5 * dec->dit_ms + 0.5 * dec->min_mark_ms,
                           DIT_MS_MIN, DIT_MS_MAX);
    }
    return MORSE_OK;
  }

  /* Silence: decide what kind of gap this is. */
  {
    double units = duration_ms / dec->dit_ms;
    if (units < 2.0) {
      /* intra-character gap: the letter continues, nothing to do */
    } else if (units < 5.0) {
      /* letter gap: resolve the buffered token */
      stream_emit_letter(dec);
    } else {
      /* word gap: resolve any pending letter, then owe a space */
      stream_emit_letter(dec);
      dec->pending_word_gap = 1;
    }
  }
  return MORSE_OK;
}

morse_status_t morse_stream_decoder_finish(morse_stream_decoder_t *dec) {
  if (!dec) {
    return MORSE_ERR_NULL;
  }
  stream_emit_letter(dec);
  return MORSE_OK;
}

double morse_stream_decoder_dit_ms(const morse_stream_decoder_t *dec) {
  return dec ? dec->dit_ms : 0.0;
}

double morse_stream_decoder_wpm(const morse_stream_decoder_t *dec) {
  if (!dec || dec->dit_ms <= 0.0) {
    return 0.0;
  }
  /* PARIS: dit_ms = 1200 / wpm  =>  wpm = 1200 / dit_ms */
  return 1200.0 / dec->dit_ms;
}
