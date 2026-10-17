/**
 * Novus OS - physical memory manager (PMM), M1 Task 4.
 *
 * Bitmap allocator over the final UEFI memory map handed to kmain.
 * Free/used state is tracked in a fixed 256 KiB .bss bitmap (2 Mi pages
 * ~= 8 GiB of 4 KiB pages) and manipulated exclusively through the
 * novuslib-rs FFI primitives (lib/novuslib_rs.h). The mmap buffer is
 * EfiLoaderData pool memory and stays identity-mapped, so it is valid to
 * read here after ExitBootServices.
 *
 * This header is the frozen PMM API; the alloc/free/count functions may
 * be called by other M1 modules.
 */

#ifndef NOVUS_PMM_H
#define NOVUS_PMM_H

#include "uefi.h"

/* lib/ is not on the kernel include path (INC is
 * "-Iarch/x86_64/include -Ikernel"), so the FFI header is pulled in by
 * a path relative to this file's directory; that keeps the PMM
 * compilable without depending on a Makefile change. */
#include "../lib/novuslib_rs.h"

#ifndef NULL
#define NULL ((VOID *)0)
#endif

/* Parse the final UEFI memory map (buffer address, total size in bytes,
 * descriptor size) and build the allocation bitmap. Must be called once
 * before any other pmm_* function. */
void pmm_init(UINT64 mmap_addr, UINT64 map_size, UINT64 desc_size);

/* Allocate one 4 KiB page; returns its physical address, or NULL on
 * exhaustion / uninitialized manager. */
void *pmm_alloc_page(void);

/* Return one previously allocated page. Unaligned, out-of-range and
 * double frees are ignored. */
void pmm_free_page(void *phys);

/* Current free page count. */
UINT64 pmm_free_count(void);

/* Total pages covered by the manager: ram_top / 4096. */
UINT64 pmm_total_pages(void);

/* Reserved (unallocatable) pages: non-conventional map entries plus the
 * explicit reservations, plus anything beyond bitmap capacity. */
UINT64 pmm_reserved_pages(void);

/* Top of RAM: highest PhysicalStart + Pages * 4096, rounded up to 2 MiB. */
UINT64 pmm_ram_top(void);

/* Diagnostics: statistics line, Rust FFI proof line, and an alloc/free
 * regression over the reserved regions. Output via kputs, English. */
void pmm_selftest(void);

#endif /* NOVUS_PMM_H */
