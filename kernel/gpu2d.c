/**
 * Novus OS - the 2D engine seam. See gpu2d.h.
 */

#include "kernel.h"
#include "gpu2d.h"
#include "gpu.h"

#define GPU2D_ENODEV (-19)

typedef struct {
  const char *name;
  /* Bring-up. Returns 0 when this backend can actually drive the engine it
   * names, non-zero otherwise; a backend is only selected on 0. */
  int (*init)(void);
  void (*fill_span)(UINT32 *row, UINT32 count, UINT32 hw);
  void (*copy_row)(UINT32 *dst, const UINT32 *src, UINT32 count);
  void (*blit)(UINT32 *dst, UINT32 dstride, const UINT32 *src, UINT32 sstride,
               UINT32 w, UINT32 h);
} gpu2d_ops_t;

static const gpu2d_ops_t *g_ops;
static UINT64             g_spans;
static UINT64             g_blits;

/* ------------------------------------------------------------------ CPU ---
 *
 * The software backend. Two things matter here and nothing else:
 *
 * 1. Write the widest word the destination can take. A framebuffer row that
 *    starts on an 8-byte boundary takes two pixels per store; over uncached
 *    memory (the framebuffer is mapped UC) that halves the number of writes
 *    that leave the CPU, which is the actual cost.
 * 2. Never branch per pixel. The tail is handled once, after the body.
 */
static void soft_fill_span(UINT32 *row, UINT32 count, UINT32 hw) {
  UINT64 pair = ((UINT64)hw << 32) | (UINT64)hw;
  UINT32 n;

  if (count == 0u) {
    return;
  }
  /* An unaligned 64-bit store to UC memory is split by the CPU into two
   * accesses, so it buys nothing; fall back to words. */
  if (((UINTN)row & 7u) == 0u && count >= 2u) {
    UINT64 *p = (UINT64 *)row;
    UINT32  pairs = count >> 1;

    n = pairs;
    while (n >= 4u) {
      p[0] = pair;
      p[1] = pair;
      p[2] = pair;
      p[3] = pair;
      p += 4;
      n -= 4u;
    }
    while (n-- > 0u) {
      *p++ = pair;
    }
    if ((count & 1u) != 0u) {
      *row = hw;                      /* the odd tail pixel, one store */
      return;
    }
    return;
  }
  n = count;
  while (n-- > 0u) {
    *row++ = hw;
  }
}

static void soft_copy_row(UINT32 *dst, const UINT32 *src, UINT32 count) {
  UINT64       *d;
  const UINT64 *s;
  UINT32        n;

  if (count == 0u) {
    return;
  }
  if ((((UINTN)dst | (UINTN)src) & 7u) == 0u && count >= 2u) {
    d = (UINT64 *)dst;
    s = (const UINT64 *)src;
    n = count >> 1;
    while (n >= 4u) {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = s[3];
      d += 4;
      s += 4;
      n -= 4u;
    }
    while (n-- > 0u) {
      *d++ = *s++;
    }
    if ((count & 1u) != 0u) {
      dst += count - 1u;
      src += count - 1u;
      *dst = *src;
    }
    return;
  }
  while (count-- > 0u) {
    *dst++ = *src++;
  }
}

static void soft_blit(UINT32 *dst, UINT32 dstride, const UINT32 *src,
                      UINT32 sstride, UINT32 w, UINT32 h) {
  UINT32 y;

  for (y = 0; y < h; y++) {
    soft_copy_row(dst + (UINTN)y * dstride, src + (UINTN)y * sstride, w);
  }
}

static int soft_init(void) { return 0; }

static const gpu2d_ops_t g_backend_soft = {
  "software",
  soft_init,
  soft_fill_span,
  soft_copy_row,
  soft_blit,
};

