/*
 * morse_alloc.h - Private allocation primitives for the morse core.
 *
 * NOT part of the installed public API. Every heap allocation the core makes -
 * both its own buffers and, via the ds *_create_alloc constructors, the nodes
 * of the ds containers it builds - goes through these. By default they are thin
 * wrappers over malloc/free and add only a small size-tracking header. When
 * morse_diagnostics_enable() has been called they additionally tally live/peak
 * bytes and allocation counts, which the GUI's diagnostics panel surfaces. The
 * header is always present so that enabling diagnostics never changes the
 * pointer layout, keeping alloc/free symmetric regardless of mode.
 *
 * These signatures match what ds's *_create_alloc constructors expect
 * (void *(*)(size_t) and void (*)(void *)), so morse_xmalloc / morse_xfree can
 * be handed to ds_list_create_alloc, ds_queue_create_alloc, etc. directly.
 */
#ifndef MORSE_ALLOC_PRIVATE_H
#define MORSE_ALLOC_PRIVATE_H

#include <stddef.h>

void *morse_xmalloc(size_t size);
void *morse_xcalloc(size_t count, size_t size);
void *morse_xrealloc(void *ptr, size_t size);
void morse_xfree(void *ptr);

/* Internal: reset/read the process-wide counters (used by morse.c). */
void morse_alloc_enable_diagnostics(void);
int morse_alloc_diagnostics_enabled(void);

typedef struct morse_alloc_counters {
  size_t allocations;
  size_t frees;
  size_t reallocations;
  size_t failed_allocations;
  size_t bytes_live;
  size_t bytes_peak;
  size_t bytes_total;
} morse_alloc_counters_t;

void morse_alloc_get_counters(morse_alloc_counters_t *out);

#endif /* MORSE_ALLOC_PRIVATE_H */
