/**
 * Novus OS - reachability of the firmware's scanout from the boot stage.
 *
 * Before ExitBootServices the CPU runs on the firmware's own page tables,
 * so a framebuffer is addressable only if those tables cover it. Guessing
 * that from "below 4 GiB" breaks on any machine whose graphics aperture
 * lands above 4 GiB: the splash then skips itself although the memory is
 * perfectly reachable through a mapping nobody bothered to build.
 *
 * boot_scanout_walk() is the whole of the decision and the whole of the
 * work: it walks the live four-level tables for the scanout range. Whole
 * 2 MiB-aligned runs get one huge leaf, the same granularity mm_init() and
 * mm_map_phys() use; unaligned edges and ranges whose PD already holds a
 * 4 KiB page table stay 4 KiB, so a device mapping never swallows memory
 * the firmware mapped for something else. A level that is absent is filled
 * in with a fresh page from the caller's allocator; a huge page that is
 * already there is accepted only when it really covers this address.
 *
 * Cache policy follows the kernel's MM_MAP_WC rule: when CR4.PAT is already
 * on, IA32_PAT entry 4 is made WC and the leaf uses the PAT bit; otherwise
 * no PAT bit is written (it would be reserved while CR4.PAT is clear) and
 * the leaf falls back to PCD/UC until the kernel's mm_init() installs its
 * own WC mapping after the CR3 switch.
 *
 * The table address and the allocator are parameters rather than globals
 * because the walk has a host test (scripts/scanout_walk_test.c) that runs
 * it against synthetic tables: this is the one piece of the boot path that
 * decides whether a machine shows its splash, and on real hardware it is
 * hard to exercise on purpose. The firmware's tables are the live ones and
 * the added identity mappings for device memory collide with nothing, so
 * boot_scanout_map() is a thin wrapper: read CR3, walk, reload CR3 when
 * something was written.
 */

#include "uefi.h"
#include "boot_scanout.h"

#define PTE_PRESENT 0x001ULL
#define PTE_RW      0x002ULL
#define PTE_PWT     0x008ULL
#define PTE_PCD     0x010ULL
#define PTE_PS      0x080ULL
#define PTE_PAT     0x1000ULL

#define ADDR_4K 0x000FFFFFFFFFF000ULL /* page-table entry address bits */
#define ADDR_2M 0x000FFFFFFFE00000ULL  /* covered by one 2 MiB page */
#define ADDR_1G 0x000FFFFFC0000000ULL  /* covered by one 1 GiB page */

#define PAGE_4K 0x1000ULL
#define PAGE_2M 0x200000ULL

/* Upper levels are plain present/writable, like the kernel's tables: making
 * the page-table pages themselves uncached only slows every walk. */
#define TABLE_FLAGS (PTE_PRESENT | PTE_RW)
/* No PAT: PCD selects the uncached entry. PWT is left clear so this matches
 * mm_map_phys(MM_MAP_UC) instead of the older UC- PCD|PWT combination. */
#define LEAF_UC (PTE_PRESENT | PTE_RW | PTE_PCD)
/* CR4.PAT is on: PAT bit selects IA32_PAT entry 4 (WC), PCD/PWT clear. */
#define LEAF_WC (PTE_PRESENT | PTE_RW | PTE_PS | PTE_PAT)

#define MSR_IA32_PAT     0x00000277u
#define CR4_PAT          (1ULL << 20)
#define IA32_PAT_PA4_WC   0x0007040100070406ULL

/* The host test compiles this file without firmware MSRs. Its allocator is
 * synthetic, so it must see the safe UC fallback rather than execute
 * rdmsr in user mode. */
#ifdef BOOT_SCANOUT_HOST
static UINT64 boot_fb_flags(void) { return LEAF_UC & ~PTE_PS; }
static int    boot_fb_wc(void) { return 0; }
#else
static UINT64 boot_msr_read(UINT32 msr) {
  UINT32 lo = 0;
  UINT32 hi = 0;

  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((UINT64)hi << 32) | (UINT64)lo;
}

