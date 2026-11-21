/**
 * Novus OS - x86_64 paging setup and direct physical map (README 3.3).
 */

#ifndef NOVUS_MM_H
#define NOVUS_MM_H

#include "uefi.h"

/* Direct physical map window: after mm_init() every RAM address in
 * [0, ram_top) is reachable at PHYSMAP_BASE + phys through PML4[256],
 * built from 2 MiB huge pages (README 3.3 virtual memory & protection). */
#define PHYSMAP_BASE 0xFFFF800000000000ULL

/* Page granularity used across the whole memory subsystem. */
#define MM_PAGE_SIZE 4096ULL
#define MM_HEAP_BASE 0xFFFF810000000000ULL
#define MM_HEAP_SIZE (256ULL * 1024 * 1024)

/* The heap window is supervisor-only and shared by every process CR3.
 * Mapping failure rolls back every newly allocated physical page. */
int mm_heap_map(UINT64 offset, UINT32 pages);
void mm_heap_unmap(UINT64 offset, UINT32 pages);

/* Build the page-table hierarchy (identity map of the described physical
 * address space through PML4[0] plus the direct physical map of
 * [0, ram_top_bytes) through PML4[256]), switch CR3 and run the physmap
 * read-back selftest.
 *
 * [mmap_addr, mmap_addr + map_size) with `desc_size` is the UEFI memory map
 * still valid at call time. RAM descriptors are mapped write-back; device
 * MMIO, the legacy hole and every gap stay uncached, so a BAR reached
 * through its raw physical address is never cached.
 *
 * [fb_phys, fb_phys + fb_size) - zero when there is no captured scanout -
 * is identity-mapped write-combining on the way, so the boot text console,
 * which draws through the framebuffer's physical address, keeps working
 * across the CR3 switch instead of having to go dark inside this function. */
void mm_init(UINT64 ram_top_bytes, UINT64 mmap_addr, UINT64 map_size,
             UINT64 desc_size, UINT64 fb_phys, UINT64 fb_size);

/* End of the identity-mapped window (exclusive). A physical address below
 * this can be used as a pointer as it stands; one above it has to go
 * through mm_map_phys() first. */
UINT64 mm_ram_top(void);

/* Current CR3 value, read with inline assembly. */
UINT64 mm_get_cr3(void);

/* The kernel's own PML4, saved by mm_init(): the template every process
 * space is cloned from. Callers must not use mm_get_cr3() for that. */
UINT64 mm_kernel_cr3(void);

/* Direct-map conversions. phys_to_virt maps into the physmap window;
 * virt_to_phys reverses it and passes identity-mapped low addresses
 * (kernel image) through unchanged. */
void *phys_to_virt(UINT64 phys);
UINT64 virt_to_phys(const void *v);

/* Map `size` bytes of physical memory (any address, including one above
 * ram_top such as a device BAR or a framebuffer) into the kernel virtual
 * space and return a linear pointer, or NULL on failure.
 *
 * MM_MAP_UC marks the range uncacheable: correct for device registers, where
 * every access must reach the hardware in order.
 * MM_MAP_WC marks it write-combining: still uncached for reads, but
 * consecutive writes are merged into full cache-line bursts before they leave
 * the CPU. That is the right mode for a scanout, and it is the difference
 * between one PCIe transaction per pixel and one per cache line on a card
 * whose framebuffer sits above 4 GiB. */
#define MM_MAP_UC 0x01u
#define MM_MAP_WC 0x02u
void *mm_map_phys(UINT64 phys, UINT64 size, UINT32 flags);

/* Turn on CR4.PAT and make sure IA32_PAT has the standard layout, so that
 * MM_MAP_WC's PTE bit means write-combining instead of a reserved-bit fault.
 * Call once, before the first WC mapping (mm_init does it). */
void mm_enable_pat(void);

/* 1 when CR4.PAT is set and IA32_PAT entry 4 really means write-combining.
 * 0 on a CPU that does not advertise PAT (mm_map_phys then maps MM_MAP_WC as
 * uncached). */
int mm_pat_available(void);

/* Read the same physical byte (kernel .text region) through the identity
 * map and through PHYSMAP_BASE and report PASS/FAIL on the console. */
void mm_selftest(void);

#endif /* NOVUS_MM_H */
