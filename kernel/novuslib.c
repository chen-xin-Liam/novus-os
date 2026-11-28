/**
 * Novus OS - freestanding memory routines.
 *
 * Plain loop implementations; -fno-builtin keeps the compiler from
 * rewriting these into recursive builtin calls.
 */

#include "kernel.h"

/* Word-at-a-time, not byte-at-a-time (B-12). The byte loop this replaces
 * cost a store instruction per byte, and the single biggest consumer in the
 * system is a present: 1920x1080 into write-combining device memory. With
 * one store per byte the write-combining buffers never fill a whole line,
 * so every 8 bytes became its own bus transaction; on the EliteBook that
 * measured 1.34 s for one full-screen present. Eight bytes per store is the
 * minimum that lets the WC buffer do its job, and 4x unrolled keeps the
 * loop overhead off the stores. */
void *NOVUS_MEMORY_ABI memcpy(void *dst, const void *src, UINT64 n) {
  UINT8 *d = (UINT8 *)dst;
  const UINT8 *s = (const UINT8 *)src;

  /* Byte head: get both sides 8-byte aligned before the word loop. */
  while (n != 0u && (((UINTN)d | (UINTN)s) & 7u) != 0u) {
    *d++ = *s++;
    n--;
  }
  {
    UINT64       *dw = (UINT64 *)(void *)d;
    const UINT64 *sw = (const UINT64 *)(const void *)s;
    UINT64        words = n >> 3;

    while (words >= 4u) {
      dw[0] = sw[0];
      dw[1] = sw[1];
      dw[2] = sw[2];
      dw[3] = sw[3];
      dw += 4;
      sw += 4;
      words -= 4u;
    }
    while (words-- != 0u) {
      *dw++ = *sw++;
    }
    d = (UINT8 *)(void *)dw;
    s = (const UINT8 *)(const void *)sw;
    n &= 7u;
  }
  while (n-- != 0u) {
    *d++ = *s++;
  }
  return dst;
}

void *NOVUS_MEMORY_ABI memset(void *dst, int c, UINT64 n) {
  UINT8 *d = (UINT8 *)dst;
  UINT64 v;
  UINT64 words;

  while (n != 0u && ((UINTN)d & 7u) != 0u) {
    *d++ = (UINT8)c;
    n--;
  }
  v = (UINT64)(UINT8)c;
  v |= v << 8;
  v |= v << 16;
  v |= v << 32;
  {
    UINT64 *dw = (UINT64 *)(void *)d;

    words = n >> 3;
    while (words >= 4u) {
      dw[0] = v;
      dw[1] = v;
      dw[2] = v;
      dw[3] = v;
      dw += 4;
      words -= 4u;
    }
    while (words-- != 0u) {
      *dw++ = v;
    }
    d = (UINT8 *)(void *)dw;
    n &= 7u;
  }
  while (n-- != 0u) {
    *d++ = (UINT8)c;
  }
  return dst;
}

int NOVUS_MEMORY_ABI memcmp(const void *a, const void *b, UINT64 n) {
  const UINT8 *x = (const UINT8 *)a;
  const UINT8 *y = (const UINT8 *)b;
  while (n--) {
    if (*x != *y) {
      return (*x < *y) ? -1 : 1;
    }
    x++;
    y++;
  }
  return 0;
}