static void boot_msr_write(UINT32 msr, UINT64 value) {
  UINT32 lo = (UINT32)value;
  UINT32 hi = (UINT32)(value >> 32);

  __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* Decided once per walk. Writing IA32_PAT entry 4 is the same move
 * mm_enable_pat() makes later; without CR4.PAT the PAT bit is reserved, so
 * that case must never take this branch. */
static UINT64 boot_fb_flags(void) {
  UINT64 cr4 = 0;

  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  if ((cr4 & CR4_PAT) != 0u) {
    UINT64 pat = boot_msr_read(MSR_IA32_PAT);

    if (((pat >> 24) & 0xFFu) != 0x01u) {
      pat = (pat & ~(0xFFULL << 24)) | (0x01ULL << 24);
      boot_msr_write(MSR_IA32_PAT, pat);
    }
    return PTE_PAT; /* caller ORs PTE_PS for a huge leaf */
  }
  return PTE_PCD;   /* UC until the kernel's mm_init() takes over */
}

static int boot_fb_wc(void) {
  return (boot_fb_flags() & PTE_PAT) != 0u;
}
#endif

/* Leaf flags for a 4 KiB page: no PS bit. */
#define LEAF_FLAGS_4K(base) ((base) | PTE_PRESENT | PTE_RW)

static UINT64 read_cr3(void) {
  UINT64 v;

  __asm__ volatile("mov %%cr3, %0" : "=r"(v));
  return v & ADDR_4K;
}

static void reload_cr3(UINT64 cr3) {
  __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* One zeroed page for a new table level. AllocatePages, not AllocatePool:
 * the table must be page aligned, and EfiLoaderData keeps it out of the
 * way of firmware's own allocations, which the kernel never frees. */
static UINT64 *bs_level_page(void *ctx) {
  EFI_BOOT_SERVICES  *bs = (EFI_BOOT_SERVICES *)ctx;
  EFI_PHYSICAL_ADDRESS addr = 0;
  UINT64              *p;
  UINT64               i;

  if (bs == 0 || bs->AllocatePages == 0) {
    return 0;
  }
  if (EFI_ERROR(bs->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &addr)) ||
      addr == 0) {
    return 0;
  }
  p = (UINT64 *)(UINTN)addr;
  for (i = 0; i < 512u; i++) {
    p[i] = 0;
  }
  return p;
}

/* Make the 4 KiB page at `phys` present, building every missing level on
 * the way. `leaf_base` is PTE_PAT or PTE_PCD, the same cache attribute the
 * kernel gives the same page. `added` records whether anything was written,
 * so the caller knows whether a TLB reload is needed. */
static int ensure_page(UINT64 *pml4, UINT64 phys, UINT64 leaf_base,
                       boot_scanout_alloc_t alloc, void *ctx, int *added) {
  UINT64  i4 = (phys >> 39) & 0x1FFu;
  UINT64  i3 = (phys >> 30) & 0x1FFu;
  UINT64  i2 = (phys >> 21) & 0x1FFu;
  UINT64  i1 = (phys >> 12) & 0x1FFu;
  UINT64  e = pml4[i4];
  UINT64 *pdpt;
  UINT64 *pd;
  UINT64 *pt;

  if ((e & PTE_PRESENT) == 0u) {
    pdpt = alloc(ctx);
    if (pdpt == 0) {
      return 0;
    }
    pml4[i4] = ((UINT64)(UINTN)pdpt) | TABLE_FLAGS;
    *added = 1;
    e = pml4[i4];
  }
  pdpt = (UINT64 *)(UINTN)(e & ADDR_4K);
  e = pdpt[i3];
  if ((e & PTE_PRESENT) == 0u) {
    pd = alloc(ctx);
    if (pd == 0) {
      return 0;
    }
    pdpt[i3] = ((UINT64)(UINTN)pd) | TABLE_FLAGS;
    *added = 1;
    e = pdpt[i3];
  } else if ((e & PTE_PS) != 0u) {
    /* A 1 GiB page covers this address, provided it is this 1 GiB. */
    return ((e & ADDR_1G) == (phys & ADDR_1G)) ? 1 : 0;
  }
  pd = (UINT64 *)(UINTN)(e & ADDR_4K);
  e = pd[i2];
  if ((e & PTE_PRESENT) == 0u) {
    pt = alloc(ctx);
    if (pt == 0) {
      return 0;
    }
    pd[i2] = ((UINT64)(UINTN)pt) | TABLE_FLAGS;
    *added = 1;
    e = pd[i2];
  } else if ((e & PTE_PS) != 0u) {
    /* A 2 MiB page covers this address, provided it is this 2 MiB. */
    return ((e & ADDR_2M) == (phys & ADDR_2M)) ? 1 : 0;
  }
  pt = (UINT64 *)(UINTN)(e & ADDR_4K);
  if ((pt[i1] & PTE_PRESENT) == 0u) {
    pt[i1] = LEAF_FLAGS_4K(leaf_base) | (phys & ADDR_4K);
    *added = 1;
  }
  return 1;
}

/* One 2 MiB leaf for a fully covered, aligned block. Returns 0 when the PD
 * already points at a 4 KiB table: that memory belongs to other mappings,
 * so the caller must use 4 KiB leaves for this block instead of replacing
 * the whole directory entry. */
static int ensure_2m(UINT64 *pml4, UINT64 phys, UINT64 leaf_base,
                     boot_scanout_alloc_t alloc, void *ctx, int *added) {
  UINT64  i4 = (phys >> 39) & 0x1FFu;
  UINT64  i3 = (phys >> 30) & 0x1FFu;
  UINT64  i2 = (phys >> 21) & 0x1FFu;
  UINT64  e = pml4[i4];
  UINT64 *pdpt;
  UINT64 *pd;

  if ((e & PTE_PRESENT) == 0u) {
    pdpt = alloc(ctx);
    if (pdpt == 0) {
      return 0;
    }
    pml4[i4] = ((UINT64)(UINTN)pdpt) | TABLE_FLAGS;
    *added = 1;
    e = pml4[i4];
  }
  pdpt = (UINT64 *)(UINTN)(e & ADDR_4K);
  e = pdpt[i3];
  if ((e & PTE_PRESENT) == 0u) {
    pd = alloc(ctx);
    if (pd == 0) {
      return 0;
    }
    pdpt[i3] = ((UINT64)(UINTN)pd) | TABLE_FLAGS;
    *added = 1;
    e = pdpt[i3];
  } else if ((e & PTE_PS) != 0u) {
    return ((e & ADDR_1G) == (phys & ADDR_1G)) ? 1 : 0;
  }
  pd = (UINT64 *)(UINTN)(e & ADDR_4K);
  e = pd[i2];
  if ((e & PTE_PS) != 0u) {
    /* An existing huge page is already a mapping; accept it only when it
     * covers this very 2 MiB block. */
    return ((e & ADDR_2M) == (phys & ADDR_2M)) ? 1 : 0;
  }
  if ((e & PTE_PRESENT) != 0u) {
    return 0; /* a 4 KiB PT is already here: fall back */
  }
  pd[i2] = (phys & ADDR_2M) | PTE_PRESENT | PTE_RW | PTE_PS | leaf_base;
  *added = 1;
  return 1;
}

int boot_scanout_walk(UINT64 *pml4, UINT64 phys, UINT64 size,
                      boot_scanout_alloc_t alloc, void *ctx, int *added) {
  UINT64 start;
  UINT64 end;
  UINT64 a;
  UINT64 leaf_base = boot_fb_flags();
  int    any = 0;

  if (pml4 == 0 || alloc == 0 || phys == 0u || size == 0u ||
      phys + size < phys) {
    return BOOT_SCANOUT_FAILED;
  }
  start = phys & ~0xFFFULL;
  end = (phys + size + 0xFFFu) & ~0xFFFULL;
  if (end < start) {
    return BOOT_SCANOUT_FAILED;
  }
  /* Map whole 2 MiB blocks with one leaf, exactly as the kernel's
   * mm_init()/mm_map_phys() do. Partial blocks keep 4 KiB granularity so
   * neighbouring addresses the firmware mapped for something else are not
   * swallowed by a device mapping. */
  for (a = phys & ~(PAGE_2M - 1u); a < end; a += PAGE_2M) {
    UINT64 block_end = a + PAGE_2M;
    UINT64 lo = (a > start) ? a : start;
    UINT64 hi = (block_end < end) ? block_end : end;
    int    whole = (lo == a && hi == block_end);
    int    wrote = 0;
    UINT64 p;

    if (whole && ensure_2m(pml4, a, leaf_base, alloc, ctx, &wrote)) {
      any |= wrote;
      continue;
    }
    for (p = lo; p < hi; p += PAGE_4K) {
      if (!ensure_page(pml4, p, leaf_base, alloc, ctx, &wrote)) {
        any |= wrote;
        if (added != 0) {
          *added = any;
        }
        return BOOT_SCANOUT_FAILED;
      }
    }
    any |= wrote;
  }
  if (added != 0) {
    *added = any;
  }
  if (any == 0) {
    return BOOT_SCANOUT_PRESENT;
  }
  return boot_fb_wc() ? BOOT_SCANOUT_MAPPED_WC : BOOT_SCANOUT_MAPPED;
}

int boot_scanout_map(void *boot_services, UINT64 phys, UINT64 size) {
  UINT64 cr3;
  UINT64 *pml4;
  int    added = 0;
  int    rc;

  cr3 = read_cr3();
  if (cr3 == 0u) {
    return BOOT_SCANOUT_FAILED;
  }
  pml4 = (UINT64 *)(UINTN)cr3;
  rc = boot_scanout_walk(pml4, phys, size, bs_level_page, boot_services,
                         &added);
  if (added != 0) {
    /* The walk may have left half a table behind even when it failed; the
     * reload costs nothing and makes sure the CPU is not holding stale
     * non-present entries for the pages that did get mapped. */
    reload_cr3(cr3);
  }
  return rc;
}

/* Bytes a GOP mode occupies, pitch-aware because PixelsPerScanLine often
 * exceeds the visible width. */
UINT64 boot_scanout_bytes(UINT32 pitch, UINT32 height) {
  return (UINT64)pitch * (UINT64)height * 4u;
}
