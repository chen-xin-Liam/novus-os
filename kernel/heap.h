/**
 * Novus OS - page-granular kernel heap (README 3.1 memory partition 4).
 */

#ifndef NOVUS_HEAP_H
#define NOVUS_HEAP_H

#include "uefi.h"

/* Reset the allocation accounting table and print the readiness banner. */
void heap_init(void);

/* Allocate ceil(size / 4 KiB) whole zeroed pages; the physical pages are
 * NOT required to be contiguous. Returns NULL on failure (table full or
 * PMM exhausted). */
void *kmalloc(UINT64 size);

/* Release a block obtained from kmalloc(), page by page. */
void kfree(void *ptr);

/* Allocate a 3-page block, write and verify a byte pattern across it,
 * free it and check the PMM free count is restored; prints PASS/FAIL. */
void heap_selftest(void);

#endif /* NOVUS_HEAP_H */
