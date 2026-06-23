/*
 * table.c - The codebook, wired into ds structures.
 *
 *   decode : ds trie, 2 splits (dit=0, dah=1) = the Morse dichotomic tree.
 *   encode : ds hash table keyed by Unicode code point.
 *
 * The actual code data lives in immortal static arrays, so the trie/hash only
 * store borrowed pointers (no per-entry ownership). Prosigns are encode-only
 * and addressed by name; they are deliberately kept out of the decode trie
 * because their patterns alias ordinary characters (AR == "+", BT == "=", ...).
 */
#include "morse/table.h"
#include "morse_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Static code data                                                          */
/* ------------------------------------------------------------------------- */

static const morse_entry_t LETTERS[] = {
    {'A', "A", ".-", MORSE_CLASS_LETTER},
    {'B', "B", "-...", MORSE_CLASS_LETTER},
    {'C', "C", "-.-.", MORSE_CLASS_LETTER},
    {'D', "D", "-..", MORSE_CLASS_LETTER},
    {'E', "E", ".", MORSE_CLASS_LETTER},
    {'F', "F", "..-.", MORSE_CLASS_LETTER},
    {'G', "G", "--.", MORSE_CLASS_LETTER},
    {'H', "H", "....", MORSE_CLASS_LETTER},
    {'I', "I", "..", MORSE_CLASS_LETTER},
    {'J', "J", ".---", MORSE_CLASS_LETTER},
    {'K', "K", "-.-", MORSE_CLASS_LETTER},
    {'L', "L", ".-..", MORSE_CLASS_LETTER},
    {'M', "M", "--", MORSE_CLASS_LETTER},
    {'N', "N", "-.", MORSE_CLASS_LETTER},
    {'O', "O", "---", MORSE_CLASS_LETTER},
    {'P', "P", ".--.", MORSE_CLASS_LETTER},
    {'Q', "Q", "--.-", MORSE_CLASS_LETTER},
    {'R', "R", ".-.", MORSE_CLASS_LETTER},
    {'S', "S", "...", MORSE_CLASS_LETTER},
    {'T', "T", "-", MORSE_CLASS_LETTER},
    {'U', "U", "..-", MORSE_CLASS_LETTER},
    {'V', "V", "...-", MORSE_CLASS_LETTER},
    {'W', "W", ".--", MORSE_CLASS_LETTER},
    {'X', "X", "-..-", MORSE_CLASS_LETTER},
    {'Y', "Y", "-.--", MORSE_CLASS_LETTER},
    {'Z', "Z", "--..", MORSE_CLASS_LETTER}};

static const morse_entry_t DIGITS[] = {
    {'0', "0", "-----", MORSE_CLASS_DIGIT},
    {'1', "1", ".----", MORSE_CLASS_DIGIT},
    {'2', "2", "..---", MORSE_CLASS_DIGIT},
    {'3', "3", "...--", MORSE_CLASS_DIGIT},
    {'4', "4", "....-", MORSE_CLASS_DIGIT},
    {'5', "5", ".....", MORSE_CLASS_DIGIT},
    {'6', "6", "-....", MORSE_CLASS_DIGIT},
    {'7', "7", "--...", MORSE_CLASS_DIGIT},
    {'8', "8", "---..", MORSE_CLASS_DIGIT},
    {'9', "9", "----.", MORSE_CLASS_DIGIT}};

static const morse_entry_t PUNCT[] = {
    {'.', ".", ".-.-.-", MORSE_CLASS_PUNCT},
    {',', ",", "--..--", MORSE_CLASS_PUNCT},
    {'?', "?", "..--..", MORSE_CLASS_PUNCT},
    {'\'', "'", ".----.", MORSE_CLASS_PUNCT},
    {'!', "!", "-.-.--", MORSE_CLASS_PUNCT},
    {'/', "/", "-..-.", MORSE_CLASS_PUNCT},
    {'(', "(", "-.--.", MORSE_CLASS_PUNCT},
    {')', ")", "-.--.-", MORSE_CLASS_PUNCT},
    {'&', "&", ".-...", MORSE_CLASS_PUNCT},
    {':', ":", "---...", MORSE_CLASS_PUNCT},
    {';', ";", "-.-.-.", MORSE_CLASS_PUNCT},
    {'=', "=", "-...-", MORSE_CLASS_PUNCT},
    {'+', "+", ".-.-.", MORSE_CLASS_PUNCT},
    {'-', "-", "-....-", MORSE_CLASS_PUNCT},
    {'_', "_", "..--.-", MORSE_CLASS_PUNCT},
    {'"', "\"", ".-..-.", MORSE_CLASS_PUNCT},
    {'$', "$", "...-..-", MORSE_CLASS_PUNCT},
    {'@', "@", ".--.-.", MORSE_CLASS_PUNCT}};

