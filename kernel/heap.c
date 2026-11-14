/**
 * Novus OS - page-granular kernel heap (README 3.1 memory partition 4).
 *
 * kmalloc() rounds requests to whole 4 KiB pages and maps individual PMM
 * pages into a contiguous virtual window; kfree() unmaps them. Every
 * allocation consumes a whole number of pages.
 *
 * Accounting: kfree() must know how many pages belong to a pointer. The
 * count is stored in a fixed-size allocation record table (linear search)
 * instead of a header in front of the user data - the caller would
 * overwrite a page header immediately. When the table is full kmalloc()
 * fails with a console warning; 256 concurrent allocations are plenty
 * for M1.
 */

#include "kernel.h"
#include "heap.h"
#include "mm.h"
#include "pmm.h"

#define HEAP_MAX_ALLOCS 256

typedef struct {
  void  *ptr;   /* first virtual page of the block */
  UINT64 pages; /* number of pages in the block */
} heap_rec_t;

static heap_rec_t g_heap_recs[HEAP_MAX_ALLOCS];

void heap_init(void) {
  memset(g_heap_recs, 0, sizeof(g_heap_recs));
  kputs("[HEAP] virtually contiguous page heap ready (256 MiB window)\n");
}

void *kmalloc(UINT64 size) {
  UINT64 pages, i, offset = 0;
  void *first;
  heap_rec_t *rec = 0;

  if (size == 0) {
    return 0;
  }
  if (size > MM_HEAP_SIZE) {
    kputs("[HEAP] warning: allocation size too large\n");
    return 0;
  }
  pages = (size + MM_PAGE_SIZE - 1) / MM_PAGE_SIZE; /* round up to pages */

  for (i = 0; i < HEAP_MAX_ALLOCS; i++) {
    if (g_heap_recs[i].ptr == 0) {
      rec = &g_heap_recs[i];
      break;
    }
  }
  if (rec == 0) {
    kputs("[HEAP] warning: allocation table full, kmalloc failed\n");
    return 0;
  }

  /* First fit over live virtual ranges. Restart after each collision;
   * freed ranges are reusable, including after allocation failure. */
  for (i = 0; i < HEAP_MAX_ALLOCS; i++) {
    heap_rec_t *other = &g_heap_recs[i];
    UINT64 start;
    if (other->ptr == 0) continue;
    start = (UINT64)(UINTN)other->ptr - MM_HEAP_BASE;
    if (offset < start + other->pages * MM_PAGE_SIZE &&
        start < offset + pages * MM_PAGE_SIZE) {
      offset = start + other->pages * MM_PAGE_SIZE;
      i = (UINT64)-1;
    }
  }
  if (offset > MM_HEAP_SIZE - pages * MM_PAGE_SIZE ||
      mm_heap_map(offset, (UINT32)pages) != 0) {
    kputs("[HEAP] warning: out of memory, kmalloc failed\n");
    return 0;
  }
  first = (void *)(UINTN)(MM_HEAP_BASE + offset);
  memset(first, 0, pages * MM_PAGE_SIZE);

  rec->ptr = first;
  rec->pages = pages;
  return first;
}

void kfree(void *ptr) {
  UINT64 i;
  heap_rec_t *rec = 0;

  if (ptr == 0) {
    return;
  }
  for (i = 0; i < HEAP_MAX_ALLOCS; i++) {
    if (g_heap_recs[i].ptr == ptr) {
      rec = &g_heap_recs[i];
      break;
    }
  }
  if (rec == 0) {
    kputs("[HEAP] warning: kfree of unknown pointer\n");
    return;
  }
  mm_heap_unmap((UINT64)(UINTN)rec->ptr - MM_HEAP_BASE, (UINT32)rec->pages);
  rec->ptr = 0;
  rec->pages = 0;
}

void heap_selftest(void) {
  UINT64 before = pmm_free_count();
  UINT8 *p;
  UINT64 i, after;

  p = (UINT8 *)kmalloc(3 * MM_PAGE_SIZE); /* exactly 3 pages */
  if (p == 0) {
    kputs("[HEAP] selftest FAIL (kmalloc)\n");
    return;
  }

  /* Write a byte pattern across all 3 pages and read it back. */
  for (i = 0; i < 3 * MM_PAGE_SIZE; i++) {
    p[i] = (UINT8)((i + i / MM_PAGE_SIZE) & 0xFF);
  }
  for (i = 0; i < 3 * MM_PAGE_SIZE; i++) {
    if (p[i] != (UINT8)((i + i / MM_PAGE_SIZE) & 0xFF)) {
      kputs("[HEAP] selftest FAIL (pattern)\n");
      kfree(p);
      return;
    }
  }
  for (i = 0; i < 3; i++) {
    const UINT8 *physical = phys_to_virt(virt_to_phys(p + i * MM_PAGE_SIZE));
    if (physical[0] != (UINT8)i) {
      kputs("[HEAP] selftest FAIL (physical alias)\n");
      kfree(p);
      return;
    }
  }
  kfree(p);

  after = pmm_free_count();
  if (after == before) {
    kputs("[HEAP] selftest PASS\n");
  } else {
    kputs("[HEAP] selftest FAIL (leak)\n");
  }
}
