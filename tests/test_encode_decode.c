/* test_encode_decode.c - String encode, decode, unknown policies, round-trips. */
#include "morse/decode.h"
#include "morse/encode.h"
#include "morse/table.h"
#include "test_util.h"

static int encode(morse_table_t *t, const char *text, const char *expect) {
  ds_str_t *out = ds_str_create();
  morse_encode_opts_t o;
  morse_status_t st;
  int ok;
  morse_encode_opts_default(&o);
  st = morse_encode_string(t, text, &o, out);
  ok = (st == MORSE_OK);
  CHECK(ok);
  if (ok) {
    CHECK_STR(FUNC_str_cstr(out), expect);
  }
  ds_str_destroy(out);
  return ok;
}

static void test_encode_basic(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  encode(t, "SOS", "... --- ...");
  encode(t, "HELLO WORLD", ".... . .-.. .-.. --- / .-- --- .-. .-.. -..");
  encode(t, "E", ".");
  encode(t, "PARIS", ".--. .- .-. .. ...");
  /* Lowercase folds to uppercase. */
  encode(t, "abc", ".- -... -.-.");
  /* Digits and punctuation. */
  encode(t, "73!", "--... ...-- -.-.--");
  morse_table_destroy(t);
}

static void test_encode_prosign_token(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  /* <AR> expands to the prosign pattern, run together as one symbol group. */
  encode(t, "<AR>", ".-.-.");
  encode(t, "<SK>", "...-.-");
  encode(t, "MSG <AR> K", "-- ... --. / .-.-. / -.-");
  morse_table_destroy(t);
}

static void test_encode_accented(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  /* CAFÉ : C A F then É (..-..). */
  encode(t, "CAF\xC3\x89", "-.-. .- ..-. ..-..");
  morse_table_destroy(t);
}

static void test_unknown_policy(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  ds_str_t *out;
  morse_encode_opts_t o;

  /* '~' has no Morse mapping. SKIP drops it. */
  out = ds_str_create();
  morse_encode_opts_default(&o);
  o.unknown = MORSE_UNKNOWN_SKIP;
  CHECK_INT(morse_encode_string(t, "A~B", &o, out), MORSE_OK);
  CHECK_STR(FUNC_str_cstr(out), ".- -...");
  ds_str_destroy(out);

  /* FAIL stops with an error. */
  out = ds_str_create();
  morse_encode_opts_default(&o);
  o.unknown = MORSE_UNKNOWN_FAIL;
  CHECK_INT(morse_encode_string(t, "A~B", &o, out), MORSE_ERR_UNKNOWN_SYMBOL);
  ds_str_destroy(out);

  morse_table_destroy(t);
}

static void test_custom_glyphs(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  ds_str_t *out = ds_str_create();
  morse_encode_opts_t o;
  morse_encode_opts_default(&o);
  o.dit = '*';
  o.dah = '_';
  CHECK_INT(morse_encode_string(t, "SO", &o, out), MORSE_OK);
  CHECK_STR(FUNC_str_cstr(out), "*** ___");
  ds_str_destroy(out);
  morse_table_destroy(t);
}

static void test_decode_basic(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  ds_str_t *out = ds_str_create();
  morse_decode_opts_t o;
  morse_decode_opts_default(&o);
  CHECK_INT(morse_decode_string(t, "... --- ...", &o, out), MORSE_OK);
  CHECK_STR(FUNC_str_cstr(out), "SOS");
  ds_str_destroy(out);

  out = ds_str_create();
  CHECK_INT(morse_decode_string(
                t, ".... . .-.. .-.. --- / .-- --- .-. .-.. -..", &o, out),
            MORSE_OK);
  CHECK_STR(FUNC_str_cstr(out), "HELLO WORLD");
  ds_str_destroy(out);
  morse_table_destroy(t);
}

/* Encode then decode must return the (uppercased) original for ASCII text. */
static void roundtrip(morse_table_t *t, const char *text) {
  ds_str_t *code = ds_str_create();
  ds_str_t *back = ds_str_create();
  morse_encode_opts_t eo;
  morse_decode_opts_t dopt;
  morse_encode_opts_default(&eo);
  morse_decode_opts_default(&dopt);
  CHECK_INT(morse_encode_string(t, text, &eo, code), MORSE_OK);
  CHECK_INT(morse_decode_string(t, FUNC_str_cstr(code), &dopt, back), MORSE_OK);
  CHECK_STR(FUNC_str_cstr(back), text);
  ds_str_destroy(code);
  ds_str_destroy(back);
}

static void test_roundtrips(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL_EXT);
  roundtrip(t, "THE QUICK BROWN FOX");
  roundtrip(t, "CQ CQ DE K1ABC");
  roundtrip(t, "PARIS CODEX 2026");
  roundtrip(t, "SOS SOS SOS");
  roundtrip(t, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  roundtrip(t, "0123456789");
  morse_table_destroy(t);
}

TEST_BEGIN("encode_decode")
test_encode_basic();
test_encode_prosign_token();
test_encode_accented();
test_unknown_policy();
test_custom_glyphs();
test_decode_basic();
test_roundtrips();
TEST_END()
