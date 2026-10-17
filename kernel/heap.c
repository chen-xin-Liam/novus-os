/**
 * Novus OS - page-granular kernel heap (README 3.1 memory partition 4).
 *
 * M1 keeps the heap deliberately simple: kmalloc() rounds the request up
 * to whole 4 KiB pages and hands out a run of fresh pages from the VMM;
 * kfree() releases the run again. No sub-page carving yet, so every
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
#include "vmm.h"
#include "pmm.h"

#define HEAP_MAX_ALLOCS 256

typedef struct {
  void  *ptr;   /* first virtual page of the block */
  UINT64 pages; /* number of pages in the block */
} heap_rec_t;

static heap_rec_t g_heap_recs[HEAP_MAX_ALLOCS];

void heap_init(void) {
  memset(g_heap_recs, 0, sizeof(g_heap_recs));
  kputs("[HEAP] page-granular heap ready\n");
}

void *kmalloc(UINT64 size) {
  UINT64 pages, i;
  void *first;
  heap_rec_t *rec = 0;

  if (size == 0) {
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

  first = vmm_alloc_page();
  if (first == 0) {
    kputs("[HEAP] warning: out of memory, kmalloc failed\n");
    return 0;
  }
  for (i = 1; i < pages; i++) {
    if (vmm_alloc_page() == 0) {
      /* Roll back the pages taken so far. */
      UINT64 j;
      for (j = 0; j < i; j++) {
        vmm_free_page((UINT8 *)first + j * MM_PAGE_SIZE);
      }
      kputs("[HEAP] warning: out of memory, kmalloc failed\n");
      return 0;
    }
  }

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
  for (i = 0; i < rec->pages; i++) {
    vmm_free_page((UINT8 *)rec->ptr + i * MM_PAGE_SIZE);
  }
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
    p[i] = (UINT8)(i & 0xFF);
  }
  for (i = 0; i < 3 * MM_PAGE_SIZE; i++) {
    if (p[i] != (UINT8)(i & 0xFF)) {
      kputs("[HEAP] selftest FAIL (pattern)\n");
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
