/* test_table.c - Codebook: variant sizes, encode/decode lookups, prosigns. */
#include "morse/table.h"
#include "test_util.h"

static const char *enc(const morse_table_t *t, unsigned int cp) {
  const char *p = NULL;
  if (morse_table_lookup_codepoint(t, cp, &p) != MORSE_OK) {
    return "<none>";
  }
  return p;
}

static void test_sizes(void) {
  morse_table_t *base = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  morse_table_t *ext = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  CHECK(base != NULL);
  CHECK(ext != NULL);
  /* Base: 26 letters + 10 digits. */
  CHECK_INT(morse_table_size(base), 36);
  /* Extended: + 18 punct + 11 accented + 11 prosigns. */
  CHECK_INT(morse_table_size(ext), 76);
  CHECK_INT(morse_table_variant(base), MORSE_VARIANT_INTERNATIONAL);
  CHECK_INT(morse_table_variant(ext), MORSE_VARIANT_INTERNATIONAL_EXT);
  morse_table_destroy(base);
  morse_table_destroy(ext);
}

static void test_encode_lookups(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  CHECK_STR(enc(t, 'E'), ".");
  CHECK_STR(enc(t, 'T'), "-");
  CHECK_STR(enc(t, 'A'), ".-");
  CHECK_STR(enc(t, 'N'), "-.");
  CHECK_STR(enc(t, 'S'), "...");
  CHECK_STR(enc(t, 'O'), "---");
  CHECK_STR(enc(t, 'H'), "....");
  CHECK_STR(enc(t, 'V'), "...-");
  CHECK_STR(enc(t, '1'), ".----");
  CHECK_STR(enc(t, '5'), ".....");
  CHECK_STR(enc(t, '0'), "-----");
  /* Punctuation (extended only). */
  CHECK_STR(enc(t, '?'), "..--..");
  CHECK_STR(enc(t, '/'), "-..-.");
  CHECK_STR(enc(t, '='), "-...-");
  CHECK_STR(enc(t, '.'), ".-.-.-");
  CHECK_STR(enc(t, '@'), ".--.-.");
  /* Accented (É = U+00C9). */
  CHECK_STR(enc(t, 0x00C9u), "..-..");
  morse_table_destroy(t);
}

static void test_variant_gating(void) {
  /* Punctuation and accented are absent from the base variant. */
  morse_table_t *base = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  const char *p = NULL;
  CHECK_INT(morse_table_lookup_codepoint(base, '?', &p),
            MORSE_ERR_UNKNOWN_SYMBOL);
  CHECK_INT(morse_table_lookup_codepoint(base, 0x00C9u, &p),
            MORSE_ERR_UNKNOWN_SYMBOL);
  /* Letters still present. */
  CHECK_STR(enc(base, 'S'), "...");
  morse_table_destroy(base);
}

static void test_decode(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  const morse_entry_t *e = NULL;
  CHECK_INT(morse_table_decode_pattern(t, "...", &e), MORSE_OK);
  CHECK_INT(e->codepoint, 'S');
  CHECK_INT(morse_table_decode_pattern(t, "-", &e), MORSE_OK);
  CHECK_INT(e->codepoint, 'T');
  CHECK_INT(morse_table_decode_pattern(t, ".----", &e), MORSE_OK);
  CHECK_INT(e->codepoint, '1');
  CHECK_INT(morse_table_decode_pattern(t, "-...-", &e), MORSE_OK);
  CHECK_INT(e->codepoint, '=');
  CHECK_INT(morse_table_decode_pattern(t, "..--..", &e), MORSE_OK);
  CHECK_INT(e->codepoint, '?');

  /* A well-formed but unassigned pattern: unknown symbol. */
  CHECK_INT(morse_table_decode_pattern(t, "--------", &e),
            MORSE_ERR_UNKNOWN_SYMBOL);
  /* A pattern containing a non dot/dash character: bad pattern. */
  CHECK_INT(morse_table_decode_pattern(t, "..x..", &e), MORSE_ERR_BAD_PATTERN);
  morse_table_destroy(t);
}

static void test_prosigns(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  const char *p = NULL;
  CHECK_INT(morse_table_lookup_prosign(t, "SOS", &p), MORSE_OK);
  CHECK_STR(p, "...---...");
  CHECK_INT(morse_table_lookup_prosign(t, "AR", &p), MORSE_OK);
  CHECK_STR(p, ".-.-.");
  CHECK_INT(morse_table_lookup_prosign(t, "BT", &p), MORSE_OK);
  CHECK_STR(p, "-...-");
  CHECK_INT(morse_table_lookup_prosign(t, "SK", &p), MORSE_OK);
  CHECK_STR(p, "...-.-");
  /* Case-insensitive. */
  CHECK_INT(morse_table_lookup_prosign(t, "sos", &p), MORSE_OK);
  CHECK_STR(p, "...---...");
  /* Unknown prosign name. */
  CHECK(morse_table_lookup_prosign(t, "ZZ", &p) != MORSE_OK);

  /* Prosigns are encode-only: their multi-char patterns must NOT decode to a
   * single entry. "...---..." is nine elements, longer than any character. */
  {
    const morse_entry_t *e = NULL;
    CHECK(morse_table_decode_pattern(t, "...---...", &e) != MORSE_OK);
  }
  morse_table_destroy(t);
}

static void test_view(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  const morse_entry_t *first = morse_table_entry_at(t, 0);
  CHECK(first != NULL);
  CHECK_INT(first->codepoint, 'A'); /* view starts with letters */
  CHECK(morse_table_entry_at(t, 76) == NULL); /* out of range */
  CHECK(morse_table_entry_at(t, 1000) == NULL);
  morse_table_destroy(t);
}

TEST_BEGIN("table")
test_sizes();
test_encode_lookups();
test_variant_gating();
test_decode();
test_prosigns();
test_view();
TEST_END()
