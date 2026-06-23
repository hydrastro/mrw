/*
 * test_util.h - A tiny zero-dependency assertion harness.
 *
 * Each test executable includes this, defines a handful of test functions, and
 * uses the CHECK_* macros. main() prints a per-file summary and returns nonzero
 * if any check failed, which is exactly what ctest keys off.
 */
#ifndef MORSE_TEST_UTIL_H
#define MORSE_TEST_UTIL_H

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_fails++;                                                               \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
    }                                                                          \
  } while (0)

#define CHECK_INT(a, b)                                                        \
  do {                                                                         \
    long _a = (long)(a), _b = (long)(b);                                       \
    g_checks++;                                                                \
    if (_a != _b) {                                                            \
      g_fails++;                                                               \
      printf("  FAIL %s:%d: %s == %s (%ld vs %ld)\n", __FILE__, __LINE__, #a,  \
             #b, _a, _b);                                                      \
    }                                                                          \
  } while (0)

#define CHECK_STR(a, b)                                                        \
  do {                                                                         \
    const char *_a = (a), *_b = (b);                                           \
    g_checks++;                                                                \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {                     \
      g_fails++;                                                               \
      printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,           \
             _a ? _a : "(null)", _b ? _b : "(null)");                          \
    }                                                                          \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                  \
  do {                                                                         \
    double _a = (a), _b = (b);                                                 \
    g_checks++;                                                                \
    if (fabs(_a - _b) > (eps)) {                                              \
      g_fails++;                                                               \
      printf("  FAIL %s:%d: %s ~= %s (%.6f vs %.6f, eps %.6f)\n", __FILE__,    \
             __LINE__, #a, #b, _a, _b, (double)(eps));                         \
    }                                                                          \
  } while (0)

#define TEST_BEGIN(name)                                                       \
  int main(void) {                                                             \
    printf("[%s]\n", name);

#define TEST_END()                                                             \
  printf("  %d checks, %d failure(s)\n", g_checks, g_fails);                   \
  return g_fails ? 1 : 0;                                                      \
  }

#endif /* MORSE_TEST_UTIL_H */
