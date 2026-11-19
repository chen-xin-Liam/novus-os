/**
 * Novus OS - display controller probe. See gpu.h.
 */

#include "kernel.h"
#include "gpu.h"
#include "pci.h"
#include "nd.h"

/* A handful of the integrated controllers people actually run Novus on.
 * Unknown ids still get the vendor's family name, so a new part shows up
 * as "Intel 显示控制器" rather than as nothing. */
typedef struct {
  UINT16      id;
  const char *name;
} gpu_name_t;

static const gpu_name_t g_intel_names[] = {
  {0x3E9Bu, "Intel UHD Graphics 630"},
  {0x3E92u, "Intel UHD Graphics 630"},
  {0x9A49u, "Intel Iris Xe Graphics"},
  {0x9A60u, "Intel UHD Graphics (Tiger Lake)"},
  {0x46A6u, "Intel UHD Graphics (Alder Lake)"},
  {0x46D0u, "Intel UHD Graphics 770"},
  {0x4680u, "Intel UHD Graphics (Alder Lake)"},
  {0xA780u, "Intel Iris Xe Graphics (Raptor Lake)"},
  {0xA7A0u, "Intel UHD Graphics 710"},
  {0x7D40u, "Intel Iris Xe Graphics (Meteor Lake)"},
  {0x7D55u, "Intel Arc Graphics (Meteor Lake)"},
  {0x1912u, "Intel HD Graphics 530"},
  {0x5912u, "Intel HD Graphics 630"},
  {0x3185u, "Intel UHD Graphics 630 (Pentium)"},
  {0x8A56u, "Intel UHD Graphics (Ice Lake)"},
  {0x9B41u, "Intel UHD Graphics (Comet Lake)"},
};

static const gpu_name_t g_amd_names[] = {
  {0x15D8u, "AMD Radeon Graphics (Picasso)"},
  {0x15DDu, "AMD Radeon Vega 8 (Raven)"},
  {0x1636u, "AMD Radeon Graphics (Renoir)"},
  {0x1638u, "AMD Radeon Graphics (Renoir)"},
  {0x164Cu, "AMD Radeon Graphics (Lucienne)"},
  {0x164Du, "AMD Radeon Graphics (Renoir)"},
  {0x15E7u, "AMD Radeon Graphics (Barcelo)"},
  {0x1681u, "AMD Radeon 680M (Rembrandt)"},
  {0x1506u, "AMD Radeon 780M (Phoenix)"},
  {0x150Eu, "AMD Radeon Graphics (Phoenix)"},
  {0x1114u, "AMD Radeon Graphics (Mendocino)"},
  {0x1900u, "AMD Radeon Graphics (Strix)"},
  {0x1901u, "AMD Radeon 890M (Strix)"},
  {0x98E4u, "AMD Radeon Graphics (Carrizo)"},
  {0x9874u, "AMD Radeon R7 (Bristol)"},
  {0x1309u, "AMD Radeon R5 (Kaveri)"},
};

static UINT16     g_vendor;
static UINT16     g_device;
static int        g_present;
static UINT32     g_mmio_bytes;
static int        g_mmio_ok;
static char       g_summary[64];

/* Where the pixels the compositor writes actually live, and which PCI
 * window a driver would have to reach them through. A real driver has to
 * know this before it touches anything: a scanout inside the controller's
 * own aperture is memory the GPU can also read, which is what makes
 * hardware blits and page flips possible at all. */
static UINT64     g_fb_phys;
static UINT64     g_fb_bytes;
static int        g_fb_bar = -1;   /* index of the BAR that covers it */
static UINT64     g_scanout_base;
static UINT64     g_scanout_bytes;

/* One register read from the controller's MMIO window. A window that
 * reads back all ones is not decoded - the firmware left it unassigned,
 * or the BAR is above the identity map and cannot be reached yet. */
static UINT32 mmio_read(UINT64 base, UINT32 off) {
  return *(volatile UINT32 *)(UINTN)(base + off);
}

static const char *name_for(UINT16 vendor, UINT16 device) {
  UINT32 i;

  if (vendor == GPU_VENDOR_INTEL) {
    for (i = 0; i < sizeof(g_intel_names) / sizeof(g_intel_names[0]); i++) {
      if (g_intel_names[i].id == device) {
        return g_intel_names[i].name;
      }
    }
    return "Intel 显示控制器";
  }
  if (vendor == GPU_VENDOR_AMD) {
    for (i = 0; i < sizeof(g_amd_names) / sizeof(g_amd_names[0]); i++) {
      if (g_amd_names[i].id == device) {
        return g_amd_names[i].name;
      }
    }
    return "AMD Radeon 显示控制器";
  }
  return 0;
}

