/**
 * Novus OS - virtual page allocator over the PMM + physmap (M1).
 */

#ifndef NOVUS_VMM_H
#define NOVUS_VMM_H

#include "uefi.h"

/* Allocate one fresh zeroed page and return its virtual address in the
 * physmap window (PHYSMAP_BASE + phys); returns NULL when the PMM is
 * exhausted. */
void *vmm_alloc_page(void);

/* Release one page previously obtained from vmm_alloc_page. */
void vmm_free_page(void *vaddr);

#endif /* NOVUS_VMM_H */