/* Accented Latin (ITU and common amateur usage). Some patterns alias each
 * other (e.g. A-ring == A-grave); the decode builder keeps the first. */
static const morse_entry_t ACCENTED[] = {
    {0x00C0, "\xC3\x80", ".--.-", MORSE_CLASS_ACCENTED},  /* A grave */
    {0x00C4, "\xC3\x84", ".-.-", MORSE_CLASS_ACCENTED},   /* A diaeresis */
    {0x00C5, "\xC3\x85", ".--.-", MORSE_CLASS_ACCENTED},  /* A ring (==A grave) */
    {0x00C7, "\xC3\x87", "-.-..", MORSE_CLASS_ACCENTED},  /* C cedilla */
    {0x00C8, "\xC3\x88", ".-..-", MORSE_CLASS_ACCENTED},  /* E grave */
    {0x00C9, "\xC3\x89", "..-..", MORSE_CLASS_ACCENTED},  /* E acute */
    {0x00D1, "\xC3\x91", "--.--", MORSE_CLASS_ACCENTED},  /* N tilde */
    {0x00D6, "\xC3\x96", "---.", MORSE_CLASS_ACCENTED},   /* O diaeresis */
    {0x00DC, "\xC3\x9C", "..--", MORSE_CLASS_ACCENTED},   /* U diaeresis */
    {0x00D0, "\xC3\x90", "..--.", MORSE_CLASS_ACCENTED},  /* Eth */
    {0x00DE, "\xC3\x9E", ".--..", MORSE_CLASS_ACCENTED}}; /* Thorn */

/* Prosigns: codepoint 0, addressed by name. Encode-only (run together, no
 * inter-letter gaps). Patterns intentionally overlap ordinary characters. */
static const morse_entry_t PROSIGNS[] = {
    {0, "AR", ".-.-.", MORSE_CLASS_PROSIGN},      /* end of message */
    {0, "AS", ".-...", MORSE_CLASS_PROSIGN},      /* wait */
    {0, "BT", "-...-", MORSE_CLASS_PROSIGN},      /* break / new paragraph */
    {0, "KN", "-.--.", MORSE_CLASS_PROSIGN},      /* go ahead, named station */
    {0, "KA", "-.-.-", MORSE_CLASS_PROSIGN},      /* starting signal (CT) */
    {0, "CT", "-.-.-", MORSE_CLASS_PROSIGN},      /* alias of KA */
    {0, "SK", "...-.-", MORSE_CLASS_PROSIGN},     /* end of work (VA) */
    {0, "SN", "...-.", MORSE_CLASS_PROSIGN},      /* understood (VE) */
    {0, "SOS", "...---...", MORSE_CLASS_PROSIGN}, /* distress */
    {0, "HH", "........", MORSE_CLASS_PROSIGN},   /* error (eight dits) */
    {0, "CL", "-.-..-..", MORSE_CLASS_PROSIGN}};  /* closing down */

/* ------------------------------------------------------------------------- */
/* trie slice callbacks                                                      */
/* ------------------------------------------------------------------------- */

static size_t pat_get_slice(void *data, size_t i) {
  const char *p = (const char *)data;
  return (p[i] == '-') ? 1u : 0u;
}

static bool pat_has_slice(void *data, size_t i) {
  const char *p = (const char *)data;
  size_t n = 0;
  while (p[n] != '\0') {
    n++;
  }
  return i < n;
}

/* ------------------------------------------------------------------------- */
/* hash callbacks (key is a code point widened to a pointer)                 */
/* ------------------------------------------------------------------------- */

static size_t cp_hash(void *key, void *user) {
  (void)user;
  return (size_t)(uintptr_t)key;
}

static int cp_cmp(void *a, void *b, void *user) {
  uintptr_t x = (uintptr_t)a;
  uintptr_t y = (uintptr_t)b;
  (void)user;
  return (x > y) - (x < y);
}

/* ------------------------------------------------------------------------- */
/* table object                                                              */
/* ------------------------------------------------------------------------- */

struct morse_table {
  morse_variant_t variant;
  ds_hash_table_t *encode; /* codepoint -> const morse_entry_t*              */
  ds_trie_t *decode;       /* pattern   -> const morse_entry_t*              */
  const morse_entry_t **view; /* all displayable entries (incl. prosigns)    */
  size_t view_count;
  const morse_entry_t *prosigns;
  size_t prosign_count;
};

