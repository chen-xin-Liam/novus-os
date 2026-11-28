/**
 * Novus OS - Intel integrated graphics bring-up. See gpu_intel.h.
 *
 * The controller is already displaying the UEFI GOP framebuffer when the
 * kernel starts. This driver therefore claims the device rather than
 * re-timing the panel: memory decoding and bus mastering are enabled, both
 * PCI windows are mapped into kernel virtual space, and the generation is
 * identified from the device id. Nothing writes to the display pipeline or
 * the render ring until a command stream exists, so a claim failure can
 * never leave the machine with a black screen.
 */

#include "kernel.h"
#include "gpu.h"
#include "gpu_intel.h"
#include "pci.h"
#include "mm.h"

typedef struct {
  UINT16    id;
  UINT32    gen;
  const char *name;
} intel_id_t;

/* Common i5/UHD/Iris parts. An unknown Intel id still gets a family name
 * and generation 0: the resources are usable even when the name is not. */
static const intel_id_t g_ids[] = {
  {0x0412u, INTEL_GEN7,  "Intel HD Graphics 4000"},
  {0x041Eu, INTEL_GEN7,  "Intel HD Graphics 4600"},
  {0x1612u, INTEL_GEN8,  "Intel HD Graphics 5600"},
  {0x161Eu, INTEL_GEN8,  "Intel HD Graphics 6200"},
  {0x1622u, INTEL_GEN8,  "Intel Iris Pro Graphics 6200"},
  {0x1902u, INTEL_GEN9,  "Intel HD Graphics 530"},
  {0x1912u, INTEL_GEN9,  "Intel HD Graphics 530"},
  {0x1932u, INTEL_GEN9,  "Intel HD Graphics 530"},
  {0x1942u, INTEL_GEN9,  "Intel HD Graphics 510"},
  {0x5912u, INTEL_GEN9,  "Intel HD Graphics 630"},
  {0x5917u, INTEL_GEN9,  "Intel UHD Graphics 620"},
  {0x5926u, INTEL_GEN9,  "Intel HD Graphics 630"},
  {0x3E92u, INTEL_GEN9,  "Intel UHD Graphics 630"},
  {0x3E9Bu, INTEL_GEN9,  "Intel UHD Graphics 630"},
  {0x9B41u, INTEL_GEN9,  "Intel UHD Graphics 630"},
  {0x9BC5u, INTEL_GEN9,  "Intel UHD Graphics 630"},
  {0x8A56u, INTEL_GEN11, "Intel UHD Graphics (Ice Lake)"},
  {0x8A5Au, INTEL_GEN11, "Intel Iris Plus Graphics (Ice Lake)"},
  {0x8A71u, INTEL_GEN11, "Intel Iris Plus Graphics G7"},
  {0x9A49u, INTEL_GEN12, "Intel Iris Xe Graphics"},
  {0x9A60u, INTEL_GEN12, "Intel UHD Graphics (Tiger Lake)"},
  {0x4680u, INTEL_GEN12, "Intel UHD Graphics (Alder Lake)"},
  {0x46A6u, INTEL_GEN12, "Intel UHD Graphics 770"},
  {0x46D0u, INTEL_GEN12, "Intel UHD Graphics 770"},
  {0xA780u, INTEL_GEN12, "Intel Iris Xe Graphics (Raptor Lake)"},
  {0xA7A0u, INTEL_GEN12, "Intel UHD Graphics 710"},
  {0x7D40u, INTEL_GEN13, "Intel Iris Xe Graphics (Meteor Lake)"},
  {0x7D55u, INTEL_GEN13, "Intel Arc Graphics (Meteor Lake)"},
  {0x7D67u, INTEL_GEN14, "Intel Graphics (Arrow Lake)"},
};

static int        g_ready;
static UINT32     g_gen;
static UINT16     g_device;
static const char *g_name = "Intel 显示控制器";
static UINT64     g_mmio_va;
static UINT64     g_aperture_va;
static UINT64     g_stolen_phys;
static UINT64     g_stolen_bytes;

static const intel_id_t *lookup(UINT16 id) {
  UINT32 i;

  for (i = 0; i < sizeof(g_ids) / sizeof(g_ids[0]); i++) {
    if (g_ids[i].id == id) {
      return &g_ids[i];
    }
  }
  return 0;
}

/* GGC lives in PCI configuration space at 0x52; bits 0..2 are the stolen
 * memory size code, and BDSM at 0x5C holds its base. Reading 0x108040
 * through the BAR0 mapping is wrong on Gen8+: that offset belongs to the
 * separate GGTT MMIO window and is often outside the 16 KiB BAR0. On a real
 * i5 that read faults while starting the kernel, so use config space only. */
