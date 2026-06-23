/*
 * morse/types.h - Fundamental types shared across the morse-deluxe core.
 *
 * The core library is plain C (C89-friendly, like the ds library it builds on)
 * and is deliberately free of any GUI or audio-backend dependency so it can be
 * unit tested in isolation. Everything user facing is prefixed `morse_`.
 */
#ifndef MORSE_TYPES_H
#define MORSE_TYPES_H

#include <stddef.h>

/* We embed ds intrusive list nodes inside our element struct, so we need the
 * real definition of list_node_t. ds.h pulls in the whole library; that is
 * fine for the core translation units. */
#include "ds.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Status / error reporting                                                  */
/* ------------------------------------------------------------------------- */

typedef enum morse_status {
  MORSE_OK = 0,
  MORSE_ERR_NULL = -1,        /* a required argument was NULL                 */
  MORSE_ERR_ALLOC = -2,       /* an allocation failed                         */
  MORSE_ERR_RANGE = -3,       /* numeric argument out of range                */
  MORSE_ERR_UNKNOWN_SYMBOL = -4, /* char/pattern not present in the codebook  */
  MORSE_ERR_BAD_PATTERN = -5, /* pattern contained an illegal element        */
  MORSE_ERR_IO = -6,          /* file / stream I/O failure                    */
  MORSE_ERR_FORMAT = -7,      /* malformed container (e.g. bad WAV header)    */
  MORSE_ERR_STATE = -8,       /* call made in an invalid object state         */
  MORSE_ERR_UNSUPPORTED = -9  /* feature compiled out / not available         */
} morse_status_t;

const char *morse_status_str(morse_status_t status);

/* ------------------------------------------------------------------------- */
/* Symbols and elements                                                      */
/* ------------------------------------------------------------------------- */

/* The atoms of Morse. A "mark" is key-down (tone on); a "space" is key-up. */
typedef enum morse_symbol {
  MORSE_SYM_DIT = 0,       /* short mark   ( . )                              */
  MORSE_SYM_DAH,           /* long mark    ( - )                              */
  MORSE_SYM_INTRA_GAP,     /* 1-unit space between elements of one character  */
  MORSE_SYM_CHAR_GAP,      /* 3-unit space between characters                 */
  MORSE_SYM_WORD_GAP,      /* 7-unit space between words                      */
  MORSE_SYM_COUNT
} morse_symbol_t;

/* True for the two "tone on" symbols. */
int morse_symbol_is_mark(morse_symbol_t s);
/* Single-character glyph used for human readable rendering. */
char morse_symbol_glyph(morse_symbol_t s);

/*
 * A timed element in a rendered stream. These are produced by the encoder as
 * an intrusive ds doubly... actually singly linked list: the ds list node is
 * the FIRST member so ds's CAST() macro (a plain pointer cast) recovers the
 * element from a node pointer.
 */
typedef struct morse_element {
  list_node_t node;        /* MUST be first: ds CAST() relies on it           */
  morse_symbol_t symbol;
  double duration_ms;      /* resolved duration for this symbol               */
  double start_ms;         /* offset from the start of the stream             */
} morse_element_t;

/* ------------------------------------------------------------------------- */
/* Policies                                                                  */
/* ------------------------------------------------------------------------- */

/* What to do when the encoder meets a character absent from the codebook. */
typedef enum morse_unknown_policy {
  MORSE_UNKNOWN_SKIP = 0,  /* silently drop it                                */
  MORSE_UNKNOWN_REPLACE,   /* emit the error prosign (8 dits) in its place    */
  MORSE_UNKNOWN_FAIL       /* abort encoding with MORSE_ERR_UNKNOWN_SYMBOL    */
} morse_unknown_policy_t;

#ifdef __cplusplus
}
#endif

#endif /* MORSE_TYPES_H */