static int ascii_casefold(int c) {
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 'A';
  }
  return c;
}

static int name_ieq(const char *a, const char *b) {
  while (*a && *b) {
    if (ascii_casefold((unsigned char)*a) != ascii_casefold((unsigned char)*b)) {
      return 0;
    }
    a++;
    b++;
  }
  return *a == *b;
}

/* Insert one entry into the decode trie, first-wins on aliased patterns. */
static void decode_put(ds_trie_t *trie, const morse_entry_t *e) {
  ds_trie_node_t *node = NULL;
  /* If this pattern already resolves to a terminal, keep the existing entry. */
  if (ds_trie_get(trie, (void *)e->pattern, pat_get_slice, pat_has_slice,
                  &node) == DS_OK &&
      node != NULL && node->is_terminal && node->terminal_data != NULL) {
    return;
  }
  ds_trie_insert(trie, (void *)e->pattern, pat_get_slice, pat_has_slice);
  node = NULL;
  if (ds_trie_get(trie, (void *)e->pattern, pat_get_slice, pat_has_slice,
                  &node) == DS_OK &&
      node != NULL) {
    node->terminal_data = (void *)e; /* overwrite trie's default (the pattern) */
  }
}

static int encode_put(ds_hash_table_t *h, const morse_entry_t *e) {
  void *key = (void *)(uintptr_t)e->codepoint;
  return ds_hash_table_insert(h, key, (void *)e) == DS_OK;
}

morse_table_t *morse_table_create(morse_variant_t variant) {
  morse_table_t *t;
  ds_hash_table_config_t cfg;
  size_t i;
  int extended;
  size_t cap;

  if (variant < 0 || variant >= MORSE_VARIANT_COUNT) {
    return NULL;
  }
  extended = (variant == MORSE_VARIANT_INTERNATIONAL_EXT);

  t = (morse_table_t *)morse_xcalloc(1, sizeof(*t));
  if (t == NULL) {
    return NULL;
  }
  t->variant = variant;

  t->decode = ds_trie_create_hash_alloc(2, morse_xmalloc, morse_xfree);
  if (t->decode == NULL) {
    morse_xfree(t);
    return NULL;
  }

  ds_hash_table_config_init(&cfg);
  cfg.hash = cp_hash;
  cfg.compare = cp_cmp;
  cfg.capacity = 97u;
  /* The config constructor manages this table with malloc/free internally;
   * that is fine - each ds container frees with the allocator it was built
   * with. The encode table is built once at startup, so it is not part of the
   * dynamic allocation activity the diagnostics panel is meant to surface. */
  t->encode = ds_hash_table_create_config(&cfg);
  if (t->encode == NULL) {
    ds_trie_destroy(t->decode);
    morse_xfree(t);
    return NULL;
  }

  /* Letters and digits are in every variant. */
  for (i = 0; i < sizeof(LETTERS) / sizeof(LETTERS[0]); i++) {
    decode_put(t->decode, &LETTERS[i]);
    encode_put(t->encode, &LETTERS[i]);
  }
  for (i = 0; i < sizeof(DIGITS) / sizeof(DIGITS[0]); i++) {
    decode_put(t->decode, &DIGITS[i]);
    encode_put(t->encode, &DIGITS[i]);
  }
  if (extended) {
    for (i = 0; i < sizeof(PUNCT) / sizeof(PUNCT[0]); i++) {
      decode_put(t->decode, &PUNCT[i]);
      encode_put(t->encode, &PUNCT[i]);
    }
    for (i = 0; i < sizeof(ACCENTED) / sizeof(ACCENTED[0]); i++) {
      decode_put(t->decode, &ACCENTED[i]);
      encode_put(t->encode, &ACCENTED[i]);
    }
    t->prosigns = PROSIGNS;
    t->prosign_count = sizeof(PROSIGNS) / sizeof(PROSIGNS[0]);
  }

  /* Build the display view: letters, digits, then (extended) punct, accented,
   * prosigns. */
  cap = sizeof(LETTERS) / sizeof(LETTERS[0]) +
        sizeof(DIGITS) / sizeof(DIGITS[0]);
  if (extended) {
    cap += sizeof(PUNCT) / sizeof(PUNCT[0]) +
           sizeof(ACCENTED) / sizeof(ACCENTED[0]) +
           sizeof(PROSIGNS) / sizeof(PROSIGNS[0]);
  }
  t->view = (const morse_entry_t **)morse_xcalloc(cap, sizeof(*t->view));
  if (t->view == NULL) {
    ds_hash_table_destroy(t->encode);
    ds_trie_destroy(t->decode);
    morse_xfree(t);
    return NULL;
  }
  t->view_count = 0;
  for (i = 0; i < sizeof(LETTERS) / sizeof(LETTERS[0]); i++) {
    t->view[t->view_count++] = &LETTERS[i];
  }
  for (i = 0; i < sizeof(DIGITS) / sizeof(DIGITS[0]); i++) {
    t->view[t->view_count++] = &DIGITS[i];
  }
  if (extended) {
    for (i = 0; i < sizeof(PUNCT) / sizeof(PUNCT[0]); i++) {
      t->view[t->view_count++] = &PUNCT[i];
    }
    for (i = 0; i < sizeof(ACCENTED) / sizeof(ACCENTED[0]); i++) {
      t->view[t->view_count++] = &ACCENTED[i];
    }
    for (i = 0; i < sizeof(PROSIGNS) / sizeof(PROSIGNS[0]); i++) {
      t->view[t->view_count++] = &PROSIGNS[i];
    }
  }
  return t;
}

