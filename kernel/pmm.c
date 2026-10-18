/**
 * Novus OS - physical memory manager (PMM), M1 Task 4.
 *
 * Bitmap allocator over the final UEFI memory map. All bitmap state is
 * manipulated through the novuslib-rs FFI (lib/novuslib_rs.h), proving
 * the C/Rust mixed-link path (README 16.4) on real kernel code.
 *
 * Layout decisions:
 * - 256 KiB static bitmap in .bss: 2 Mi pages ~= 8 GiB RAM coverage.
 *   Pages beyond bitmap capacity stay unallocatable and are accounted
 *   into the reserved count.
 * - Explicitly reserved regions (README 2.3 "共享内存规范" + M1 spec):
 *   physical 0x7E000-0x7FFFF (shared-memory pages, A/B core handoff),
 *   physical 0x8000 (AP trampoline page), physical 0x0 (BIOS/IVT,
 *   defensive).
 * - The init path takes a small bounded self-implemented spin lock
 *   (1<<20 attempts, timeout degrades with a warning); deliberately not
 *   the shared-memory lock owned by the mm/ agents.
 */

#include "pmm.h"
#include "kernel.h"

#define PMM_PAGE_SIZE    4096ULL
#define PMM_BITMAP_BYTES 262144ULL                 /* 256 KiB in .bss */
#define PMM_BITMAP_BITS  (PMM_BITMAP_BYTES * 8ULL) /* 2 Mi pages ~= 8 GiB */
#define PMM_2M_MASK      0x1FFFFFULL
#define PMM_SPIN_LIMIT   (1ULL << 20)

/* Explicitly reserved physical regions (page numbers). */
#define PMM_PAGE_IVT      0ULL                     /* BIOS/IVT fallback    */
#define PMM_PAGE_AP_TRAMP (0x8000ULL / PMM_PAGE_SIZE)
#define PMM_PAGE_SHMEM_LO (0x7E000ULL / PMM_PAGE_SIZE)
#define PMM_PAGE_SHMEM_HI (0x7FFFFULL / PMM_PAGE_SIZE)

/* Alloc/free regression depth for the selftest. */
#define PMM_SELFTEST_N 8

static UINT8 pmm_bitmap[PMM_BITMAP_BYTES]; /* .bss, zeroed by loader */

static UINT64 pmm_top;   /* ram_top in bytes, 2 MiB aligned */
static UINT64 pmm_total; /* pages: pmm_top / 4096 */
static UINT64 pmm_bits;  /* pages actually tracked by the bitmap */
static UINT64 pmm_free;
static UINT64 pmm_resv;
static UINT32 pmm_double_free;
static int    pmm_ready;

/* ------------------------------------------------------------------ */
/* Bounded init spin lock (self-implemented, local to the PMM)          */
/* ------------------------------------------------------------------ */

static volatile int pmm_init_lock;

/* Returns 1 when the lock is held, 0 after the bounded spin timed out
 * (degraded mode: the caller proceeds without exclusion). */
static int pmm_lock_acquire(void) {
  UINT64 spins = 0;
  while (__sync_lock_test_and_set(&pmm_init_lock, 1) != 0) {
    spins++;
    if (spins > PMM_SPIN_LIMIT) {
      kputs("[PMM] WARNING: init lock timeout; continuing unlocked\n");
      return 0;
    }
  }
  return 1;
}

static void pmm_lock_release(int held) {
  if (held) {
    __sync_lock_release(&pmm_init_lock);
  }
}

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                       */
/* ------------------------------------------------------------------ */

/* Mark one page used; moves it from the free to the reserved bucket. */
static void pmm_reserve_page(UINT64 idx) {
  if (idx >= pmm_bits) {
    return;
  }
  if (novus_rs_bitmap_test(pmm_bitmap, idx) == 0) {
    novus_rs_bitmap_set(pmm_bitmap, idx);
    pmm_free--;
  }
}

