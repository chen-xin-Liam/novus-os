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
 *                [0, ram_top). The VMM allocates single pages through it.
 *   - PML4[258]: shared 4 KiB heap mappings, contiguous in virtual space.
 *
 * The identity map and physmap use 2 MiB huge pages: a PML4/PDPT entry points at
 * the next table, each PD covers one 1 GiB slice with 512 x 2 MiB leaf
 * entries (PS bit set). The whole M1 tree is therefore 1 PML4 + 6 PDPTs
 * + N PDs. The separate heap window does not split either huge-page map.
 *
 * Table pages come from pmm_alloc_page(). While the tables are built the
 * firmware identity map is still active, so physical addresses can be
 * written directly. CR3 PCD/PWT are kept clear and every table base is
 * 4 KiB aligned because pmm_alloc_page only returns whole pages.
 */

#include "kernel.h"
#include "mm.h"
#include "pmm.h"
#include "msr.h"

/* PTE flag bits (x86_64 SDM Vol.3, chapter 4). */
#define MM_PTE_PRESENT     0x0000000000000001ULL
#define MM_PTE_WRITE       0x0000000000000002ULL
#define MM_PTE_HUGE        0x0000000000000080ULL /* PS: leaf at PDE level */
#define MM_PTE_PCD         0x0000000000000010ULL /* cache disable (device) */
#define MM_PTE_PAT         0x0000000000001000ULL /* selects IA32_PAT entry 4 */

/* Address fields. Table pointers carry bits [51:12], 2 MiB leaf entries
 * carry bits [51:21] (bits 20:12 must be zero there or #PF fires). */
#define MM_PTE_ADDR4K_MASK 0x000FFFFFFFFFF000ULL
#define MM_PTE_ADDR2M_MASK 0x000FFFFFFFE00000ULL

/* The PAT layout this kernel needs: entries 0 WB, 1 WT, 2 UC, 3 UC-, 4 WC,
 * 5 WT, 6 UC, 7 UC-. The reset value of IA32_PAT (0x0007040600070406) has
 * entry 4 as WB, which is the one bit that matters here: MM_PTE_PAT on a
 * leaf selects entry 4, so leaving the reset value in place makes every
 * "write-combining" framebuffer mapping write-back instead - the pixels
 * land in cache and the panel never sees them. Firmware that programs PAT
 * at all programs it as the reset value, so the fix has to be explicit
 * rather than conditional on the MSR differing from a default. */
#define IA32_PAT_MSR      0x00000277u
#define IA32_PAT_WC_PA4   0x0007040100070406ULL /* PA4 = 01: WC */
#define CR4_PAT           0x0000000000100000ULL
#define CR4_LA57          0x0000000000001000ULL

/* 0 until CPUID says this CPU has PAT and CR4/IA32_PAT have taken the write.
 * The PTE PAT bit is a reserved bit on a CPU without PAT, so "no PAT" has to
 * mean "never set that bit", not "set it and hope": a write-combining request
 * degrades to uncached instead of faulting. */
static int g_pat_ok;

static int mm_cpu_has_pat(void) {
  UINT32 a, b, c, d;
  UINT32 leaf = 1u;

  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(leaf)
                   : "memory");
  /* SDM CPUID.01H:EDX[16] = PAT (page attribute table). */
  return ((d & (1u << 16)) != 0u) ? 1 : 0;
}

int mm_pat_available(void) { return g_pat_ok; }

/* One page table / directory: 512 entries x 8 bytes = one 4 KiB page. */
typedef struct {
  UINT64 e[512];
} mm_table_t;

/* Install the entire hierarchy before vm_space_create() clones the PML4.
 * Static tables avoid allocating page-table pages in a low-memory rollback. */
static mm_table_t g_heap_pdpt __attribute__((aligned(4096)));
static mm_table_t g_heap_pd __attribute__((aligned(4096)));
static mm_table_t g_heap_pt[MM_HEAP_SIZE / (2 * 1024 * 1024)]
    __attribute__((aligned(4096)));

#ifndef MM_INVALIDATE
#define MM_INVALIDATE(address) \
  __asm__ volatile("invlpg (%0)" : : "r"((UINT64)(address)) : "memory")
