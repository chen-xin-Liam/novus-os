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
#include "acpi.h"
#include "lapic.h"
#include "idt.h"
#include <stddef.h>

/* IA32_APIC_BASE MSR (SDM Vol.4): base [35:12], global enable bit 11. */
#define LAPIC_MSR_APIC_BASE 0x1B
#define LAPIC_MSR_BASE_MASK 0xFFFFF000ULL
#define LAPIC_MSR_GLOBAL_EN (1ULL << 11)

/* Spurious-vector register: bits 7:0 vector, bit 8 software enable. */
#define LAPIC_REG_SPURIOUS  0x0F0
#define LAPIC_SPUR_VECTOR   0xFF
#define LAPIC_SPUR_ENABLE   (1u << 8)
#define LAPIC_REG_LVT_TIMER 0x320u
#define LAPIC_REG_TIMER_INIT 0x380u
#define LAPIC_REG_TIMER_CURRENT 0x390u
#define LAPIC_REG_TIMER_DIV 0x3E0u
#define LAPIC_TIMER_MASKED (1u << 16)
#define ACPI_FADT_PM_TMR 76u
#define ACPI_FADT_PM_TMR_LEN 91u
#define ACPI_FADT_FLAGS 112u
#define ACPI_FADT_X_PM_TMR 208u
#define ACPI_PM_TMR_EXT (1u << 8)
#define PM_TIMER_HZ 3579545u
#define PM_TIMER_TARGET_TICKS ((PM_TIMER_HZ * 10u) / 1000u)
#define PM_TIMER_SPIN_LIMIT 100000000u

static UINT64 g_lapic_base;

static UINT32 lapic_read_le32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) |
         ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static int pm_timer_port(const acpi_table_t *fadt, UINT16 *port,
                         UINT32 *mask) {
  const UINT8 *raw = (const UINT8 *)fadt;
  UINT32 flags;
  UINT32 address = 0;
  UINT8 width = 0;

  if (fadt->length < ACPI_FADT_FLAGS + 4u) {
    return 0;
  }
  flags = lapic_read_le32(raw + ACPI_FADT_FLAGS);
  if (fadt->length >= ACPI_FADT_X_PM_TMR + 12u) {
    const UINT8 *gas = raw + ACPI_FADT_X_PM_TMR;
    UINT64 extended_address = 0;
    memcpy(&extended_address, gas + 4, sizeof(extended_address));
    if (gas[0] == 1u && gas[2] == 0u && extended_address <= 0xFFFFu) {
      address = (UINT32)extended_address;
      width = gas[1];
    }
  }
  if (address == 0 && fadt->length > ACPI_FADT_PM_TMR_LEN) {
    address = lapic_read_le32(raw + ACPI_FADT_PM_TMR);
    width = (flags & ACPI_PM_TMR_EXT) != 0u ? 32u : 24u;
    if (raw[ACPI_FADT_PM_TMR_LEN] < 4u) {
      address = 0;
    }
  }
  if (address == 0 || address > 0xFFFCu || width < 24u) {
    return 0;
  }
  *port = (UINT16)address;
  *mask = width >= 32u || (flags & ACPI_PM_TMR_EXT) != 0u
              ? 0xFFFFFFFFu
              : 0x00FFFFFFu;
  return 1;
}

UINT32 lapic_timer_calibrate(UINT64 mmap, UINT64 size, UINT64 stride) {
  const acpi_table_t *fadt = acpi_find_table(mmap, size, stride, "FACP");
  UINT16 port;
  UINT32 mask;
  UINT32 start;
  UINT32 now = 0;
  UINT32 current;
  UINT32 elapsed;
  UINT32 spins;

  if (g_lapic_base == 0 || fadt == NULL ||
      !pm_timer_port(fadt, &port, &mask)) {
    return 0;
  }
  lapic_write(g_lapic_base, LAPIC_REG_TIMER_DIV, 0x3u);
  lapic_write(g_lapic_base, LAPIC_REG_LVT_TIMER,
              LAPIC_TIMER_MASKED | IDT_TIMER_VECTOR);
  lapic_write(g_lapic_base, LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);
  start = io_in32(port) & mask;
  for (spins = 0; spins < PM_TIMER_SPIN_LIMIT; spins++) {
    now = io_in32(port) & mask;
    if (((now - start) & mask) >= PM_TIMER_TARGET_TICKS) {
      break;
    }
    __asm__ volatile("pause");
  }
  current = lapic_read(g_lapic_base, LAPIC_REG_TIMER_CURRENT);
  lapic_write(g_lapic_base, LAPIC_REG_TIMER_INIT, 0u);
  if (spins == PM_TIMER_SPIN_LIMIT || current == 0xFFFFFFFFu) {
    return 0;
  }
  elapsed = 0xFFFFFFFFu - current;
  return elapsed > 0u ? elapsed : 0u;
}

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
