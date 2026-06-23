/*
 * test_cw.c - Tests for the CW gear-interface layer (pure logic): the iambic
 * keyer state machine, the keyed-text sender, the cwdaemon request parser, and
 * the WinKeyer command builder.
 */
#include "morse/cw.h"
#include "morse/morse.h"
#include "test_util.h"

#include <string.h>

/* ---- iambic keyer ------------------------------------------------------- */

/* Run the keyer for `ms` with fixed paddle inputs; collect completed elements
 * into out[] (0=dit,1=dah). Returns the number collected. */
static int run_keyer(morse_iambic_t *k, int dit, int dah, double ms, int *out,
                     int cap) {
  double t = 0.0;
  int n = 0;
  const double dt = 1.0;
  for (t = 0.0; t < ms; t += dt) {
    int el = -1;
    morse_iambic_tick(k, dit, dah, dt, &el);
    if (el >= 0 && n < cap) {
      out[n++] = el;
    }
  }
  return n;
}

static void test_iambic_dit_hold(void) {
  morse_iambic_t k;
  int els[64];
  int n, i, alldit = 1;
  morse_iambic_init(&k, MORSE_IAMBIC_A, 60.0);
  /* dit element = 60ms key + 60ms gap = 120ms; ~8 dits in 1000ms */
  n = run_keyer(&k, 1, 0, 1000.0, els, 64);
  CHECK(n >= 7 && n <= 9);
  for (i = 0; i < n; i++) {
    if (els[i] != 0) {
      alldit = 0;
    }
  }
  CHECK(alldit);
}

static void test_iambic_dah_hold(void) {
  morse_iambic_t k;
  int els[64];
  int n, i, alldah = 1;
  morse_iambic_init(&k, MORSE_IAMBIC_A, 60.0);
  /* dah element = 180ms key + 60ms gap = 240ms; ~4 dahs in 1000ms */
  n = run_keyer(&k, 0, 1, 1000.0, els, 64);
  CHECK(n >= 3 && n <= 5);
  for (i = 0; i < n; i++) {
    if (els[i] != 1) {
      alldah = 0;
    }
  }
  CHECK(alldah);
}

static void test_iambic_squeeze_alternates(void) {
  morse_iambic_t k;
  int els[64];
  int n, i, alt = 1;
  morse_iambic_init(&k, MORSE_IAMBIC_A, 60.0);
  n = run_keyer(&k, 1, 1, 1000.0, els, 64);
  CHECK(n >= 4);
  /* squeeze must alternate dit/dah (starting on a dit per our init) */
  CHECK_INT(els[0], 0);
  for (i = 1; i < n; i++) {
    if (els[i] == els[i - 1]) {
      alt = 0;
    }
  }
  CHECK(alt);
}

static void test_iambic_idle(void) {
  morse_iambic_t k;
  int el = -1, key;
  morse_iambic_init(&k, MORSE_IAMBIC_A, 60.0);
  key = morse_iambic_tick(&k, 0, 0, 5.0, &el);
  CHECK_INT(key, 0);
  CHECK_INT(el, -1);
}

/* ---- keyed text sender -------------------------------------------------- */

typedef struct {
  int marks;          /* number of key-down pulses */
  double mark_ms[64]; /* their durations */
  double total_ms;
  int key_state;
  double pending;
} keyrec_t;

static void rec_key(int on, void *user) {
  keyrec_t *r = (keyrec_t *)user;
  if (on) {
    r->key_state = 1;
    r->pending = 0.0;
  } else {
    /* a mark just ended: record the time accumulated while key was down */
    if (r->key_state == 1 && r->marks < 64) {
      r->mark_ms[r->marks++] = r->pending;
    }
    r->key_state = 0;
  }
}

static void rec_delay(double ms, void *user) {
  keyrec_t *r = (keyrec_t *)user;
  r->total_ms += ms;
  if (r->key_state == 1) {
    r->pending += ms;
  }
}

static void test_sender_single_dit(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  morse_timing_t tm;
  morse_durations_t d;
  keyrec_t r;
  memset(&r, 0, sizeof(r));
  morse_timing_default(&tm);
  morse_timing_resolve(&tm, &d);
  /* 'E' is a single dit */
  CHECK_INT(morse_cw_key_text(t, "E", &d, rec_key, rec_delay, &r), MORSE_OK);
  CHECK_INT(r.marks, 1);
  CHECK_NEAR(r.mark_ms[0], d.dit_ms, 1.0);
  morse_table_destroy(t);
}

static void test_sender_letter_a(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  morse_timing_t tm;
  morse_durations_t d;
  keyrec_t r;
  memset(&r, 0, sizeof(r));
  morse_timing_default(&tm);
  morse_timing_resolve(&tm, &d);
  /* 'A' is dit-dah */
  CHECK_INT(morse_cw_key_text(t, "A", &d, rec_key, rec_delay, &r), MORSE_OK);
  CHECK_INT(r.marks, 2);
  CHECK_NEAR(r.mark_ms[0], d.dit_ms, 1.0);
  CHECK_NEAR(r.mark_ms[1], d.dah_ms, 1.0);
  morse_table_destroy(t);
}

