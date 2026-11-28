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

#define GPU_VENDOR_NONE   0x0000u
#define GPU_VENDOR_INTEL  0x8086u
#define GPU_VENDOR_AMD    0x1002u
#define GPU_VENDOR_NVIDIA 0x10DEu
#define GPU_VENDOR_OTHER  0xFFFFu

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

/* Second step: the scanout the firmware left behind, and whether the
 * controller's own aperture covers it. gpu_framebuffer_bar() returns the
 * PCI BAR index that contains the pixels, or -1 when they live in memory
 * no controller window reaches - the difference between "a driver could
 * take over this scanout" and "the framebuffer is just system RAM". */
UINT64 gpu_framebuffer_phys(void);
UINT64 gpu_framebuffer_bytes(void);
int    gpu_framebuffer_bar(void);

/* Third step: the two windows any driver has to know about before it can
 * touch the controller. gpu_mmio_phys() is BAR 0 (the register file),
 * gpu_aperture_phys() is BAR 2 (the window through which the GPU's own
 * memory is visible - GTT aperture on Intel, VRAM/framebuffer aperture on
 * AMD). Both are 0 when that BAR is missing or sits above the identity
 * map. gpu_mmio_is_64bit() says whether the register window is a 64-bit
 * BAR, which is what decides if a driver can move it freely. */
UINT64 gpu_mmio_phys(void);
UINT64 gpu_mmio_bytes(void);
UINT64 gpu_aperture_phys(void);
UINT64 gpu_aperture_bytes(void);
int    gpu_mmio_is_64bit(void);

/* Intel only, and only when the firmware programmed it: the base of the
 * stolen graphics memory the BIOS carved out of system RAM (PCI config
 * 0x5C, BDSM). Integrated Intel graphics keep their own buffers there
 * before any driver allocates anything, so a driver that ignores it writes
 * into memory the firmware believes it owns. Zero on AMD and on a machine
 * where the register is empty. */
UINT64 gpu_stolen_phys(void);
UINT64 gpu_stolen_bytes(void);

/* Hardware acceleration, P1: the graphics generation the controller
 * reports (GMD_ID, or the PCI family fallback) and whether its register
 * window is mapped and readable. A driver may only touch the controller
 * when gpu_intel_ready() is non-zero; otherwise everything stays on the
 * software 2D path. gpu_probe_line() is a short "intel gen=12" string for
 * the on-screen status, never NULL, empty on non-Intel machines. */
int         gpu_intel_gen(void);
int         gpu_intel_ready(void);
const char *gpu_probe_line(void);

/* Non-zero once a display controller was located. */
int gpu_present(void);

#endif /* NOVUS_GPU_H */
