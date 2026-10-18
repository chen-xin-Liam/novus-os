/**
 * Novus OS - x86_64 paging: table build, CR3 switch, selftest (README 3.3).
 *
 * mm_init() builds a fresh page-table hierarchy out of pages handed out
 * by the PMM, then switches CR3. Two windows exist afterwards:
 *
 *   - PML4[0]  : identity map of the whole low 4 GiB. This covers the
 *                kernel image, the VGA text buffer at 0xB8000, the LAPIC
 *                MMIO area at 0xFEE00000, the AP trampoline at 0x8000,
 *                the shared-memory block at 0x7E000 and the AP stacks.
 *                The identity map is complete BEFORE the CR3 switch, so
 *                the switch instruction, this function's code and the
 *                kernel stack stay mapped on both paths at all times.
 *   - PML4[256]: direct physical map at PHYSMAP_BASE covering all RAM in
 *                [0, ram_top). The VMM and heap allocate through it
 *                without any per-page mapping work.
 *
 * Everything is built from 2 MiB huge pages: a PML4/PDPT entry points at
 * the next table, each PD covers one 1 GiB slice with 512 x 2 MiB leaf
 * entries (PS bit set). The whole M1 tree is therefore 1 PML4 + 6 PDPTs
 * + N PDs and no 4 KiB page-splitting logic exists yet.
 *
 * Table pages come from pmm_alloc_page(). While the tables are built the
 * firmware identity map is still active, so physical addresses can be
 * written directly. CR3 PCD/PWT are kept clear and every table base is
 * 4 KiB aligned because pmm_alloc_page only returns whole pages.
 */

#include "kernel.h"
#include "mm.h"
#include "pmm.h"

/* PTE flag bits (x86_64 SDM Vol.3, chapter 4). */
#define MM_PTE_PRESENT     0x0000000000000001ULL
#define MM_PTE_WRITE       0x0000000000000002ULL
#define MM_PTE_HUGE        0x0000000000000080ULL /* PS: leaf at PDE level */

/* Address fields. Table pointers carry bits [51:12], 2 MiB leaf entries
 * carry bits [51:21] (bits 20:12 must be zero there or #PF fires). */
#define MM_PTE_ADDR4K_MASK 0x000FFFFFFFFFF000ULL
#define MM_PTE_ADDR2M_MASK 0x000FFFFFFFE00000ULL

/* One page table / directory: 512 entries x 8 bytes = one 4 KiB page. */
typedef struct {
  UINT64 e[512];
} mm_table_t;

static void mm_write_cr3(UINT64 table_phys) {
  /* PCD/PWT (bits 4/3) stay clear so table walks are cacheable; the base
   * is 4 KiB aligned by construction (pmm_alloc_page). */
  __asm__ volatile("movq %0, %%cr3" : : "r"(table_phys & ~0xFFFULL) : "memory");
}

UINT64 mm_get_cr3(void) {
  UINT64 v;
  __asm__ volatile("movq %%cr3, %0" : "=r"(v));
  return v;
}

/* The hierarchy mm_init() built, captured before any process space exists.
 * Every process space is cloned from *this* one. Cloning "whatever CR3 is
 * loaded right now" instead inherits the running task's private tables -
 * a spawned program would then share its parent's page tables, the parent
 * would see the child's mappings, and whichever of them exited first would
 * free the other's code pages (M8 "closing a window kills the desktop"). */
static UINT64 g_kernel_cr3;

void mm_capture_kernel_cr3(void) { g_kernel_cr3 = mm_get_cr3() & ~0xFFFULL; }

UINT64 mm_kernel_cr3(void) {
  return (g_kernel_cr3 != 0) ? g_kernel_cr3 : (mm_get_cr3() & ~0xFFFULL);
}

void *phys_to_virt(UINT64 phys) {
  return (void *)(PHYSMAP_BASE + phys);
}

UINT64 virt_to_phys(const void *v) {
  UINT64 a = (UINT64)v;
  if (a >= PHYSMAP_BASE) {
    return a - PHYSMAP_BASE;
  }
  /* Kernel image and everything else below PHYSMAP_BASE is identity-
   * mapped: the run address equals the physical address. */
  return a;
}