/* True when the physical address falls in an explicitly reserved region. */
static int pmm_in_reserved_region(UINT64 addr) {
  UINT64 idx = addr / PMM_PAGE_SIZE;
  return idx == PMM_PAGE_IVT || idx == PMM_PAGE_AP_TRAMP ||
         (idx >= PMM_PAGE_SHMEM_LO && idx <= PMM_PAGE_SHMEM_HI);
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void pmm_init(UINT64 mmap_addr, UINT64 map_size, UINT64 desc_size) {
  const EFI_MEMORY_DESCRIPTOR *d;
  UINT64 off, top, start_pg, end_pg;
  int locked;

  if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
    kputs("[PMM] ERROR: descriptor size too small\n");
    return;
  }
  if (mmap_addr == 0 || map_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
    kputs("[PMM] ERROR: empty memory map\n");
    return;
  }

  locked = pmm_lock_acquire();

  /* Pass 1: ram_top = highest PhysicalStart + Pages * 4096 across all
   * descriptors, rounded up to the 2 MiB allocator granularity. */
  top = 0;
  for (off = 0; off + sizeof(EFI_MEMORY_DESCRIPTOR) <= map_size;
       off += desc_size) {
    d = (const EFI_MEMORY_DESCRIPTOR *)(mmap_addr + off);
    if (d->PhysicalStart + d->Pages * PMM_PAGE_SIZE > top) {
      top = d->PhysicalStart + d->Pages * PMM_PAGE_SIZE;
    }
  }
  pmm_top   = (top + PMM_2M_MASK) & ~PMM_2M_MASK;
  pmm_total = pmm_top / PMM_PAGE_SIZE;
  pmm_bits  = (pmm_total <= PMM_BITMAP_BITS) ? pmm_total : PMM_BITMAP_BITS;

  /* Start with every tracked page marked used, then free the
   * conventional-memory runs below. Pages covered by other descriptor
   * types (including EfiLoaderCode/EfiLoaderData, i.e. the kernel image
   * and other firmware-loaded regions) stay used. */
  memset(pmm_bitmap, 0xFF, PMM_BITMAP_BYTES);

  /* Pass 2: Type == EfiConventionalMemory -> free (0). */
  pmm_free = 0;
  for (off = 0; off + sizeof(EFI_MEMORY_DESCRIPTOR) <= map_size;
       off += desc_size) {
    d = (const EFI_MEMORY_DESCRIPTOR *)(mmap_addr + off);
    if (d->Type != EfiConventionalMemory) {
      continue;
    }
    start_pg = (d->PhysicalStart + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    end_pg   = (d->PhysicalStart + d->Pages * PMM_PAGE_SIZE) / PMM_PAGE_SIZE;
    while (start_pg < end_pg && start_pg < pmm_bits) {
      if (novus_rs_bitmap_test(pmm_bitmap, start_pg) != 0) {
        novus_rs_bitmap_clear(pmm_bitmap, start_pg);
        pmm_free++;
      }
      start_pg++;
    }
  }

  /* Explicit reservations (README 2.3 + spec): 2 shared-memory pages at
   * 0x7E000-0x7FFFF, the AP trampoline page at 0x8000, and the BIOS/IVT
   * page at 0x0 as a defensive floor. */
  pmm_reserve_page(PMM_PAGE_IVT);
  pmm_reserve_page(PMM_PAGE_AP_TRAMP);
  {
    UINT64 pg;
    for (pg = PMM_PAGE_SHMEM_LO; pg <= PMM_PAGE_SHMEM_HI; pg++) {
      pmm_reserve_page(pg);
    }
  }

  /* Reserved = everything that is not free: non-conventional map types,
   * the explicit reservations above, the gap up to the 2 MiB-aligned
   * ram_top, and pages beyond bitmap capacity (unallocatable). */
  pmm_resv  = pmm_total - pmm_free;
  pmm_ready = 1;

  pmm_lock_release(locked);
}

/* ------------------------------------------------------------------ */
/* Allocation                                                           */
/* ------------------------------------------------------------------ */

void *pmm_alloc_page(void) {
  INT64  idx;
  UINT64 addr;

  if (!pmm_ready) {
    return NULL;
  }
  idx = novus_rs_bitmap_find_first_clear_run(pmm_bitmap, pmm_bits, 1);
  if (idx < 0) {
    return NULL;
  }
  novus_rs_bitmap_set(pmm_bitmap, (UINT64)idx);
  pmm_free--;
  addr = (UINT64)idx * PMM_PAGE_SIZE;
  return (void *)addr;
}

/* Counted so the health line and the self test can see a caller bug. */
UINT32 pmm_double_free_count(void) { return pmm_double_free; }

void pmm_free_page(void *phys) {
  UINT64 idx;

  if (!pmm_ready || phys == NULL) {
    return;
  }
  if (((UINT64)phys & (PMM_PAGE_SIZE - 1)) != 0) {
    return; /* not page-aligned: ignore */
  }
  idx = (UINT64)phys / PMM_PAGE_SIZE;
  if (idx >= pmm_bits) {
    return; /* outside bitmap-managed range */
  }
  if (novus_rs_bitmap_test(pmm_bitmap, idx) == 0) {
    /* Freeing a page twice is harmless to the bitmap but always a bug in
     * the caller - usually a page table or code page released by the
     * wrong address space. Say so instead of swallowing it. */
    pmm_double_free++;
    if (pmm_double_free <= 8u) {
      kputs("[PMM] WARN double free of ");
      kputs_hex((UINT64)(UINTN)phys);
      kputs("\n");
    } else if (pmm_double_free == 9u) {
      kputs("[PMM] WARN further double frees suppressed\n");
    }
    return;
  }
  novus_rs_bitmap_clear(pmm_bitmap, idx);
  pmm_free++;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                           */
/* ------------------------------------------------------------------ */

UINT64 pmm_free_count(void) {
  return pmm_free;
}

UINT64 pmm_total_pages(void) {
  return pmm_total;
}

UINT64 pmm_reserved_pages(void) {
  return pmm_resv;
}

UINT64 pmm_ram_top(void) {
  return pmm_top;
}

/* ------------------------------------------------------------------ */
/* Rust panic landing pad (declared in lib/novuslib_rs.h)               */
/* ------------------------------------------------------------------ */

VOID novus_rs_panic_hook(const UINT8 *msg, UINT64 len) {
  char   buf[65];
  UINT64 i;

  if (len > 64) {
    len = 64; /* message bytes are raw, not NUL-terminated */
  }
  for (i = 0; i < len; i++) {
    buf[i] = (char)msg[i];
  }
  buf[len] = '\0';

  kputs("[PANIC] novuslib-rs: ");
  kputs(buf);
  kputs("\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

/* ------------------------------------------------------------------ */
/* Selftest                                                             */
/* ------------------------------------------------------------------ */

void pmm_selftest(void) {
  void  *pages[PMM_SELFTEST_N];
  UINT64 free_before, free_after;
  UINTN  i, j;
  int    ok = 1;

  if (!pmm_ready) {
    kputs("[PMM] selftest SKIPPED (not initialized)\n");
    return;
  }

  kputs("[PMM] bitmap 256KiB, pages: total/reserved/free = ");
  kputs_hex(pmm_total_pages());
  kputs("/");
  kputs_hex(pmm_reserved_pages());
  kputs("/");
  kputs_hex(pmm_free_count());
  kputs("\n");

  /* Rust FFI proof: the version call travels through the real C <->
   * Rust link, not a stub. */
  if (novus_rs_version() == 0x00010000) {
    kputs("[PMM] novuslib-rs v0.1 FFI active\n");
  } else {
    kputs("[PMM] ERROR: novuslib-rs version mismatch\n");
    ok = 0;
  }

  /* Alloc/free regression: grab N pages, require them distinct and
   * outside every reserved region, release them, and require the free
   * count to return exactly to its starting value. */
  free_before = pmm_free_count();
  for (i = 0; i < PMM_SELFTEST_N; i++) {
    pages[i] = pmm_alloc_page();
    if (pages[i] == NULL) {
      kputs("[PMM] ERROR: alloc returned NULL\n");
      ok = 0;
      break;
    }
    if (pmm_in_reserved_region((UINT64)pages[i])) {
      kputs("[PMM] ERROR: alloc handed out a reserved page\n");
      ok = 0;
      break;
    }
    for (j = 0; j < i; j++) {
      if (pages[j] == pages[i]) {
        kputs("[PMM] ERROR: duplicate allocation\n");
        ok = 0;
        break;
      }
    }
  }
  for (j = 0; j <= i && j < PMM_SELFTEST_N; j++) {
    if (pages[j] != NULL) {
      pmm_free_page(pages[j]);
    }
  }
  free_after = pmm_free_count();

  if (free_after != free_before) {
    kputs("[PMM] ERROR: free count not restored\n");
    ok = 0;
  }
  if (pmm_free_count() + pmm_reserved_pages() != pmm_total_pages()) {
    kputs("[PMM] ERROR: statistics inconsistent\n");
    ok = 0;
  }

  if (ok) {
    kputs("[PMM] selftest PASS (alloc/free regression)\n");
  } else {
    kputs("[PMM] selftest FAIL\n");
  }
}