#endif

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
  if (a >= MM_HEAP_BASE && a < MM_HEAP_BASE + MM_HEAP_SIZE) {
    UINT64 page = (a - MM_HEAP_BASE) / MM_PAGE_SIZE;
    UINT64 entry = g_heap_pt[page / 512].e[page % 512];
    if ((entry & MM_PTE_PRESENT) == 0) {
      kputs("[MM] ERROR: translating unmapped heap address\n");
      return 0;
    }
    return (entry & MM_PTE_ADDR4K_MASK) | (a & (MM_PAGE_SIZE - 1));
  }
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

void mm_init(UINT64 ram_top_bytes, UINT64 fb_phys, UINT64 fb_size) {
  UINT64 pml4_phys, pdpt_phys, cr4;
  mm_table_t *pml4, *pdpt;
  UINT32 i;
  UINT64 fb_bits;

  /* 5-level paging: our tables are 4-level, so loading one of them while
   * CR4.LA57 is set makes every walk interpret a PML5 whose entries we do
   * not own - the first instruction after the CR3 write faults. There is
   * no cheap recovery (the CR3 format itself differs), so say so out loud
   * and keep the firmware tables instead of dying silently. */
  __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
  if ((cr4 & CR4_LA57) != 0u) {
    kputs("[MM] FATAL: 5-level paging is active (CR4.LA57 set); the kernel's "
          "4-level tables cannot be loaded, keeping the firmware tables\n");
    return;
  }

  /* Before any table is built: the PTE PAT bit is only legal once CR4.PAT
   * is set, and it only means "write-combining" when IA32_PAT entry 4 says
   * so. */
  mm_enable_pat();

  /* Framebuffer cache bits: PAT entry 4 when we have it, PCD otherwise.
   * A framebuffer mapped write-back never reaches the panel. */
  fb_bits = (g_pat_ok != 0) ? MM_PTE_PAT : MM_PTE_PCD;

  kputs("[MM] building page tables\n");

  pml4 = mm_table_new(&pml4_phys);
  if (pml4 == 0) {
    return;
  }

  /* PML4[0]: identity map of the whole of physical RAM, [0, ram_top).
   *
   * This used to stop at 4 GiB, on the theory that the kernel image and its
   * stack live low and everything else reaches memory through the physmap
   * window. That theory is wrong on a machine with enough RAM: the boot
   * stage allocates the kernel stack, the GDT/TSS and the IDT through the
   * PMM, and the PMM hands out the first conventional pages it finds - which
   * on this laptop is 0x2B62E5040, 11.6 GiB up. Every pointer in the handoff
   * block is a raw physical address, so the moment CR3 flips to tables that
   * only describe the low 4 GiB, the first return address read off that
   * stack is unmapped: #PF, and the IDT that would catch it is unmapped too,
   * so it double-faults into a triple fault and the machine stops dead -
   * with the console it was drawing through now mapped, which is how this
   * finally became visible.
   *
   * Identity-mapping RAM to ram_top costs one PD (4 KiB) per GiB and removes
   * the whole class of "the boot stage put something high". */
  pdpt = mm_table_new(&pdpt_phys);
  if (pdpt == 0) {
    return;
  }
  pml4->e[0] = MM_PTE_PRESENT | MM_PTE_WRITE | (pdpt_phys & MM_PTE_ADDR4K_MASK);
  {
    UINT64 n_pd = (ram_top_bytes + 0x3FFFFFFFULL) >> 30; /* ceil to 1 GiB */

    if (n_pd == 0u) {
      n_pd = 1u; /* even an empty map has to describe something */
    }
    if (n_pd > 512u) {
      kputs("[MM] identity map clamped to 512 GiB (ram_top=");
      kputs_hex(ram_top_bytes);
      kputs(")\n");
      n_pd = 512u;
    }
    for (i = 0; (UINT64)i < n_pd; i++) {
      UINT64 pd_phys;
      mm_table_t *pd = mm_table_new(&pd_phys);
      if (pd == 0) {
        return;
      }
      pdpt->e[i] = MM_PTE_PRESENT | MM_PTE_WRITE | (pd_phys & MM_PTE_ADDR4K_MASK);
      mm_fill_pd_2m(pd, (UINT64)i << 30);
    }
    kputs("[MM] identity map ");
    kputs_hex(ram_top_bytes);
    kputs(" bytes, ");
    kputs_hex(n_pd);
    kputs(" x 1 GiB\n");
  }

  /* Then take the captured scanout out of that range and mark it uncached.
   *
   * The identity map above describes RAM, and mm_fill_pd_2m marks every one
   * of those pages write-back. A framebuffer is MMIO: left write-back its
   * pixels sit in cache and the panel never sees them, which is the same
   * black screen by a different route. The boot text console (fbcon) draws
   * through the framebuffer's physical address, so this also keeps it alive
   * across the CR3 switch instead of pausing inside mm_init - the pause is
   * what made the earlier failure silent. */
  if (fb_size != 0u) {
    UINT64 p_start = fb_phys & ~0x1FFFFFULL;
    UINT64 p_end = (fb_phys + fb_size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    UINT64 p;
    UINT64 pages = 0;

    for (p = p_start; p < p_end; p += 0x200000ULL) {
      UINT32 sl = (UINT32)((p >> 30) & 0x1FFu);
      mm_table_t *pd;
      if ((pdpt->e[sl] & MM_PTE_PRESENT) != 0u) {
        pd = (mm_table_t *)(UINTN)(pdpt->e[sl] & MM_PTE_ADDR4K_MASK);
      } else {
        UINT64 pd_phys2;
        pd = mm_table_new(&pd_phys2);
        if (pd == 0) {
          break;
        }
        pdpt->e[sl] = MM_PTE_PRESENT | MM_PTE_WRITE |
                      (pd_phys2 & MM_PTE_ADDR4K_MASK);
      }
      pd->e[(p >> 21) & 0x1FFu] = MM_PTE_PRESENT | MM_PTE_WRITE | MM_PTE_HUGE |
                                  (p & MM_PTE_ADDR2M_MASK) | fb_bits;
      pages++;
    }
    kputs("[MM] framebuffer at ");
    kputs_hex(fb_phys);
    kputs(" marked uncached over ");
    kputs_hex(pages << 21);
    kputs(" bytes, cache-");
    kputs((g_pat_ok != 0) ? "write-combining\n" : "disabled\n");
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
    if (n_pd > 512u) {
      /* One PDPT holds 512 GiB; beyond that the raw array write below
       * would overwrite whatever follows it in the heap. */
      kputs("[MM] physmap clamped to 512 GiB (ram_top=");
      kputs_hex(ram_top_bytes);
      kputs(")\n");
      n_pd = 512u;
    }
    for (gi = 0; gi < n_pd; gi++) {
      UINT64 pd_phys;
      mm_table_t *pd = mm_table_new(&pd_phys);
      if (pd == 0) {
        return;
      }
      pdpt->e[gi] = MM_PTE_PRESENT | MM_PTE_WRITE | (pd_phys & MM_PTE_ADDR4K_MASK);
      mm_fill_pd_2m(pd, gi << 30);
    }
    kputs("[MM] physmap ");
    kputs_hex(ram_top_bytes);
    kputs(" bytes, ");
    kputs_hex(n_pd);
    kputs(" x 1 GiB\n");
  }

  /* Switch CR3. The new tables identity-map the full low 4 GiB, so this
   * function's code and the kernel stack stay reachable on both the
   * identity path and the new physmap path. */
  memset(&g_heap_pdpt, 0, sizeof(g_heap_pdpt));
  memset(&g_heap_pd, 0, sizeof(g_heap_pd));
  memset(g_heap_pt, 0, sizeof(g_heap_pt));
  pml4->e[258] = (UINT64)(UINTN)&g_heap_pdpt | MM_PTE_PRESENT | MM_PTE_WRITE;
  g_heap_pdpt.e[0] = (UINT64)(UINTN)&g_heap_pd | MM_PTE_PRESENT | MM_PTE_WRITE;
  for (i = 0; i < MM_HEAP_SIZE / (2 * 1024 * 1024); i++) {
    g_heap_pd.e[i] = (UINT64)(UINTN)&g_heap_pt[i] | MM_PTE_PRESENT | MM_PTE_WRITE;
  }
  kputs("[MM] switching CR3 to ");
  kputs_hex(pml4_phys);
  kputs("\n");
  mm_write_cr3(pml4_phys);
  mm_capture_kernel_cr3();

  kputs("[MM] paging enabled, CR3=");
  kputs_hex(mm_get_cr3());
  kputs("\n");

  mm_selftest();
}

static int mm_heap_range(UINT64 offset, UINT32 pages) {
  return pages != 0 && (offset & (MM_PAGE_SIZE - 1)) == 0 &&
      offset < MM_HEAP_SIZE &&
      (UINT64)pages <= (MM_HEAP_SIZE - offset) / MM_PAGE_SIZE;
}

void mm_heap_unmap(UINT64 offset, UINT32 pages) {
  UINT32 i;
  if (!mm_heap_range(offset, pages)) {
    kputs("[MM] ERROR: invalid heap unmap range\n");
    return;
  }
  for (i = 0; i < pages; i++) {
    UINT64 page = offset / MM_PAGE_SIZE + i;
    UINT64 *entry = &g_heap_pt[page / 512].e[page % 512];
    UINT64 phys = *entry & MM_PTE_ADDR4K_MASK;
    if ((*entry & MM_PTE_PRESENT) == 0) {
      kputs("[MM] ERROR: heap page already unmapped\n");
      continue;
    }
    *entry = 0;
    MM_INVALIDATE(MM_HEAP_BASE + page * MM_PAGE_SIZE);
    pmm_free_page((void *)(UINTN)phys);
  }
}

int mm_heap_map(UINT64 offset, UINT32 pages) {
  UINT32 i;
  if (!mm_heap_range(offset, pages)) {
    kputs("[MM] ERROR: invalid heap map range\n");
    return -1;
  }
  for (i = 0; i < pages; i++) {
    UINT64 page = offset / MM_PAGE_SIZE + i;
    if (g_heap_pt[page / 512].e[page % 512] != 0) {
      kputs("[MM] ERROR: heap mapping overlaps an allocation\n");
      return -1;
    }
  }
  for (i = 0; i < pages; i++) {
    UINT64 page = offset / MM_PAGE_SIZE + i;
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();
    if (phys == 0) {
      if (i != 0) mm_heap_unmap(offset, i);
      kputs("[MM] ERROR: no physical page for heap mapping\n");
      return -1;
    }
    g_heap_pt[page / 512].e[page % 512] = phys | MM_PTE_PRESENT | MM_PTE_WRITE;
    MM_INVALIDATE(MM_HEAP_BASE + page * MM_PAGE_SIZE);
  }
  return 0;
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

/* ---- high physical mapping (device MMIO / framebuffers above ram_top) -----
 *
 * The identity map covers only the low 4 GiB and the physmap window only RAM
 * below ram_top, so a device register block or a framebuffer sitting at a
 * physical address above those windows is unreachable. mm_map_phys() maps such
 * ranges into a fixed high virtual region through PML4[257] using 2 MiB huge
 * pages (the page-table grain the rest of mm_init already uses). */
#define MM_DEVMAP_BASE        0xFFFF808000000000ULL  /* -> PML4[257] */
#define MM_DEVMAP_PML4_IDX    257u
#define MM_MAP_UC             0x01u

static UINT64 g_devmap_next = MM_DEVMAP_BASE;

/* Return the (already mapped) child table for a present parent entry, or
 * allocate and wire one in. Callers write the leaf through the returned
 * pointer. */
static mm_table_t *mm_ensure_level(mm_table_t *parent, UINT32 idx) {
  UINT64 e = parent->e[idx];

  if ((e & MM_PTE_PRESENT) != 0u) {
    return (mm_table_t *)phys_to_virt(e & MM_PTE_ADDR4K_MASK);
  }
  {
    UINT64 child_phys;
    mm_table_t *child = mm_table_new(&child_phys);

    if (child == 0) {
      return 0;
    }
    parent->e[idx] = MM_PTE_PRESENT | MM_PTE_WRITE |
                     (child_phys & MM_PTE_ADDR4K_MASK);
    return child;
  }
}

/* Walk PML4[257] down to the PD covering virtual address `va`, creating the
 * PDPT and PD on demand. Returns the PD (virtual) or NULL. */
static mm_table_t *mm_dev_pd(UINT64 va) {
  mm_table_t *pml4 = (mm_table_t *)phys_to_virt(mm_kernel_cr3());
  mm_table_t *pdpt;
  mm_table_t *pd;

  if (pml4 == 0) {
    return 0;
  }
  pdpt = mm_ensure_level(pml4, MM_DEVMAP_PML4_IDX);
  if (pdpt == 0) {
    return 0;
  }
  pd = mm_ensure_level(pdpt, (UINT32)((va >> 30) & 0x1FFu));
  return pd;
}

/* PAT/CR4 constants, the PAT-availability flag and its CPUID probe live
 * above mm_init(): mm_init() reads g_pat_ok to pick framebuffer cache bits,
 * so they cannot be declared after it. */

void mm_enable_pat(void) {
  UINT64 cr4;

  g_pat_ok = 0;
  if (mm_cpu_has_pat() == 0) {
    kputs("[MM] PAT not advertised by CPUID: write-combining needs an MTRR\n");
    return;
  }
  __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
  kputs("[MM] PAT: CPUID reports it, CR4=");
  kputs_hex(cr4);
  kputs("\n");
  if ((cr4 & CR4_PAT) == 0u) {
    /* Leave CR4 alone. Setting PAT from here is legal on paper but hung the
     * firmware's own page tables on the reference VM, and firmware that
     * leaves CR4.PAT clear is firmware that also leaves the PAT MSR at reset,
     * so the PTE route buys nothing on such a machine that an MTRR range
     * does not: mm_mtrr_wc_cover() below turns the framebuffer into a WC
     * range instead. The machines that matter - laptops whose firmware does
     * set CR4.PAT - take the PTE route with IA32_PAT entry 4 = WC below. */
    kputs("[MM] firmware left CR4.PAT clear: PTE write-combining off, "
          "using an MTRR WC range instead\n");
    return;
  }
  if (msr_read(IA32_PAT_MSR) != IA32_PAT_WC_PA4) {
    msr_write(IA32_PAT_MSR, IA32_PAT_WC_PA4);
  }
  g_pat_ok = 1;
  kputs("[MM] PAT enabled: CR4.PAT set, IA32_PAT entry 4 = write-combining\n");
}/* ---- write-combining without PAT: an MTRR WC range -----------------------
 *
 * Memory type is the intersection of the MTRR type and the page attributes.
 * A cacheable (WB) page inside an MTRR range marked WC therefore behaves as
 * write-combining, which is exactly what PAT would have bought us - and it
 * works on a machine whose firmware never set CR4.PAT. The variable MTRRs
 * are few (8 on most parts) and firmware owns some of them, so the rule here
 * is: reuse hardware state if it already says WC, program one free slot if
 * there is one, and give up quietly rather than fight an existing range. */
#define MM_MSR_MTRRCAP        0x000000FEu
#define MM_MSR_MTRR_DEF_TYPE  0x000002FFu
#define MM_MSR_MTRR_PHYSBASE  0x00000200u /* + 2 * n */
#define MM_MSR_MTRR_PHYSMASK  0x00000201u /* + 2 * n */
#define MM_MTRR_TYPE_WC       1u
#define MM_MTRR_TYPE_WB       6u
#define MM_MTRR_PHYSBASE_TYPE 0xFFULL
#define MM_MTRR_PHYSMASK_VALID (1ULL << 11)
#define MM_MTRR_DEF_ENABLE    (1ULL << 11)
#define MM_MTRR_CAP_WC        (1ULL << 10)
#define MM_MTRR_MASK_ADDR     0x000FFFFFFFFFF000ULL

/* Size encoded in a PHYSMASK: the address bits set to zero are the ones the
 * range covers, so size = ~mask + 2^12. */
static UINT64 mm_mtrr_range_size(UINT64 mask) {
  UINT64 inv = (~mask) & MM_MTRR_MASK_ADDR;

  return ((inv + 0x1000ULL) == 0u) ? 0u : inv + 0x1000ULL;
}

/* 1 when [lo, hi) is covered by an MTRR range of type WC, 0 when it is not
 * (including "some other range overlaps", where the answer is someone
 * else's business). */
static int mm_mtrr_wc_cover(UINT64 lo, UINT64 hi) {
  UINT64 cap = msr_read(MM_MSR_MTRRCAP);
  UINT32 vcnt = (UINT32)(cap & 0xFFu);
  UINT64 size = 0x1000ULL;
  UINT64 base, span;
  UINT32 i;
  int    slot = -1;

  if ((cap & MM_MTRR_CAP_WC) == 0u || vcnt == 0u || hi <= lo) {
    kputs("[MM] no variable MTRR with WC support (MTRRCAP=0x");
    kputs_hex(cap);
    kputs("): framebuffer mapped uncached\n");
    return 0;
  }
  span = hi - lo;
  while (size < span) {
    size <<= 1;
  }
  base = lo & ~(size - 1ULL);

  for (i = 0; i < vcnt; i++) {
    UINT64 m = msr_read(MM_MSR_MTRR_PHYSMASK + 2ULL * i);

    if ((m & MM_MTRR_PHYSMASK_VALID) == 0u) {
      if (slot < 0) {
        slot = (int)i;
      }
      continue;
    }
    {
      UINT64 b = msr_read(MM_MSR_MTRR_PHYSBASE + 2ULL * i);
      UINT64 rbase = b & MM_MTRR_MASK_ADDR;
      UINT64 rsize = mm_mtrr_range_size(m);
      UINT64 rhi = rbase + rsize;

      if (rsize == 0u) {
        continue;
      }
      if (rbase <= base && hi <= rhi) {
        /* Our whole range already sits inside this one. */
        if ((b & MM_MTRR_PHYSBASE_TYPE) == MM_MTRR_TYPE_WC) {
          kputs("[MM] framebuffer already inside a WC MTRR range, "
                "write-combining is on\n");
          return 1;
        }
        return 0;
      }
      if (rbase < hi && base < rhi) {
        /* Partial overlap: the intersection would resolve to the more
         * restrictive type, so a WC range here would silently do nothing. */
        return 0;
      }
    }
  }
  if (slot < 0) {
    kputs("[MM] no free MTRR slot for the framebuffer: mapped uncached\n");
    return 0;
  }
  {
    UINT64 def = msr_read(MM_MSR_MTRR_DEF_TYPE);

    msr_write(MM_MSR_MTRR_PHYSBASE + 2ULL * (UINT64)slot,
              (base & MM_MTRR_MASK_ADDR) | MM_MTRR_TYPE_WC);
    msr_write(MM_MSR_MTRR_PHYSMASK + 2ULL * (UINT64)slot,
              ((~(size - 1ULL)) & MM_MTRR_MASK_ADDR) | MM_MTRR_PHYSMASK_VALID);
    if ((def & MM_MTRR_DEF_ENABLE) == 0u) {
      msr_write(MM_MSR_MTRR_DEF_TYPE, def | MM_MTRR_DEF_ENABLE);
    }
  }
  kputs("[MM] WC MTRR programmed: 0x");
  kputs_hex(base);
  kputs(" for 0x");
  kputs_hex(size);
  kputs(" bytes\n");
  return 1;
}

void *mm_map_phys(UINT64 phys, UINT64 size, UINT32 flags) {
  UINT64 base = g_devmap_next;
  UINT64 cur = base;
  /* Pages are counted from the 2 MiB boundary the range is rounded down to,
   * not from `phys`: a framebuffer that starts 512 KiB into a 2 MiB page
   * needs the whole of that page plus enough pages for the rest, so the
   * offset has to be added before the division or the tail of the scanout
   * lands outside the mapping. */
  UINT64 off = phys & 0x1FFFFFULL;
  UINT32 n = (UINT32)(((off + size + 0x1FFFFFULL) >> 21) & 0xFFFFFFFFu);
  UINT32 i;
  UINT32 cache_bits = 0u;

  if (n == 0u) {
    return 0;
  }
  /* Decide the cache policy once, before the loop: every page of this
   * mapping gets the same answer. */
  if ((flags & MM_MAP_UC) != 0u) {
    cache_bits = MM_PTE_PCD;
  } else if ((flags & MM_MAP_WC) != 0u) {
    if (g_pat_ok != 0) {
      cache_bits = MM_PTE_PAT;
    } else if (mm_mtrr_wc_cover(phys & ~0x1FFFFFULL,
                               ((phys & ~0x1FFFFFULL) +
                                ((UINT64)n << 21))) != 0) {
      /* Cacheable page inside a WC MTRR range: the effective type is WC,
       * so no PTE bits at all. */
      cache_bits = 0u;
    } else {
      cache_bits = MM_PTE_PCD;
    }
  }
  for (i = 0; i < n; i++) {
    UINT64 p = (phys & ~0x1FFFFFu) + ((UINT64)i << 21);
    UINT64 v = cur + ((UINT64)i << 21);
    mm_table_t *pd = mm_dev_pd(v);
    UINT32 pdi;

    if (pd == 0) {
      return 0;
    }
    pdi = (UINT32)((v >> 21) & 0x1FFu);
    pd->e[pdi] = MM_PTE_PRESENT | MM_PTE_WRITE | MM_PTE_HUGE |
                 (p & MM_PTE_ADDR2M_MASK) | cache_bits;
  }
  g_devmap_next = cur + ((UINT64)n << 21);
  return (void *)base;
}