static void read_stolen(const pci_dev_t *dev) {
  UINT8 gcc;
  UINT32 mb;

  if (dev == 0) {
    return;
  }
  gcc = pci_read8(dev->bus, dev->dev, dev->func, 0x52u);
  switch (gcc & 7u) {
  case 1u: mb = 32u;  break;
  case 2u: mb = 64u;  break;
  case 3u: mb = 128u; break;
  case 4u: mb = 256u; break;
  case 5u: mb = 512u; break;
  default: mb = 0u;   break;
  }
  if (mb != 0u) {
    g_stolen_bytes = (UINT64)mb * 1024u * 1024u;
    g_stolen_phys  = gpu_stolen_phys();
  }
}

void intel_gpu_init(void) {
  UINT64 mmio_phys;
  UINT64 mmio_bytes;
  UINT64 aper_phys;
  UINT64 aper_bytes;
  const intel_id_t *id;
  pci_dev_t dev;
  int found;
  UINT32 probe;

  if (g_ready || gpu_vendor() != GPU_VENDOR_INTEL) {
    return;
  }
  g_device = gpu_device();
  id = lookup(g_device);
  if (id != 0) {
    g_gen  = id->gen;
    g_name = id->name;
  }

  /* Claim the device. gpu_init() may have found it through the framebuffer
   * BAR instead of by id, so look it up by the id it reported. */
  found = (pci_find(GPU_VENDOR_INTEL, g_device, &dev) == 0);
  if (found) {
    pci_enable_device(&dev);
  }

  mmio_phys  = gpu_mmio_phys();
  mmio_bytes = gpu_mmio_bytes();
  if (mmio_bytes < 4096u) {
    mmio_bytes = 4096u;
  }
  if (mmio_phys != 0u) {
    g_mmio_va = (UINT64)(UINTN)mm_map_phys(mmio_phys, mmio_bytes, MM_MAP_UC);
  }

  /* A BAR that reads back all ones is unassigned or not decoded. Never
   * announce a driver on such a window: a later command submission would
   * write into whatever happens to live at that address. */
  if (g_mmio_va != 0u) {
    probe = *(volatile UINT32 *)(UINTN)g_mmio_va;
    if (probe != 0xFFFFFFFFu) {
      g_ready = 1;
    }
  }

  aper_phys  = gpu_aperture_phys();
  aper_bytes = gpu_aperture_bytes();
  if (g_ready && aper_phys != 0u && aper_bytes >= 4096u) {
    /* Only the first 64 MiB is mapped: that is more than enough for a GTT
     * plus a few frame buffers, and keeps the devmap window bounded. */
    if (aper_bytes > 64u * 1024u * 1024u) {
      aper_bytes = 64u * 1024u * 1024u;
    }
    g_aperture_va = (UINT64)(UINTN)mm_map_phys(aper_phys, aper_bytes,
                                               MM_MAP_WC);
  }
  read_stolen(found ? &dev : 0);

  kputs("[INTEL-GPU] ");
  kputs(g_name);
  kputs(" device=0x");
  kputs_hex(g_device);
  kputs(" gen=");
  kputs_hex(g_gen);
  kputs(" ready=");
  kputs_hex((UINT64)g_ready);
  kputs("\n");
  if (g_ready) {
    kputs("[INTEL-GPU] MMIO=0x");
    kputs_hex(g_mmio_va);
    kputs(" aperture=0x");
    kputs_hex(g_aperture_va);
    kputs(" stolen=0x");
    kputs_hex(g_stolen_phys);
    kputs(" bytes=");
    kputs_hex(g_stolen_bytes);
    kputs("\n");
  }
  kputs("[INTEL-GPU] scanout remains on the GOP framebuffer; the 3D engine "
        "renders into scanout memory\n");
}

int intel_gpu_ready(void) { return g_ready; }
UINT32 intel_gpu_generation(void) { return g_gen; }
UINT16 intel_gpu_device_id(void) { return g_device; }
const char *intel_gpu_name(void) { return g_name; }
UINT64 intel_gpu_mmio(void) { return g_mmio_va; }
UINT64 intel_gpu_aperture(void) { return g_aperture_va; }
UINT64 intel_gpu_stolen_phys(void) { return g_stolen_phys; }
UINT64 intel_gpu_stolen_bytes(void) { return g_stolen_bytes; }