void morse_table_destroy(morse_table_t *table) {
  if (table == NULL) {
    return;
  }
  /* Values/keys are borrowed (static or integer); destroy frees only nodes. */
  if (table->encode != NULL) {
    ds_hash_table_destroy(table->encode);
  }
  if (table->decode != NULL) {
    ds_trie_destroy(table->decode);
  }
  morse_xfree(table->view);
  morse_xfree(table);
}

morse_variant_t morse_table_variant(const morse_table_t *table) {
  return table ? table->variant : MORSE_VARIANT_INTERNATIONAL;
}

morse_status_t morse_table_lookup_codepoint(const morse_table_t *table,
                                            unsigned int codepoint,
                                            const char **out_pattern) {
  void *value = NULL;
  ds_status_t s;
  if (table == NULL || out_pattern == NULL) {
    return MORSE_ERR_NULL;
  }
  s = ds_hash_table_get(table->encode, (void *)(uintptr_t)codepoint, &value);
  if (s != DS_OK || value == NULL) {
    return MORSE_ERR_UNKNOWN_SYMBOL;
  }
  *out_pattern = ((const morse_entry_t *)value)->pattern;
  return MORSE_OK;
}

morse_status_t morse_table_lookup_prosign(const morse_table_t *table,
                                          const char *name,
                                          const char **out_pattern) {
  size_t i;
  if (table == NULL || name == NULL || out_pattern == NULL) {
    return MORSE_ERR_NULL;
  }
  for (i = 0; i < table->prosign_count; i++) {
    if (name_ieq(table->prosigns[i].name, name)) {
      *out_pattern = table->prosigns[i].pattern;
      return MORSE_OK;
    }
  }
  return MORSE_ERR_UNKNOWN_SYMBOL;
}

morse_status_t morse_table_decode_pattern(const morse_table_t *table,
                                          const char *pattern,
                                          const morse_entry_t **out_entry) {
  ds_trie_node_t *node = NULL;
  size_t i;
  if (table == NULL || pattern == NULL || out_entry == NULL) {
    return MORSE_ERR_NULL;
  }
  if (pattern[0] == '\0') {
    return MORSE_ERR_UNKNOWN_SYMBOL;
  }
  for (i = 0; pattern[i] != '\0'; i++) {
    if (pattern[i] != '.' && pattern[i] != '-') {
      return MORSE_ERR_BAD_PATTERN;
    }
    if (i >= (size_t)(MORSE_MAX_PATTERN - 1)) {
      return MORSE_ERR_BAD_PATTERN; /* far longer than any real code */
    }
  }
  if (ds_trie_get(table->decode, (void *)pattern, pat_get_slice, pat_has_slice,
                  &node) != DS_OK ||
      node == NULL || !node->is_terminal || node->terminal_data == NULL) {
    return MORSE_ERR_UNKNOWN_SYMBOL;
  }
  *out_entry = (const morse_entry_t *)node->terminal_data;
  return MORSE_OK;
}

size_t morse_table_size(const morse_table_t *table) {
  return table ? table->view_count : 0;
}

const morse_entry_t *morse_table_entry_at(const morse_table_t *table,
                                          size_t index) {
  if (table == NULL || index >= table->view_count) {
    return NULL;
  }
  return table->view[index];
}
