/**
 * Novus OS - freestanding memory routines.
 *
 * Plain loop implementations; -fno-builtin keeps the compiler from
 * rewriting these into recursive builtin calls.
 */

#include "kernel.h"

void *memcpy(void *dst, const void *src, UINT64 n) {
  UINT8 *d = (UINT8 *)dst;
  const UINT8 *s = (const UINT8 *)src;
  while (n--) {
    *d++ = *s++;
  }
  return dst;
}

void *memset(void *dst, int c, UINT64 n) {
  UINT8 *d = (UINT8 *)dst;
  while (n--) {
    *d++ = (UINT8)c;
  }
  return dst;
}

int memcmp(const void *a, const void *b, UINT64 n) {
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
