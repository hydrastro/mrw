/* types.c - status strings and symbol helpers. */
#include "morse/types.h"

const char *morse_status_str(morse_status_t status) {
  switch (status) {
  case MORSE_OK:
    return "ok";
  case MORSE_ERR_NULL:
    return "null argument";
  case MORSE_ERR_ALLOC:
    return "allocation failed";
  case MORSE_ERR_RANGE:
    return "argument out of range";
  case MORSE_ERR_UNKNOWN_SYMBOL:
    return "unknown symbol";
  case MORSE_ERR_BAD_PATTERN:
    return "malformed pattern";
  case MORSE_ERR_IO:
    return "I/O error";
  case MORSE_ERR_FORMAT:
    return "malformed format";
  case MORSE_ERR_STATE:
    return "invalid state";
  case MORSE_ERR_UNSUPPORTED:
    return "unsupported";
  }
  return "unknown status";
}

int morse_symbol_is_mark(morse_symbol_t s) {
  return s == MORSE_SYM_DIT || s == MORSE_SYM_DAH;
}

char morse_symbol_glyph(morse_symbol_t s) {
  switch (s) {
  case MORSE_SYM_DIT:
    return '.';
  case MORSE_SYM_DAH:
    return '-';
  case MORSE_SYM_INTRA_GAP:
    return '_';
  case MORSE_SYM_CHAR_GAP:
    return ' ';
  case MORSE_SYM_WORD_GAP:
    return '/';
  case MORSE_SYM_COUNT:
  default:
    return '?';
  }
}
