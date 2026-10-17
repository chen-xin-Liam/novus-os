/**
 * Novus OS - local APIC (xAPIC mode) bring-up, M1 Task 6.
 *
 * lapic_init() detects the local APIC (CPUID leaf 1 EDX bit 9), enables
 * it at both levels (IA32_APIC_BASE MSR global-enable bit 11 and the
 * spurious-vector register software-enable bit 8) and returns the 4 KiB
 * MMIO base. On QEMU and on essentially all real hardware that page
 * sits at 0xFEE00000. IPI traffic (smp.c) goes through lapic_read/
 * lapic_write; lapic_base() hands the cached base to other modules.
 */

#ifndef NOVUS_LAPIC_H
#define NOVUS_LAPIC_H

#include "uefi.h"

/* Detect and enable the local APIC; returns the page-aligned LAPIC MMIO
 * base, or 0 when the CPU has no local APIC (CPUID leaf 1 EDX bit 9
 * clear). Must run before any lapic_read/lapic_write/lapic_base call. */
UINT64 lapic_init(void);

/* LAPIC MMIO base cached by lapic_init(); 0 before init / no APIC. */
UINT64 lapic_base(void);

/* xAPIC MMIO register access: offset into the 4 KiB LAPIC page
 * (e.g. ICR low = 0x300, ICR high = 0x310, spurious = 0x0F0). */
UINT32 lapic_read(UINT64 base, UINT32 offset);
void lapic_write(UINT64 base, UINT32 offset, UINT32 value);

#endif /* NOVUS_LAPIC_H */
