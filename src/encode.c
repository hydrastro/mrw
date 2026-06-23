/* encode.c - UTF-8 text -> Morse (string form and timed element list). */
#include "morse/encode.h"
#include "morse_alloc.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

/* Decode one UTF-8 scalar from s; returns bytes consumed (>=1), sets *cp.
 * Lenient: a malformed lead byte is returned verbatim as a 1-byte "codepoint".*/
static size_t utf8_next(const unsigned char *s, unsigned int *cp) {
  unsigned int c = s[0];
  if (c < 0x80u) {
    *cp = c;
    return 1;
  }
  if ((c & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
    *cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
    return 2;
  }
  if ((c & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u &&
      (s[2] & 0xC0u) == 0x80u) {
    *cp = ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    return 3;
  }
  if ((c & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u &&
      (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) {
    *cp = ((c & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) |
          ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
    return 4;
  }
  *cp = c;
  return 1;
}

/* Case-fold a code point to the upper-case form present in the codebook. */
static unsigned int cp_upper(unsigned int cp) {
  if (cp >= 'a' && cp <= 'z') {
    return cp - 32u;
  }
  /* Latin-1 lowercase accented letters -> uppercase (skip U+00F7 division). */
  if (cp >= 0x00E0u && cp <= 0x00FEu && cp != 0x00F7u) {
    return cp - 0x20u;
  }
  return cp;
}

static int is_space_cp(unsigned int cp) {
  return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' ||
         cp == '\v';
}

void morse_encode_opts_default(morse_encode_opts_t *opts) {
  if (opts == NULL) {
    return;
  }
  opts->unknown = MORSE_UNKNOWN_SKIP;
  opts->dit = '.';
  opts->dah = '-';
  opts->element_sep = "";
  opts->letter_sep = " ";
  opts->word_sep = " / ";
}

/* Append one ".-" pattern to ds_str applying glyph + element separators. */
static int append_pattern_string(ds_str_t *out, const char *pattern,
                                 const morse_encode_opts_t *o) {
  size_t i;
  for (i = 0; pattern[i] != '\0'; i++) {
    if (i > 0 && o->element_sep[0] != '\0') {
      if (ds_str_append_cstr(out, o->element_sep) != 0) {
        return 0;
      }
    }
    if (ds_str_pushc(out, pattern[i] == '-' ? o->dah : o->dit) != 0) {
      return 0;
    }
  }
  return 1;
}

/* ------------------------------------------------------------------------- */
/* prosign token scanning: <NAME>                                            */
/* ------------------------------------------------------------------------- */

/* If s points at a '<NAME>' prosign known to the table, copy NAME (NUL term)
 * into name_buf and return the byte length of the whole token; else 0. */
static size_t scan_prosign(const morse_table_t *table, const unsigned char *s,
                           char *name_buf, size_t name_cap) {
  size_t i = 1; /* skip '<' */
  size_t n = 0;
  const char *pat;
  if (s[0] != '<') {
    return 0;
  }
  while (s[i] != '\0' && s[i] != '>' && n + 1 < name_cap) {
    name_buf[n++] = (char)s[i++];
  }
  if (s[i] != '>') {
    return 0; /* unterminated */
  }
  name_buf[n] = '\0';
  if (morse_table_lookup_prosign(table, name_buf, &pat) != MORSE_OK) {
    return 0; /* not a known prosign */
  }
  return i + 1; /* include the closing '>' */
}

/* ------------------------------------------------------------------------- */
/* string encode                                                             */
/* ------------------------------------------------------------------------- */

morse_status_t morse_encode_string(const morse_table_t *table,
                                   const char *text,
                                   const morse_encode_opts_t *opts,
                                   ds_str_t *out) {
  morse_encode_opts_t local;
  const unsigned char *s;
  int emitted_any = 0;
  int pending_word = 0; /* a word break is pending before the next letter */
  char name_buf[MORSE_MAX_PATTERN + 8];

  if (table == NULL || text == NULL || out == NULL) {
    return MORSE_ERR_NULL;
  }
  if (opts == NULL) {
    morse_encode_opts_default(&local);
    opts = &local;
  }

  s = (const unsigned char *)text;
  while (*s != '\0') {
    unsigned int cp;
    size_t adv;
    const char *pattern = NULL;

    /* word break */
    if (is_space_cp(*s)) {
      if (emitted_any) {
        pending_word = 1;
      }
      s++;
      continue;
    }

    /* prosign token */
    adv = scan_prosign(table, s, name_buf, sizeof(name_buf));
    if (adv > 0) {
      (void)morse_table_lookup_prosign(table, name_buf, &pattern);
    } else {
      adv = utf8_next(s, &cp);
      if (morse_table_lookup_codepoint(table, cp_upper(cp), &pattern) !=
          MORSE_OK) {
        /* uncodeable character */
        switch (opts->unknown) {
        case MORSE_UNKNOWN_SKIP:
          s += adv;
          continue;
        case MORSE_UNKNOWN_REPLACE:
          (void)morse_table_lookup_prosign(table, "HH", &pattern); /* error */
          if (pattern == NULL) {
            s += adv;
            continue;
          }
          break;
        case MORSE_UNKNOWN_FAIL:
        default:
          return MORSE_ERR_UNKNOWN_SYMBOL;
        }
      }
    }

    /* emit the inter-token separator lazily */
    if (emitted_any) {
      if (ds_str_append_cstr(out, pending_word ? opts->word_sep
                                               : opts->letter_sep) != 0) {
        return MORSE_ERR_ALLOC;
      }
    }
    if (!append_pattern_string(out, pattern, opts)) {
      return MORSE_ERR_ALLOC;
    }
    emitted_any = 1;
    pending_word = 0;
    s += adv;
  }
  return MORSE_OK;
}

/* ------------------------------------------------------------------------- */
/* element-list encode                                                       */
/* ------------------------------------------------------------------------- */

static void free_element_node(ds_list_node_t *n) { morse_xfree(n); }

void morse_elements_free(list_t *list) {
  if (list == NULL) {
    return;
  }
  ds_list_destroy(list, free_element_node);
}

/* Append one symbol element to the list, advancing *cursor_ms. */
static int push_symbol(list_t *list, const morse_durations_t *d,
                       morse_symbol_t sym, double *cursor_ms) {
  morse_element_t *e = (morse_element_t *)morse_xmalloc(sizeof(*e));
  if (e == NULL) {
    return 0;
  }
  e->symbol = sym;
  e->duration_ms = morse_duration_of(d, sym);
  e->start_ms = *cursor_ms;
  *cursor_ms += e->duration_ms;
  ds_list_append(list, &e->node);
  return 1;
}

/* Append a pattern's marks (with intra gaps) as one character. */
static int push_pattern(list_t *list, const morse_durations_t *d,
                        const char *pattern, double *cursor_ms) {
  size_t i;
  for (i = 0; pattern[i] != '\0'; i++) {
    if (i > 0) {
      if (!push_symbol(list, d, MORSE_SYM_INTRA_GAP, cursor_ms)) {
        return 0;
      }
    }
    if (!push_symbol(list, d, pattern[i] == '-' ? MORSE_SYM_DAH : MORSE_SYM_DIT,
                     cursor_ms)) {
      return 0;
    }
  }
  return 1;
}

morse_status_t morse_encode_elements(const morse_table_t *table,
                                     const char *text,
                                     const morse_durations_t *durations,
                                     morse_unknown_policy_t unknown,
                                     list_t *out_list, double *total_ms_out) {
  const unsigned char *s;
  int emitted_any = 0;
  int pending_word = 0;
  double cursor = 0.0;
  char name_buf[MORSE_MAX_PATTERN + 8];

  if (table == NULL || text == NULL || durations == NULL || out_list == NULL) {
    return MORSE_ERR_NULL;
  }

  s = (const unsigned char *)text;
  while (*s != '\0') {
    unsigned int cp;
    size_t adv;
    const char *pattern = NULL;

    if (is_space_cp(*s)) {
      if (emitted_any) {
        pending_word = 1;
      }
      s++;
      continue;
    }

    adv = scan_prosign(table, s, name_buf, sizeof(name_buf));
    if (adv > 0) {
      (void)morse_table_lookup_prosign(table, name_buf, &pattern);
    } else {
      adv = utf8_next(s, &cp);
      if (morse_table_lookup_codepoint(table, cp_upper(cp), &pattern) !=
          MORSE_OK) {
        switch (unknown) {
        case MORSE_UNKNOWN_SKIP:
          s += adv;
          continue;
        case MORSE_UNKNOWN_REPLACE:
          (void)morse_table_lookup_prosign(table, "HH", &pattern);
          if (pattern == NULL) {
            s += adv;
            continue;
          }
          break;
        case MORSE_UNKNOWN_FAIL:
        default:
          return MORSE_ERR_UNKNOWN_SYMBOL;
        }
      }
    }

    if (emitted_any) {
      if (!push_symbol(out_list, durations,
                       pending_word ? MORSE_SYM_WORD_GAP : MORSE_SYM_CHAR_GAP,
                       &cursor)) {
        return MORSE_ERR_ALLOC;
      }
    }
    if (!push_pattern(out_list, durations, pattern, &cursor)) {
      return MORSE_ERR_ALLOC;
    }
    emitted_any = 1;
    pending_word = 0;
    s += adv;
  }

  if (total_ms_out != NULL) {
    *total_ms_out = cursor;
  }
  return MORSE_OK;
}
