/*
 * morse/table.h - The codebook: bidirectional character <-> pattern mapping.
 *
 * Internally the table keeps two ds structures wired from the same source data:
 *
 *   encode (codepoint -> pattern) : a ds hash table
 *   decode (pattern   -> codepoint): a ds trie with num_splits = 2, i.e. the
 *                                    classic Morse dichotomic tree where a dit
 *                                    descends the 0-branch and a dah the
 *                                    1-branch. terminal_data carries the entry.
 *
 * Patterns are NUL-terminated ASCII strings over the two-character alphabet
 * { '.', '-' } and never exceed MORSE_MAX_PATTERN-1 elements.
 */
#ifndef MORSE_TABLE_H
#define MORSE_TABLE_H

#include "morse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MORSE_MAX_PATTERN 12 /* longest standard pattern is 8; leave headroom */

/* Selectable code variants. The decoder is always driven by whatever variant
 * the table was built with. */
typedef enum morse_variant {
  MORSE_VARIANT_INTERNATIONAL = 0, /* ITU-R M.1677 letters + digits           */
  MORSE_VARIANT_INTERNATIONAL_EXT, /* + punctuation, symbols, accented Latin  */
  MORSE_VARIANT_COUNT
} morse_variant_t;

/* Logical class of an entry, used for the reference chart and filtering. */
typedef enum morse_entry_class {
  MORSE_CLASS_LETTER = 0,
  MORSE_CLASS_DIGIT,
  MORSE_CLASS_PUNCT,
  MORSE_CLASS_ACCENTED,
  MORSE_CLASS_PROSIGN
} morse_entry_class_t;

/*
 * One codebook entry. `codepoint` is a Unicode scalar value for normal
 * characters (ASCII fits directly) and 0 for prosigns, which are addressed by
 * `name` instead (e.g. "SOS", "AR", "SK").
 */
typedef struct morse_entry {
  unsigned int codepoint;             /* 0 for prosigns                       */
  const char *name;                   /* display label / prosign key          */
  char pattern[MORSE_MAX_PATTERN];    /* ".-" style, NUL terminated           */
  morse_entry_class_t klass;
} morse_entry_t;

typedef struct morse_table morse_table_t; /* opaque                          */

/* Build/destroy. Returns NULL on allocation failure. */
morse_table_t *morse_table_create(morse_variant_t variant);
void morse_table_destroy(morse_table_t *table);

morse_variant_t morse_table_variant(const morse_table_t *table);

/* ---- encode direction (character -> pattern) -------------------------- */

/* Look up the pattern for a Unicode codepoint. On success returns MORSE_OK and
 * points *out at an internal NUL-terminated pattern string (owned by table). */
morse_status_t morse_table_lookup_codepoint(const morse_table_t *table,
                                            unsigned int codepoint,
                                            const char **out_pattern);

/* Look up a prosign by name (case-insensitive), e.g. "SOS". */
morse_status_t morse_table_lookup_prosign(const morse_table_t *table,
                                          const char *name,
                                          const char **out_pattern);

/* ---- decode direction (pattern -> character) -------------------------- */

/* Resolve a full ".-" pattern to an entry via the trie. Returns MORSE_OK and
 * sets *out_entry on success, MORSE_ERR_UNKNOWN_SYMBOL if the pattern is not a
 * terminal node, MORSE_ERR_BAD_PATTERN if it contains illegal characters. */
morse_status_t morse_table_decode_pattern(const morse_table_t *table,
                                          const char *pattern,
                                          const morse_entry_t **out_entry);

/* ---- introspection (for the reference chart) -------------------------- */

size_t morse_table_size(const morse_table_t *table);
const morse_entry_t *morse_table_entry_at(const morse_table_t *table,
                                          size_t index);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_TABLE_H */
