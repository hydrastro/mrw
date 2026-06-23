/* test_timing.c - PARIS timing, Farnsworth spacing, and the weight knob. */
#include "morse/timing.h"
#include "test_util.h"

static void test_defaults(void) {
  morse_timing_t t;
  morse_timing_default(&t);
  /* Default is 20 wpm, no Farnsworth, 50% weight. */
  CHECK_NEAR(t.wpm, 20.0, 1e-9);
}

static void test_paris_standard(void) {
  /* At W wpm a dit is 1200/W ms. The canonical "PARIS " word is 50 units, so
   * one word lasts 60000/W ms. We check the unit duration directly and the
   * standard symbol multiples. */
  morse_timing_t t;
  morse_durations_t d;
  morse_timing_default(&t);
  t.wpm = 20.0;
  t.char_wpm = 0.0;
  t.weight = 0.5;
  CHECK_INT(morse_timing_resolve(&t, &d), MORSE_OK);
  CHECK_NEAR(d.dit_ms, 60.0, 1e-6);       /* 1200/20 */
  CHECK_NEAR(d.dah_ms, 180.0, 1e-6);      /* 3 units */
  CHECK_NEAR(d.intra_gap_ms, 60.0, 1e-6); /* 1 unit */
  CHECK_NEAR(d.char_gap_ms, 180.0, 1e-6); /* 3 units */
  CHECK_NEAR(d.word_gap_ms, 420.0, 1e-6); /* 7 units */

  /* 12 wpm => dit 100 ms. */
  t.wpm = 12.0;
  CHECK_INT(morse_timing_resolve(&t, &d), MORSE_OK);
  CHECK_NEAR(d.dit_ms, 100.0, 1e-6);
  CHECK_NEAR(d.word_gap_ms, 700.0, 1e-6);
}

static void test_duration_of(void) {
  morse_timing_t t;
  morse_durations_t d;
  morse_timing_default(&t);
  t.wpm = 20.0;
  morse_timing_resolve(&t, &d);
  CHECK_NEAR(morse_duration_of(&d, MORSE_SYM_DIT), 60.0, 1e-9);
  CHECK_NEAR(morse_duration_of(&d, MORSE_SYM_DAH), 180.0, 1e-9);
  CHECK_NEAR(morse_duration_of(&d, MORSE_SYM_INTRA_GAP), 60.0, 1e-9);
  CHECK_NEAR(morse_duration_of(&d, MORSE_SYM_CHAR_GAP), 180.0, 1e-9);
  CHECK_NEAR(morse_duration_of(&d, MORSE_SYM_WORD_GAP), 420.0, 1e-9);
}

static void test_farnsworth(void) {
  /* Character speed 20, overall 10: marks stay fast (dit from char speed) but
   * inter-character / word gaps stretch so the *overall* rate is 10 wpm. */
  morse_timing_t t;
  morse_durations_t fast, fw;
  morse_timing_default(&t);

  t.wpm = 20.0;
  t.char_wpm = 20.0;
  morse_timing_resolve(&t, &fast);

  t.wpm = 10.0;
  t.char_wpm = 20.0;
  CHECK_INT(morse_timing_resolve(&t, &fw), MORSE_OK);

  /* Mark/intra element durations come from the character speed (20 wpm). */
  CHECK_NEAR(fw.dit_ms, fast.dit_ms, 1e-6);
  CHECK_NEAR(fw.dah_ms, fast.dah_ms, 1e-6);
  /* Spacing must be strictly larger than the non-Farnsworth case. */
  CHECK(fw.char_gap_ms > fast.char_gap_ms);
  CHECK(fw.word_gap_ms > fast.word_gap_ms);
  /* ARRL ratio: word gap is 7/3 of the character gap. */
  CHECK_NEAR(fw.word_gap_ms / fw.char_gap_ms, 7.0 / 3.0, 1e-3);
}

static void test_weight(void) {
  /* Heavier weight lengthens the mark and shortens the following intra gap,
   * while keeping the dit+intra period constant. */
  morse_timing_t t;
  morse_durations_t even, heavy;
  morse_timing_default(&t);
  t.wpm = 20.0;
  t.char_wpm = 0.0;

  t.weight = 0.5;
  morse_timing_resolve(&t, &even);
  t.weight = 0.6;
  CHECK_INT(morse_timing_resolve(&t, &heavy), MORSE_OK);

  CHECK(heavy.dit_ms > even.dit_ms);
  CHECK(heavy.intra_gap_ms < even.intra_gap_ms);
  CHECK_NEAR(heavy.dit_ms + heavy.intra_gap_ms,
             even.dit_ms + even.intra_gap_ms, 1e-6);
}

static void test_clamps(void) {
  /* Out-of-range inputs clamp rather than explode. */
  morse_timing_t t;
  morse_durations_t d;
  morse_timing_default(&t);
  t.wpm = 0.0;     /* -> clamps up to a sane minimum */
  t.char_wpm = 0.0;
  t.weight = 5.0;  /* -> clamps into [0.25, 0.75] */
  CHECK_INT(morse_timing_resolve(&t, &d), MORSE_OK);
  CHECK(d.dit_ms > 0.0);
  CHECK(d.dah_ms > 0.0);
}

TEST_BEGIN("timing")
test_defaults();
test_paris_standard();
test_duration_of();
test_farnsworth();
test_weight();
test_clamps();
TEST_END()
