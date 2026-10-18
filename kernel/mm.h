/**
 * Novus OS - x86_64 paging setup and direct physical map (README 3.3).
 */

#ifndef NOVUS_MM_H
#define NOVUS_MM_H

#include "uefi.h"

/* Direct physical map window: after mm_init() every RAM address in
 * [0, ram_top) is reachable at PHYSMAP_BASE + phys through PML4[256],
 * built from 2 MiB huge pages (README 3.3 virtual memory & protection). */
#define PHYSMAP_BASE 0xFFFF800000000000ULL

/* Page granularity used across the whole memory subsystem. */
#define MM_PAGE_SIZE 4096ULL

/* Build the page-table hierarchy (identity map of the low 4 GiB through
 * PML4[0] plus the direct physical map of [0, ram_top_bytes) through
 * PML4[256]), switch CR3 and run the physmap read-back selftest. */
void mm_init(UINT64 ram_top_bytes);

/* Current CR3 value, read with inline assembly. */
UINT64 mm_get_cr3(void);

/* The kernel's own PML4, saved by mm_init(): the template every process
 * space is cloned from. Callers must not use mm_get_cr3() for that. */
UINT64 mm_kernel_cr3(void);

/* Direct-map conversions. phys_to_virt maps into the physmap window;
 * virt_to_phys reverses it and passes identity-mapped low addresses
 * (kernel image) through unchanged. */
void *phys_to_virt(UINT64 phys);
UINT64 virt_to_phys(const void *v);

/* Read the same physical byte (kernel .text region) through the identity
 * map and through PHYSMAP_BASE and report PASS/FAIL on the console. */
void mm_selftest(void);

#endif /* NOVUS_MM_H */
