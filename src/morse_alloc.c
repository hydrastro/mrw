/*
 * morse_alloc.c - Implementation of the core's allocation primitives.
 *
 * Each block carries a header large enough to hold its payload size while
 * preserving max_align_t alignment for the returned pointer. The header lets
 * free()/realloc() know the old size so live-byte accounting stays exact. The
 * counters are process-wide and thread-unsafe by design: the CLI and GUI here
 * are single-threaded, and the panel is purely observational.
 */
#include "morse_alloc.h"

#include <stdlib.h>
#include <string.h>

/* A header that holds the payload size yet keeps the following bytes aligned
 * for any scalar type. Using max_align_t as a union member forces the union's
 * alignment (and hence size, rounded up) to the platform maximum. */
typedef union morse_block_header {
  size_t size;
  max_align_t align;
} morse_block_header_t;

#define MORSE_HDR (sizeof(morse_block_header_t))

static int g_diag_enabled = 0;
static morse_alloc_counters_t g_counters;

void morse_alloc_enable_diagnostics(void) {
  g_diag_enabled = 1;
  memset(&g_counters, 0, sizeof(g_counters));
}

int morse_alloc_diagnostics_enabled(void) { return g_diag_enabled; }

void morse_alloc_get_counters(morse_alloc_counters_t *out) {
  if (out != NULL) {
    *out = g_counters;
  }
}

static void note_alloc(size_t n) {
  if (!g_diag_enabled) {
    return;
  }
  g_counters.allocations++;
  g_counters.bytes_total += n;
  g_counters.bytes_live += n;
  if (g_counters.bytes_live > g_counters.bytes_peak) {
    g_counters.bytes_peak = g_counters.bytes_live;
  }
}

static void note_free(size_t n) {
  if (!g_diag_enabled) {
    return;
  }
  g_counters.frees++;
  if (g_counters.bytes_live >= n) {
    g_counters.bytes_live -= n;
  } else {
    g_counters.bytes_live = 0;
  }
}

static void note_failed(void) {
  if (g_diag_enabled) {
    g_counters.failed_allocations++;
  }
}

void *morse_xmalloc(size_t size) {
  unsigned char *base = (unsigned char *)malloc(MORSE_HDR + size);
  if (base == NULL) {
    note_failed();
    return NULL;
  }
  ((morse_block_header_t *)base)->size = size;
  note_alloc(size);
  return base + MORSE_HDR;
}

void *morse_xcalloc(size_t count, size_t size) {
  size_t n;
  void *p;
  /* Guard the multiply against overflow. */
  if (count != 0 && size > ((size_t)-1) / count) {
    note_failed();
    return NULL;
  }
  n = count * size;
  p = morse_xmalloc(n);
  if (p != NULL) {
    memset(p, 0, n);
  }
  return p;
}

void *morse_xrealloc(void *ptr, size_t size) {
  unsigned char *base;
  unsigned char *grown;
  size_t old;

  if (ptr == NULL) {
    return morse_xmalloc(size);
  }
  base = (unsigned char *)ptr - MORSE_HDR;
  old = ((morse_block_header_t *)base)->size;

  grown = (unsigned char *)realloc(base, MORSE_HDR + size);
  if (grown == NULL) {
    note_failed();
    return NULL; /* original block left intact, per realloc contract */
  }
  ((morse_block_header_t *)grown)->size = size;

  if (g_diag_enabled) {
    g_counters.reallocations++;
    if (size >= old) {
      size_t d = size - old;
      g_counters.bytes_total += d;
      g_counters.bytes_live += d;
      if (g_counters.bytes_live > g_counters.bytes_peak) {
        g_counters.bytes_peak = g_counters.bytes_live;
      }
    } else {
      size_t d = old - size;
      if (g_counters.bytes_live >= d) {
        g_counters.bytes_live -= d;
      } else {
        g_counters.bytes_live = 0;
      }
    }
  }
  return grown + MORSE_HDR;
}

void morse_xfree(void *ptr) {
  unsigned char *base;
  size_t n;
  if (ptr == NULL) {
    return;
  }
  base = (unsigned char *)ptr - MORSE_HDR;
  n = ((morse_block_header_t *)base)->size;
  note_free(n);
  free(base);
}
