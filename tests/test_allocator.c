/* Exercise the DSA context through growing strings and allocation failures. */
#include "../src/morse_alloc.h"
#include "morse/morse.h"
#include "test_util.h"

TEST_BEGIN("DSA allocator");
{
  ds_context_t *context = morse_dsa_context();
  ds_str_t *text;
  char *block;
  char *resized;
  char expected[769];
  morse_alloc_stats_t stats;
  size_t i;

  morse_diagnostics_enable();
  text = ds_str_create(context);
  CHECK(text != NULL);
  if (text == NULL) return 1;
  /* Grow repeatedly through DSA, then release through the same context. */
  for (i = 0; i < 256; ++i) {
    CHECK_INT(ds_str_append_cstr(text, "CQ "), 0);
    memcpy(expected + 3 * i, "CQ ", 3);
  }
  expected[768] = '\0';
  CHECK_INT(strlen(FUNC_str_cstr(text)), 768);
  CHECK_STR(FUNC_str_cstr(text), expected);
  ds_str_destroy(text);

  block = (char *)ds_context_calloc(context, 8, 1);
  CHECK(block != NULL);
  if (block == NULL) return 1;
  CHECK_INT(block[0], 0);
  CHECK_INT(block[7], 0);
  memcpy(block, "PARIS", 6);
  resized = (char *)ds_context_realloc(context, block, 1024);
  CHECK(resized != NULL);
  if (resized == NULL) {
    ds_context_free(context, block);
    return 1;
  }
  block = resized;
  CHECK_STR(block, "PARIS");
  /* Failed growth must preserve the original allocation and its contents. */
  CHECK(ds_context_realloc(context, block, (size_t)-1) == NULL);
  CHECK_STR(block, "PARIS");
  CHECK(ds_context_alloc(context, (size_t)-1) == NULL);
  CHECK(ds_context_calloc(context, (size_t)-1, 2) == NULL);
  CHECK(ds_context_realloc(context, block, 0) == NULL);

  CHECK(morse_diagnostics_get(&stats));
  CHECK(stats.reallocations > 0);
  CHECK_INT(stats.allocations, stats.frees);
  CHECK_INT(stats.bytes_live, 0);
  CHECK_INT(stats.failed_allocations, 3);
}
TEST_END()
