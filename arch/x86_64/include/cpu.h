/**
 * Novus OS - x86_64 CPU primitives (CPUID, port I/O).
 */

#ifndef NOVUS_CPU_H
#define NOVUS_CPU_H

#include "uefi.h"

/* Query CPUID with an explicit subleaf. Any output pointer may be NULL
 * and is skipped in that case. */
static inline void cpuid(UINT32 leaf, UINT32 subleaf, UINT32 *a, UINT32 *b,
                         UINT32 *c, UINT32 *d) {
  UINT32 ra, rb, rc, rd;
  __asm__ volatile("cpuid"
                   : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                   : "a"(leaf), "c"(subleaf)
                   : "memory");
  if (a) { *a = ra; }
  if (b) { *b = rb; }
  if (c) { *c = rc; }
  if (d) { *d = rd; }
}

static inline void io_out8(UINT16 port, UINT8 val) {
  __asm__ volatile("outb %0,%1" : : "a"(val), "Nd"(port));
}

static inline UINT8 io_in8(UINT16 port) {
  UINT8 v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
  return v;
}

#endif /* NOVUS_CPU_H */
