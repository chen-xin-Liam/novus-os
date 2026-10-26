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

/* Flush and invalidate every level of the processor caches. Used once the
 * framebuffer is re-mapped uncacheable, so writes the firmware/early boot
 * made through a cacheable identity alias do not linger and confuse the
 * display's uncached reads of the same physical pages. */
static inline void cpu_wbinvd(void) {
  __asm__ volatile("wbinvd" : : : "memory");
}

static inline UINT8 io_in8(UINT16 port) {
  UINT8 v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
  return v;
}

static inline void io_out16(UINT16 port, UINT16 val) {
  __asm__ volatile("outw %0,%1" : : "a"(val), "Nd"(port));
}

static inline UINT16 io_in16(UINT16 port) {
  UINT16 v;
  __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port));
  return v;
}

static inline void io_out32(UINT16 port, UINT32 val) {
  __asm__ volatile("outl %0,%1" : : "a"(val), "Nd"(port));
}

static inline UINT32 io_in32(UINT16 port) {
  UINT32 v;
  __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port));
  return v;
}

/* Small settling delay for legacy port writes (PCI config, PIC, ...). */
static inline void io_wait(void) { io_out8(0x80u, 0u); }

#endif /* NOVUS_CPU_H */