/* Allocate and zero one page-table page. Runs while the firmware identity
 * map is still active, so the returned physical address is written to
 * directly. Returns NULL (with a console error) when the PMM is dry. */
static mm_table_t *mm_table_new(UINT64 *phys_out) {
  void *p = pmm_alloc_page();
  if (p == 0) {
    kputs("[MM] ERROR: out of pages for page tables\n");
    return 0;
  }
  memset(p, 0, MM_PAGE_SIZE);
  *phys_out = (UINT64)p;
  return (mm_table_t *)p;
}

/* Fill one page directory with 512 x 2 MiB Present|Write|PS leaf entries
 * covering [phys_base, phys_base + 1 GiB). */
static void mm_fill_pd_2m(mm_table_t *pd, UINT64 phys_base) {
  UINT32 i;
  for (i = 0; i < 512; i++) {
    pd->e[i] = MM_PTE_PRESENT | MM_PTE_WRITE | MM_PTE_HUGE |
               ((phys_base + ((UINT64)i << 21)) & MM_PTE_ADDR2M_MASK);
  }
}

void mm_init(UINT64 ram_top_bytes) {
  UINT64 pml4_phys, pdpt_phys;
  mm_table_t *pml4, *pdpt;
  UINT32 i;

  pml4 = mm_table_new(&pml4_phys);
  if (pml4 == 0) {
    return;
  }

  /* PML4[0]: identity map of the low 4 GiB = 4 PDPT slices, each backed
   * by one PD of 512 x 2 MiB entries. */
  pdpt = mm_table_new(&pdpt_phys);
  if (pdpt == 0) {
    return;
  }
  pml4->e[0] = MM_PTE_PRESENT | MM_PTE_WRITE | (pdpt_phys & MM_PTE_ADDR4K_MASK);
  for (i = 0; i < 4; i++) {
    UINT64 pd_phys;
    mm_table_t *pd = mm_table_new(&pd_phys);
    if (pd == 0) {
      return;
    }
    pdpt->e[i] = MM_PTE_PRESENT | MM_PTE_WRITE | (pd_phys & MM_PTE_ADDR4K_MASK);
    mm_fill_pd_2m(pd, (UINT64)i << 30);
  }

  /* PML4[256]: direct physical map of [0, ram_top). One PD per 1 GiB
   * slice, so ram_top is rounded up to a whole number of 2 MiB pages
   * (PDs are the mapping unit here). */
  pdpt = mm_table_new(&pdpt_phys);
  if (pdpt == 0) {
    return;
  }
  pml4->e[256] = MM_PTE_PRESENT | MM_PTE_WRITE | (pdpt_phys & MM_PTE_ADDR4K_MASK);
  {
    UINT64 n_pd = (ram_top_bytes + 0x3FFFFFFFULL) >> 30; /* ceil to 1 GiB */
    UINT64 gi;
    for (gi = 0; gi < n_pd; gi++) {
      UINT64 pd_phys;
      mm_table_t *pd = mm_table_new(&pd_phys);
      if (pd == 0) {
        return;
      }
      pdpt->e[gi] = MM_PTE_PRESENT | MM_PTE_WRITE | (pd_phys & MM_PTE_ADDR4K_MASK);
      mm_fill_pd_2m(pd, gi << 30);
    }
  }

  /* Switch CR3. The new tables identity-map the full low 4 GiB, so this
   * function's code and the kernel stack stay reachable on both the
   * identity path and the new physmap path. */
  mm_write_cr3(pml4_phys);
  mm_capture_kernel_cr3();

  kputs("[MM] paging enabled, CR3=");
  kputs_hex(mm_get_cr3());
  kputs("\n");

  mm_selftest();
}

void mm_selftest(void) {
  /* Probe a .text byte via a kernel function address (the PE image has
   * no exported start symbol) and read it back through both windows. */
  const UINT8 *ident = (const UINT8 *)(UINT64)&mm_init;
  UINT64 phys = virt_to_phys(ident);
  const UINT8 *mapped = (const UINT8 *)phys_to_virt(phys);

  if (*ident == *mapped) {
    kputs("[MM] physmap readback PASS\n");
  } else {
    kputs("[MM] physmap readback FAIL\n");
  }
}
