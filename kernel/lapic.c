/**
 * Novus OS - local APIC (xAPIC mode) bring-up, M1 Task 6.
 *
 * Detection and enabling happen at two levels:
 *   - CPUID leaf 1 EDX bit 9 gates the whole feature;
 *   - IA32_APIC_BASE MSR (0x1B): bits [35:12] hold the 4 KiB-aligned
 *     MMIO base, bit 11 is the APIC global enable (set and write back
 *     when clear);
 *   - spurious-vector register (offset 0xF0) bit 8 is the software
 *     enable required for ICR writes to actually send IPIs; the
 *     spurious vector 0xFF never fires because interrupts stay masked.
 *
 * xAPIC mode keeps the register file in a 4 KiB MMIO page reached
 * through the low-4GiB identity map (mm.c). On QEMU and on essentially
 * all real hardware that page sits at 0xFEE00000. x2APIC is out of M1
 * scope; smp.c drives the APIC ID 1 through the MMIO ICR.
 */

#include "kernel.h"
#include "cpu.h"
#include "lapic.h"

/* IA32_APIC_BASE MSR (SDM Vol.4): base [35:12], global enable bit 11. */
#define LAPIC_MSR_APIC_BASE 0x1B
#define LAPIC_MSR_BASE_MASK 0xFFFFF000ULL
#define LAPIC_MSR_GLOBAL_EN (1ULL << 11)

/* Spurious-vector register: bits 7:0 vector, bit 8 software enable. */
#define LAPIC_REG_SPURIOUS  0x0F0
#define LAPIC_SPUR_VECTOR   0xFF
#define LAPIC_SPUR_ENABLE   (1u << 8)

static UINT64 g_lapic_base;

/* RDMSR/WRMSR: ECX selects the MSR, the value travels in EDX:EAX. */
static UINT64 lapic_rdmsr(UINT32 msr) {
  UINT32 lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((UINT64)hi << 32) | lo;
}

static void lapic_wrmsr(UINT32 msr, UINT64 value) {
  __asm__ volatile("wrmsr"
                   :
                   : "c"(msr), "a"((UINT32)value), "d"((UINT32)(value >> 32)));
}

UINT32 lapic_read(UINT64 base, UINT32 offset) {
  return *(volatile UINT32 *)(UINTN)(base + offset);
}

void lapic_write(UINT64 base, UINT32 offset, UINT32 value) {
  /* xAPIC register stores are in-order enough for IPI sending; the
   * volatile write is the whole story (no read side effects here). */
  *(volatile UINT32 *)(UINTN)(base + offset) = value;
}

UINT64 lapic_base(void) {
  return g_lapic_base;
}

UINT64 lapic_init(void) {
  UINT32 a, b, c, d;
  UINT64 msr;

  /* CPUID leaf 1, EDX bit 9: local APIC present. */
  cpuid(1, 0, &a, &b, &c, &d);
  if ((d & (1u << 9)) == 0) {
    kputs("[SMP] WARNING: local APIC not supported\n");
    g_lapic_base = 0;
    return 0;
  }

  msr = lapic_rdmsr(LAPIC_MSR_APIC_BASE);
  if ((msr & LAPIC_MSR_GLOBAL_EN) == 0) {
    msr |= LAPIC_MSR_GLOBAL_EN;
    lapic_wrmsr(LAPIC_MSR_APIC_BASE, msr);
  }
  g_lapic_base = msr & LAPIC_MSR_BASE_MASK;

  /* Software enable through the spurious-vector register so that ICR
   * writes send IPIs (the MSR global enable alone is not sufficient). */
  lapic_write(g_lapic_base, LAPIC_REG_SPURIOUS,
              lapic_read(g_lapic_base, LAPIC_REG_SPURIOUS) |
                  LAPIC_SPUR_ENABLE | LAPIC_SPUR_VECTOR);

  kputs("[SMP] LAPIC at ");
  kputs_hex(g_lapic_base);
  kputs("\n");
  return g_lapic_base;
}