void gpu_init(void) {
  pci_dev_t list[PCI_MAX_DEVICES];
  pci_dev_t d;
  pci_dev_t first;
  int       n;
  int       i;
  int       have = 0;
  int       have_first = 0;
  const char *name;
  UINT64      base;

  if (g_present != 0) {
    return;
  }
  /* Class 03h is "display controller". Integrated controllers usually
   * declare subclass 80h ("other"), the firmware VGA device 00h. Walk the
   * whole bus once and prefer Intel or AMD when the machine has more than
   * one: a discrete card sitting next to the integrated one must not hide
   * it. */
  n = pci_scan(list, PCI_MAX_DEVICES);
  for (i = 0; i < n; i++) {
    if (list[i].class_code != 0x03u) {
      continue;
    }
    if (have_first == 0) {
      first = list[i];
      have_first = 1;
    }
    if (list[i].vendor == GPU_VENDOR_INTEL ||
        list[i].vendor == GPU_VENDOR_AMD) {
      d = list[i];
      have = 1;
      break;
    }
  }
  if (have == 0) {
    if (have_first == 0) {
      g_vendor = GPU_VENDOR_NONE;
      g_device = 0xFFFFu;
      g_summary[0] = 0;
      kputs("[GPU] no display controller on the PCI bus\n");
      return;
    }
    d = first;
  }
  g_present = 1;
  g_vendor = d.vendor;
  g_device = d.device;
  base = d.bar[0];
  g_mmio_bytes = (UINT32)d.bar_size[0];

  name = name_for(d.vendor, d.device);
  if (base != 0u && (base >> 32) == 0u && g_mmio_bytes != 0u) {
    /* Reading the window is the whole point of the probe: a driver can
     * only be built on a BAR that answers. The first dword of both Intel
     * and AMD register windows is not a write-sensitive register, so
     * reading it leaves the controller as it was. */
    UINT32 v = mmio_read(base, 0u);
    g_mmio_ok = (v != 0xFFFFFFFFu) ? 1 : 0;
    kputs("[GPU] ");
    kputs(name != 0 ? name : "显示控制器");
    kputs(" vendor=");
    kputs_hex(d.vendor);
    kputs(" device=");
    kputs_hex(d.device);
    kputs(" mmio=");
    kputs_hex(base);
    kputs(" size=");
    kputs_hex(g_mmio_bytes);
    kputs(" reg0=");
    kputs_hex(v);
    kputs(g_mmio_ok != 0 ? " (reachable)\n" : " (all ones!)\n");
  } else {
    kputs("[GPU] ");
    kputs(name != 0 ? name : "显示控制器");
    kputs(" vendor=");
    kputs_hex(d.vendor);
    kputs(" device=");
    kputs_hex(d.device);
    kputs(" has no reachable BAR 0\n");
  }

  /* Second step: locate the scanout. The firmware handed us a linear
   * framebuffer; the question a driver asks next is whether that memory is
   * inside one of the controller's BAR windows. It is when the GPU owns
   * the display path (the usual case on integrated graphics, where the
   * aperture is system memory), and it is not when the firmware left the
   * scanout in plain RAM or on a discrete card's own memory. */
  {
    const nd_output_t *outs = 0;
    UINT32             count = 0;
    UINT32             i;
    UINT32             k;

    outs = nd_outputs(&count);
    if (outs != 0 && count > 0u) {
      g_fb_phys = outs[0].fb_phys;
      g_fb_bytes = outs[0].fb_size;
      for (k = 0; k < 6u; k++) {
        UINT64 lo = d.bar[k];
        UINT64 hi = lo + (UINT64)d.bar_size[k];

        if (d.bar_size[k] == 0u || lo == 0u) {
          continue;
        }
        if (g_fb_phys >= lo && g_fb_phys + g_fb_bytes <= hi) {
          g_fb_bar = (int)k;
          /* The aperture proper: how much room the controller advertises
           * on that window, which is what a memory manager budgets with. */
          g_scanout_base = lo;
          g_scanout_bytes = (UINT64)d.bar_size[k];
          break;
        }
      }
      kputs("[GPU] framebuffer=");
      kputs_hex(g_fb_phys);
      kputs(" size=");
      kputs_hex(g_fb_bytes);
      if (g_fb_bar >= 0) {
        kputs(" inside BAR ");
        kputs_hex((UINT64)g_fb_bar);
        kputs(" (gpu aperture ");
        kputs_hex(g_scanout_bytes);
        kputs(")\n");
      } else {
        kputs(" outside every BAR (firmware linear framebuffer)\n");
      }
    }
    i = count;
    (void)i;
  }

  /* Build the one-line summary the settings page shows. It travels in an
   * IPC reply whose payload is 128 bytes, so the room left for it is
   * small: the chip's name only, cut on a UTF-8 boundary so a mixed
   * Chinese/Latin name never ends in half a character. The vendor:device
   * pair stays in the boot log. */
  {
    const char *s = name != 0 ? name : "显示控制器";
    UINT32      i = 0u;
    UINT32      n = 0u;

    while (s[n] != '\0') {
      unsigned char c = (unsigned char)s[n];
      UINT32        w = (c < 0x80u) ? 1u
                       : ((c & 0xE0u) == 0xC0u) ? 2u
                       : ((c & 0xF0u) == 0xE0u) ? 3u
                                                 : 4u;

      if (n + w > 20u) {
        break;
      }
      n += w;
    }
    while (i < n) {
      g_summary[i] = s[i];
      i++;
    }
    g_summary[i] = 0;
  }
}

UINT16 gpu_vendor(void) { return g_vendor; }
UINT16 gpu_device(void) { return g_device; }
int    gpu_present(void) { return g_present; }

UINT64 gpu_framebuffer_phys(void) { return g_fb_phys; }
UINT64 gpu_framebuffer_bytes(void) { return g_fb_bytes; }

/* Index of the BAR that covers the scanout, or -1 when the pixels sit in
 * memory no controller window reaches. */
int gpu_framebuffer_bar(void) { return g_fb_bar; }

const char *gpu_summary(void) {
  if (g_present == 0) {
    return "未检测到显示控制器";
  }
  return g_summary;
}
