/**
 * Novus OS - virtual page allocator over the PMM + physmap (M1).
 *
 * There is no per-page mapping work in M1: mm_init() keeps a direct
 * physical map of all RAM at PHYSMAP_BASE (README 3.3), so a physical
 * page from the PMM is immediately usable at PHYSMAP_BASE + phys. The
 * VMM only wraps that conversion, zero-fills fresh pages and translates
 * back on free.
 */

#include "kernel.h"
#include "mm.h"
#include "vmm.h"
#include "pmm.h"

void *vmm_alloc_page(void) {
  void *phys = pmm_alloc_page();
  void *v;
  if (phys == 0) {
    return 0;
  }
  v = phys_to_virt((UINT64)phys);
  /* Clear the page before exposing it so callers never observe data left
   * by its previous owner; page tables and heap buffers rely on this.
   * Fresh page tables therefore begin with all entries non-present. */
  memset(v, 0, MM_PAGE_SIZE);
  return v;
}

void vmm_free_page(void *vaddr) {
  if (vaddr == 0) {
    return;
  }
  /* The PMM bitmap is indexed by physical page number, not by the
   * physmap address returned to callers, so undo the direct-map offset.
   * This recovers the physical page number that the allocator tracks. */
  pmm_free_page((void *)virt_to_phys(vaddr));
}
