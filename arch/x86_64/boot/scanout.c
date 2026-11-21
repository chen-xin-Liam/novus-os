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
 * work: it walks the live four-level tables for every 4 KiB page of the
 * scanout. A level that is absent is filled in with a fresh page from the
 * caller's allocator; a huge page that is already there is accepted only
 * when it really covers this address. Newly built entries are
 * present/writable and carry PCD (and PWT), so the framebuffer arrives
 * uncached - the same policy the kernel applies to a high scanout with
 * mm_map_phys().
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

#define ADDR_4K 0x000FFFFFFFFFF000ULL /* page-table entry address bits */
#define ADDR_2M 0x000FFFFFFFE00000ULL  /* covered by one 2 MiB page */
#define ADDR_1G 0x000FFFFFC0000000ULL  /* covered by one 1 GiB page */

#define PAGE_4K 0x1000ULL

/* Present, writable, write-through, cache-disable: the framebuffer must
 * never sit in a cacheable alias while the display reads it. */
#define LEVEL_FLAGS (PTE_PRESENT | PTE_RW | PTE_PWT | PTE_PCD)

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
 * the way. `added` records whether anything was written, so the caller
 * knows whether a TLB reload is needed. */
static int ensure_page(UINT64 *pml4, UINT64 phys, boot_scanout_alloc_t alloc,
                       void *ctx, int *added) {
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
    pml4[i4] = ((UINT64)(UINTN)pdpt) | LEVEL_FLAGS;
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
    pdpt[i3] = ((UINT64)(UINTN)pd) | LEVEL_FLAGS;
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
    pd[i2] = ((UINT64)(UINTN)pt) | LEVEL_FLAGS;
    *added = 1;
    e = pd[i2];
  } else if ((e & PTE_PS) != 0u) {
    /* A 2 MiB page covers this address, provided it is this 2 MiB. */
    return ((e & ADDR_2M) == (phys & ADDR_2M)) ? 1 : 0;
  }
  pt = (UINT64 *)(UINTN)(e & ADDR_4K);
  if ((pt[i1] & PTE_PRESENT) == 0u) {
    pt[i1] = (phys & ADDR_4K) | LEVEL_FLAGS;
    *added = 1;
  }
  return 1;
}

int boot_scanout_walk(UINT64 *pml4, UINT64 phys, UINT64 size,
                      boot_scanout_alloc_t alloc, void *ctx, int *added) {
  UINT64 start;
  UINT64 end;
  UINT64 a;
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
  for (a = start; a < end; a += PAGE_4K) {
    int wrote = 0;

    if (!ensure_page(pml4, a, alloc, ctx, &wrote)) {
      any |= wrote;
      if (added != 0) {
        *added = any;
      }
      return BOOT_SCANOUT_FAILED;
    }
    any |= wrote;
  }
  if (added != 0) {
    *added = any;
  }
  return any ? BOOT_SCANOUT_MAPPED : BOOT_SCANOUT_PRESENT;
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
