/* test_roundtrip.c - End-to-end text -> WAV file -> text, plus alloc balance. */
#include "morse/morse.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <process.h>
#define test_getpid _getpid
#else
#include <unistd.h>
#define test_getpid getpid
#endif

static void rt(const char *text, const char *name, double wpm, double tone) {
  morse_timing_t tm;
  morse_synth_opts_t so;
  char path[4096];
  const char *tmp = getenv("TMPDIR");
  int len = snprintf(path, sizeof(path), "%s/%s-%ld.wav", tmp ? tmp : ".",
                     name, (long)test_getpid());
  CHECK(len > 0 && (size_t)len < sizeof(path));
  if (len <= 0 || (size_t)len >= sizeof(path)) {
    return;
  }
  ds_str_t *out = ds_str_create(NULL);
  morse_timing_default(&tm);
  tm.wpm = wpm;
  morse_synth_opts_default(&so);
  so.tone_hz = tone;

  CHECK_INT(morse_text_to_wav(text, MORSE_VARIANT_INTERNATIONAL_EXT, &tm, &so,
                              path),
            MORSE_OK);
  CHECK_INT(morse_wav_to_text(path, MORSE_VARIANT_INTERNATIONAL_EXT, NULL, out),
            MORSE_OK);
  CHECK_STR(FUNC_str_cstr(out), text);
  ds_str_destroy(out);
  remove(path);
}

static void test_full_stack(void) {
  rt("HELLO WORLD", "mrt1", 20.0, 600.0);
  rt("CQ CQ DE K1ABC", "mrt2", 22.0, 650.0);
  rt("THE QUICK BROWN FOX 123", "mrt3", 18.0, 700.0);
  rt("PARIS CODEX", "mrt4", 25.0, 550.0);
}

static void test_version(void) {
  const char *v = morse_version_string();
  CHECK(v != NULL);
  CHECK_STR(v, "1.0.0");
}

static void test_alloc_balance(void) {
  morse_alloc_stats_t s;
  /* Diagnostics was enabled at the very start of main(); after all the round
   * trips above and their frees, the counted allocations must balance. */
  if (morse_diagnostics_get(&s)) {
    CHECK(s.allocations > 0);
    CHECK_INT(s.allocations, s.frees); /* no leaks in counted allocations */
    CHECK_INT(s.bytes_live, 0);
    CHECK_INT(s.failed_allocations, 0);
    CHECK(s.bytes_peak > 0);
    printf("  alloc stats: %zu allocs, %zu frees, peak %zu bytes\n",
           s.allocations, s.frees, s.bytes_peak);
  }
}

int main(void) {
  printf("[roundtrip]\n");
  morse_diagnostics_enable();
  test_version();
  test_full_stack();
  test_alloc_balance();
  printf("  %d checks, %d failure(s)\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
