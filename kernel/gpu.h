/**
 * Novus OS - display controller probe (GPU bring-up, first step).
 *
 * The kernel does not drive a GPU yet: it draws on whatever linear
 * framebuffer the firmware left behind (UEFI GOP). What this module adds
 * is the layer every later driver needs - find the display controller on
 * the PCI bus, name it, and prove that its registers can be reached from
 * the kernel.
 *
 * Two families are recognised by name: Intel integrated graphics (8086)
 * and AMD integrated graphics (1002). Both keep their memory-mapped
 * register window in BAR 0, which the kernel's identity map of the low
 * 4 GiB already covers, so a probe needs no extra mapping and no
 * per-vendor code path.
 */

#ifndef NOVUS_GPU_H
#define NOVUS_GPU_H

#include "uefi.h"

#define GPU_VENDOR_NONE  0x0000u
#define GPU_VENDOR_INTEL 0x8086u
#define GPU_VENDOR_AMD   0x1002u
#define GPU_VENDOR_OTHER 0xFFFFu

/* Find the display controller, name it, and check that BAR 0 answers.
 * Call once, after the PCI bus is usable and before the desktop is
 * painted; it is harmless to call twice. */
void gpu_init(void);

/* The PCI identity of the controller that was found. */
UINT16 gpu_vendor(void);
UINT16 gpu_device(void);

/* "Intel UHD Graphics 630 · 8086:3E9B · MMIO 4 MiB", or
 * "标准 VGA · 1234:1111" when the machine has no recognisable GPU.
 * Never NULL. */
const char *gpu_summary(void);

/* Non-zero once a display controller was located. */
int gpu_present(void);

#endif /* NOVUS_GPU_H */
