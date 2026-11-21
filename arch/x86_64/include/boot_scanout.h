/**
 * Novus OS - can the boot stage touch the firmware's scanout?
 *
 * The boot stage runs on the firmware's page tables, not the kernel's, so
 * the framebuffer is only addressable if those tables already cover it.
 * Until now that was assumed from the address alone ("below 4 GiB means the
 * identity map covers it"), which is wrong the moment a machine hands out a
 * PCIe aperture above 4 GiB: the splash and the firmware-logo capture then
 * skip themselves on hardware that could perfectly well show them.
 *
 * boot_scanout_map() answers the question the honest way, by walking the
 * live tables: every page of [phys, phys + size) is checked, and a missing
 * level is filled in with a fresh uncached identity mapping. It is
 * idempotent, so both the logo capture and the display capture can call it
 * for the same framebuffer.
 */

#ifndef NOVUS_BOOT_SCANOUT_H
#define NOVUS_BOOT_SCANOUT_H

#include "uefi.h"

/* Already covered by the firmware's own tables: use it as it stands. */
#define BOOT_SCANOUT_PRESENT 1
/* Was missing; now mapped by this call with the UC fallback (identity). */
#define BOOT_SCANOUT_MAPPED 2
/* Was missing; now mapped with a 2 MiB leaf selecting IA32_PAT entry 4
 * (write-combining). The kernel installs the same policy after its CR3
 * switch; this path is only taken when firmware already enabled CR4.PAT. */
#define BOOT_SCANOUT_MAPPED_WC 3
#define BOOT_SCANOUT_FAILED 0

/* Returns BOOT_SCANOUT_PRESENT, BOOT_SCANOUT_MAPPED, or BOOT_SCANOUT_FAILED
 * when no boot services are available or a level could not be built. */
int boot_scanout_map(void *boot_services, UINT64 phys, UINT64 size);

/* The testable core: walk `pml4` and make [phys, phys + size) present,
 * asking `alloc(ctx)` for any missing table level. `added` reports whether
 * anything was written (so the caller knows to reload CR3). Returns the same
 * codes as boot_scanout_map(). */
typedef UINT64 *(*boot_scanout_alloc_t)(void *ctx);

int boot_scanout_walk(UINT64 *pml4, UINT64 phys, UINT64 size,
                      boot_scanout_alloc_t alloc, void *ctx, int *added);

/* Bytes a GOP mode occupies, pitch-aware because PixelsPerScanLine often
 * exceeds the visible width. */
UINT64 boot_scanout_bytes(UINT32 pitch, UINT32 height);

#endif /* NOVUS_BOOT_SCANOUT_H */
