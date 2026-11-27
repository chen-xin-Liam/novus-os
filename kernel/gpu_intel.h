/**
 * Novus OS - Intel integrated graphics bring-up.
 *
 * gpu.c locates the display controller and reports its resources; this module
 * claims the Intel device, maps its MMIO and aperture windows into the kernel
 * and identifies the generation. It deliberately does not re-program the
 * display pipeline: UEFI GOP has already set up a scanout, and re-timing a
 * working panel from scratch is the fastest way to blank a machine that was
 * booting fine. Software rendering keeps using that scanout; the GPU presents
 * every frame the kernel writes into it.
 */

#ifndef NOVUS_GPU_INTEL_H
#define NOVUS_GPU_INTEL_H

#include "uefi.h"

/* Generation numbers follow Intel's numbering, not PCI revision ids. */
#define INTEL_GEN_UNKNOWN 0u
#define INTEL_GEN7        7u
#define INTEL_GEN8        8u
#define INTEL_GEN9        9u
#define INTEL_GEN11       11u
#define INTEL_GEN12       12u
#define INTEL_GEN13       13u
#define INTEL_GEN14       14u

/* Called once after gpu_init(), before gpu2d_init(). Idempotent. */
void intel_gpu_init(void);

/* 1 when an Intel controller was claimed and its MMIO window is readable. */
int intel_gpu_ready(void);

/* Generation, device id and name for sysinfo and the 3D query ABI. */
UINT32 intel_gpu_generation(void);
UINT16 intel_gpu_device_id(void);
const char *intel_gpu_name(void);

/* Kernel virtual addresses of the claimed windows; 0 when unavailable. */
UINT64 intel_gpu_mmio(void);
UINT64 intel_gpu_aperture(void);
UINT64 intel_gpu_stolen_phys(void);
UINT64 intel_gpu_stolen_bytes(void);

#endif /* NOVUS_GPU_INTEL_H */
