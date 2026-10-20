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

/* BSP-side wake-up: enumerate MADT processor IDs, then copy + patch the
 * trampoline and run INIT-SIPI-SIPI for every enabled AP. Each AP gets
 * its own stack and a bounded wait; failures degrade to fewer CPUs. */
void smp_wake_ap(UINT64 kernel_cr3, UINT64 mmap, UINT64 map_size,
				 UINT64 desc_size);

/* AP-side C entry, called by the trampoline in long mode on the AP's
 * own stack with the kernel CR3 already loaded. Publishes the serial
 * banner, bumps ap_online_count and stores ap_command = SLEEP, then
 * parks until the M4 scheduler takes over. */
void ap_main(void);

#endif /* NOVUS_SMP_H */
