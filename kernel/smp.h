/**
 * Novus OS - SMP wake-up of the B core (AP), M1 Task 6 (README 2.2
 * steps 5-6, spec 3.4 degradation rules).
 */

#ifndef NOVUS_SMP_H
#define NOVUS_SMP_H

#include "uefi.h"

/* Linear/physical address the AP trampoline blob is copied to; matches
 * .equ TRAMP_BASE in arch/x86_64/kernel/ap_trampoline.S and the PMM
 * reservation of page 0x8000 (kernel/pmm.c). SIPI vector 8 -> the AP
 * starts at vector << 12 = 0x8000. */
#define SMP_TRAMPOLINE_PHYS 0x8000ULL
#define SMP_SIPI_VECTOR     8

/* BSP-side wake-up: copy + patch the trampoline, run INIT-SIPI-SIPI
 * through the local APIC and wait (bounded) for the AP to publish
 * itself in shared memory. Never panics: every failure path prints a
 * warning and leaves the system running uniprocessor (spec 3.4). */
void smp_wake_ap(UINT64 kernel_cr3);

/* AP-side C entry, called by the trampoline in long mode on the AP's
 * own stack with the kernel CR3 already loaded. Publishes the serial
 * banner, bumps ap_online_count and stores ap_command = SLEEP, then
 * parks until the M4 scheduler takes over. */
void ap_main(void);

#endif /* NOVUS_SMP_H */