static void test_sender_counts_marks(void) {
  morse_table_t *t = morse_table_create(MORSE_VARIANT_INTERNATIONAL);
  morse_timing_t tm;
  morse_durations_t d;
  keyrec_t r;
  memset(&r, 0, sizeof(r));
  morse_timing_default(&tm);
  morse_timing_resolve(&tm, &d);
  /* "SOS" = ... --- ... = 9 marks */
  CHECK_INT(morse_cw_key_text(t, "SOS", &d, rec_key, rec_delay, &r), MORSE_OK);
  CHECK_INT(r.marks, 9);
  CHECK(r.total_ms > 0.0);
  morse_table_destroy(t);
}

/* ---- cwdaemon parser ---------------------------------------------------- */

static void test_cwd_text(void) {
  morse_cwd_msg_t m;
  const char *s = "CQ TEST\n";
  CHECK_INT(morse_cwd_parse(s, strlen(s), &m), MORSE_OK);
  CHECK_INT(m.kind, MORSE_CWD_TEXT);
  CHECK_INT((int)m.text_len, 7); /* newline stripped */
  CHECK(strncmp(m.text, "CQ TEST", 7) == 0);
}

static void test_cwd_speed_tone(void) {
  morse_cwd_msg_t m;
  char s1[8];
  char s2[8];
  s1[0] = 0x1B;
  s1[1] = '2';
  s1[2] = '3';
  s1[3] = '0';
  CHECK_INT(morse_cwd_parse(s1, 4, &m), MORSE_OK);
  CHECK_INT(m.kind, MORSE_CWD_SPEED);
  CHECK_INT(m.ival, 30);
  s2[0] = 0x1B;
  s2[1] = '3';
  s2[2] = '6';
  s2[3] = '5';
  s2[4] = '0';
  CHECK_INT(morse_cwd_parse(s2, 5, &m), MORSE_OK);
  CHECK_INT(m.kind, MORSE_CWD_TONE);
  CHECK_INT(m.ival, 650);
}

static void test_cwd_commands(void) {
  morse_cwd_msg_t m;
  char esc2[2];
  esc2[0] = 0x1B;
  esc2[1] = '4';
  CHECK_INT(morse_cwd_parse(esc2, 2, &m), MORSE_OK);
  CHECK_INT(m.kind, MORSE_CWD_ABORT);
  esc2[1] = '0';
  CHECK_INT(morse_cwd_parse(esc2, 2, &m), MORSE_OK);
  CHECK_INT(m.kind, MORSE_CWD_RESET);
  esc2[1] = '5';
  CHECK_INT(morse_cwd_parse(esc2, 2, &m), MORSE_OK);
  CHECK_INT(m.kind, MORSE_CWD_EXIT);
  esc2[1] = 'z';
  CHECK_INT(morse_cwd_parse(esc2, 2, &m), MORSE_OK);
  CHECK_INT(m.kind, MORSE_CWD_UNKNOWN);
}

static void test_cwd_ptt(void) {
  morse_cwd_msg_t m;
  char s[4];
  s[0] = 0x1B;
  s[1] = 'a';
  s[2] = '1';
  CHECK_INT(morse_cwd_parse(s, 3, &m), MORSE_OK);
  CHECK_INT(m.kind, MORSE_CWD_PTT);
  CHECK_INT(m.ival, 1);
  s[2] = '0';
  CHECK_INT(morse_cwd_parse(s, 3, &m), MORSE_OK);
  CHECK_INT(m.ival, 0);
}

/* ---- WinKeyer builder --------------------------------------------------- */

static void test_winkeyer(void) {
  unsigned char b[16];
  size_t n;

  n = morse_winkeyer_open(b, sizeof(b));
  CHECK_INT((int)n, 2);
  CHECK_INT(b[0], 0x00);
  CHECK_INT(b[1], 0x02);

  n = morse_winkeyer_set_speed(b, sizeof(b), 25);
  CHECK_INT((int)n, 2);
  CHECK_INT(b[0], 0x02);
  CHECK_INT(b[1], 25);
  /* clamping */
  morse_winkeyer_set_speed(b, sizeof(b), 3);
  CHECK_INT(b[1], 5);
  morse_winkeyer_set_speed(b, sizeof(b), 250);
  CHECK_INT(b[1], 99);

  n = morse_winkeyer_set_sidetone(b, sizeof(b), 800); /* 4000/800 = 5 */
  CHECK_INT((int)n, 2);
  CHECK_INT(b[0], 0x01);
  CHECK_INT(b[1], 5);

  n = morse_winkeyer_close(b, sizeof(b));
  CHECK_INT(b[1], 0x03);

  n = morse_winkeyer_text(b, sizeof(b), "PARIS");
  CHECK_INT((int)n, 5);
  CHECK(memcmp(b, "PARIS", 5) == 0);
}

TEST_BEGIN("cw")
test_iambic_dit_hold();
test_iambic_dah_hold();
test_iambic_squeeze_alternates();
test_iambic_idle();
test_sender_single_dit();
test_sender_letter_a();
test_sender_counts_marks();
test_cwd_text();
test_cwd_speed_tone();
test_cwd_commands();
test_cwd_ptt();
test_winkeyer();
TEST_END()