/* -------------------------------------------------------------- vendors ---
 *
 * One entry per family. Each is the place a real driver will land, and each
 * returns ENODEV today: the controller's registers may be reachable, but a
 * 2D engine is driven through a command stream (Intel: ring + XY_SRC_COPY /
 * XY_COLOR_BLT, AMD: ring + DCN, NVIDIA: pushbuffer + a GSP-negotiated
 * channel), and none of those exist yet. Reporting "no device" keeps the
 * compositor on the software path instead of writing garbage into the
 * framebuffer.
 */

/* Intel Gen9..Xe: needs the graphics ring, the GTT and the display engine
 * programmed by us rather than by the firmware. Not started. */
static int intel_init(void) {
  if (gpu_vendor() != GPU_VENDOR_INTEL || gpu_mmio_phys() == 0u) {
    return GPU2D_ENODEV;
  }
  kputs("[GPU2D] Intel 2D engine: registers reachable, command stream not "
        "written yet (needs ring + GTT)\n");
  return GPU2D_ENODEV;
}

/* AMD GCN..RDNA: needs the ring, the IH and, on APUs, the DCN block. */
static int amd_init(void) {
  if (gpu_vendor() != GPU_VENDOR_AMD || gpu_mmio_phys() == 0u) {
    return GPU2D_ENODEV;
  }
  kputs("[GPU2D] AMD 2D engine: registers reachable, command stream not "
        "written yet (needs ring + IH)\n");
  return GPU2D_ENODEV;
}

/* NVIDIA Turing..Blackwell: the display engine cannot be driven at all
 * before the GSP firmware has been booted and a channel negotiated, and
 * that firmware is signed vendor code we do not ship. Until then a
 * GeForce behaves exactly like the firmware framebuffer: the CPU draws.
 * This is the 5070 case, and it is the honest answer for any 50-series
 * card today. */
static int nvidia_init(void) {
  if (gpu_vendor() != GPU_VENDOR_NVIDIA) {
    return GPU2D_ENODEV;
  }
  kputs("[GPU2D] NVIDIA 2D engine: needs GSP firmware + a negotiated channel; "
        "falling back to the CPU over the firmware framebuffer\n");
  return GPU2D_ENODEV;
}

typedef struct {
  const char *name;
  int (*init)(void);
} gpu2d_vendor_t;

static const gpu2d_vendor_t g_vendors[] = {
  {"intel", intel_init},
  {"amd", amd_init},
  {"nvidia", nvidia_init},
};

void gpu2d_init(void) {
  UINT32 i;

  if (g_ops != 0) {
    return;
  }
  /* Order matters: a real engine always wins over the CPU when it says it
   * can drive the hardware. */
  for (i = 0; i < sizeof(g_vendors) / sizeof(g_vendors[0]); i++) {
    int rc = g_vendors[i].init();

    (void)rc;
  }
  g_ops = &g_backend_soft;
  kputs("[GPU2D] backend=");
  kputs(g_ops->name);
  kputs(" (64-bit span stores; no vendor engine wired up)\n");
}

const char *gpu2d_backend(void) {
  return (g_ops != 0) ? g_ops->name : "none";
}

int gpu2d_hardware(void) {
  return (g_ops != 0 && g_ops != &g_backend_soft) ? 1 : 0;
}

void gpu2d_fill_span(UINT32 *row, UINT32 count, UINT32 hw) {
  if (g_ops == 0) {
    gpu2d_init();
  }
  g_spans++;
  g_ops->fill_span(row, count, hw);
}

void gpu2d_copy_row(UINT32 *dst, const UINT32 *src, UINT32 count) {
  if (g_ops == 0) {
    gpu2d_init();
  }
  g_ops->copy_row(dst, src, count);
}

void gpu2d_blit(UINT32 *dst, UINT32 dstride, const UINT32 *src, UINT32 sstride,
                UINT32 w, UINT32 h) {
  if (g_ops == 0) {
    gpu2d_init();
  }
  g_blits++;
  g_ops->blit(dst, dstride, src, sstride, w, h);
}

UINT64 gpu2d_span_count(void) { return g_spans; }
UINT64 gpu2d_blit_count(void) { return g_blits; }
