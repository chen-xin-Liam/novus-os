/**
 * Novus OS - PCI configuration space and BAR sizing (M10).
 *
 * Config access is mechanism #1: write {enable, bus, dev, func, offset} to
 * 0xCF8 and read/write 0xCFC. An empty slot reads back as 0xFFFFFFFF,
 * which is also how a BAR is sized: write all ones, read the mask back,
 * restore the original value. The low four bits of the mask are the BAR's
 * type flags and are not part of the size.
 */

#include "kernel.h"
#include "cpu.h"
#include "pci.h"

#if CONFIG_NOVUS_PCI

#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

static UINT32 addr_reg(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
  return (UINT32)(0x80000000u | ((UINT32)bus << 16) | ((UINT32)dev << 11) |
                  ((UINT32)func << 8) | ((UINT32)off & 0xFCu));
}

UINT32 pci_read32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
  io_out32(PCI_CONFIG_ADDR, addr_reg(bus, dev, func, off));
  return io_in32(PCI_CONFIG_DATA);
}

UINT16 pci_read16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
  UINT32 v = pci_read32(bus, dev, func, off & 0xFCu);

  return (UINT16)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

UINT8 pci_read8(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
  UINT32 v = pci_read32(bus, dev, func, off & 0xFCu);

  return (UINT8)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

void pci_write32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off, UINT32 val) {
  io_out32(PCI_CONFIG_ADDR, addr_reg(bus, dev, func, off));
  io_out32(PCI_CONFIG_DATA, val);
}

/* Size and base of one memory BAR.
 *
 * A 64-bit BAR is a pair of dwords: the low half carries the type bits
 * (bit 2 set) and the high half holds the upper address. Both halves are
 * sized together, so the driver learns the real window length, and the
 * full 64-bit base is reported even when it lies above 4 GiB - the
 * kernel's identity map stops at 4 GiB, so such a BAR has to be moved
 * before it can be used. */
static int bar_info(UINT8 bus, UINT8 dev, UINT8 func, UINT8 index,
                    UINT64 *base_out, UINT64 *size_out) {
  UINT8  off = (UINT8)(0x10u + index * 4u);
  UINT32 lo = pci_read32(bus, dev, func, off);
  UINT32 hi = 0;
  UINT32 mask_lo;
  UINT32 mask_hi = 0;
  UINT64 size;
  int    is64;

  if ((lo & 0x1u) != 0u) {
    return -1; /* I/O space */
  }
  /* The high dword of a 64-bit BAR is not a BAR of its own. */
  if (index > 0u) {
    UINT32 prev = pci_read32(bus, dev, func, (UINT8)(off - 4u));

    if ((prev & 0x1u) == 0u && (prev & 0x6u) == 0x4u) {
      return -2;
    }
  }
  is64 = ((lo & 0x6u) == 0x4u);
  if (is64) {
    hi = pci_read32(bus, dev, func, (UINT8)(off + 4u));
  }
  pci_write32(bus, dev, func, off, 0xFFFFFFFFu);
  mask_lo = pci_read32(bus, dev, func, off);
  if (is64) {
    pci_write32(bus, dev, func, (UINT8)(off + 4u), 0xFFFFFFFFu);
    mask_hi = pci_read32(bus, dev, func, (UINT8)(off + 4u));
    pci_write32(bus, dev, func, (UINT8)(off + 4u), hi);
  }
  pci_write32(bus, dev, func, off, lo);

  if (mask_lo == 0u || mask_lo == 0xFFFFFFFFu) {
    return -1;
  }
  size = ((UINT64)(~(mask_lo & 0xFFFFFFF0u)) & 0xFFFFFFF0u) + 1u;
  if (is64 && mask_hi != 0u && mask_hi != 0xFFFFFFFFu) {
    size += ((UINT64)(~mask_hi) + 1u) << 32;
  }
  *base_out = ((UINT64)(lo & 0xFFFFFFF0u)) | ((UINT64)hi << 32);
  *size_out = size;
  return 0;
}

void pci_enable_device(const pci_dev_t *d) {
  UINT32 cmd = pci_read32(d->bus, d->dev, d->func, 0x04);

  cmd |= 0x6u; /* memory space + bus master */
  pci_write32(d->bus, d->dev, d->func, 0x04, cmd);
}

int pci_bar_assign(pci_dev_t *d, UINT8 index, UINT64 addr) {
  UINT8  off;
  UINT32 cmd;
  UINT32 lo;
  int    is64;

  if (d == 0 || index > 5u || d->bar_size[index] == 0u) {
    return -1;
  }
  off = (UINT8)(0x10u + index * 4u);
  lo = pci_read32(d->bus, d->dev, d->func, off);
  is64 = ((lo & 0x1u) == 0u && (lo & 0x6u) == 0x4u);
  cmd = pci_read32(d->bus, d->dev, d->func, 0x04);
  /* The window moves, so decoding has to be off while it is written. */
  pci_write32(d->bus, d->dev, d->func, 0x04, cmd & ~2u);
  pci_write32(d->bus, d->dev, d->func, off, (UINT32)(addr & 0xFFFFFFF0u));
  if (is64) {
    pci_write32(d->bus, d->dev, d->func, (UINT8)(off + 4u),
                (UINT32)(addr >> 32));
  }
  pci_write32(d->bus, d->dev, d->func, 0x04, cmd | 2u);

  lo = pci_read32(d->bus, d->dev, d->func, off);
  if ((lo & 0xFFFFFFF0u) != (UINT32)(addr & 0xFFFFFFF0u)) {
    return -1;
  }
  d->bar64[index] = addr;
  d->bar[index] = (addr < 0x100000000ULL) ? addr : 0u;
  return 0;
}

int pci_scan(pci_dev_t *out, int max) {
  int    n = 0;
  UINT32 bus;
  UINT8  dev;
  UINT8  fn;

  if (out == 0 || max <= 0) {
    return -1;
  }
  for (bus = 0; bus < 256u && n < max; bus++) {
    for (dev = 0; dev < 32u && n < max; dev++) {
      UINT8 hdr0 = pci_read8((UINT8)bus, dev, 0, 0x0E);
      int   multifunction = ((hdr0 & 0x80u) != 0u);

      for (fn = 0; fn < (multifunction ? 8u : 1u) && n < max; fn++) {
        UINT32     id = pci_read32((UINT8)bus, dev, fn, 0x00);
        pci_dev_t *d;
        UINT8      i;

        if (id == 0xFFFFFFFFu || id == 0u) {
          continue;
        }
        d = &out[n++];
        memset(d, 0, sizeof(*d));
        d->bus = (UINT8)bus;
        d->dev = dev;
        d->func = fn;
        d->vendor = (UINT16)(id & 0xFFFFu);
        d->device = (UINT16)(id >> 16);
        d->class_code = pci_read8(d->bus, dev, fn, 0x0B);
        d->subclass = pci_read8(d->bus, dev, fn, 0x0A);
        d->prog_if = pci_read8(d->bus, dev, fn, 0x09);
        d->irq = pci_read8(d->bus, dev, fn, 0x3C);
        for (i = 0; i < 6u; i++) {
          UINT64 base = 0;
          UINT64 size = 0;

          if (bar_info(d->bus, dev, fn, i, &base, &size) == 0) {
            d->bar64[i] = base;
            d->bar_size[i] = size;
            /* Only a base inside the identity map can be dereferenced. */
            d->bar[i] = (base < 0x100000000ULL) ? base : 0u;
          }
        }
      }
    }
  }
  return n;
}

int pci_find(UINT16 vendor, UINT16 device, pci_dev_t *out) {
  pci_dev_t list[PCI_MAX_DEVICES];
  int       n = pci_scan(list, PCI_MAX_DEVICES);
  int       i;

  for (i = 0; i < n; i++) {
    if (list[i].vendor == vendor && list[i].device == device) {
      if (out != 0) {
        *out = list[i];
      }
      return 1;
    }
  }
  return 0;
}

int pci_find_class(UINT8 cls, UINT8 sub, int index, pci_dev_t *out) {
  pci_dev_t list[PCI_MAX_DEVICES];
  int       n = pci_scan(list, PCI_MAX_DEVICES);
  int       i;
  int       seen = 0;

  for (i = 0; i < n; i++) {
    if (list[i].class_code != cls) {
      continue;
    }
    if (sub != 0xFFu && list[i].subclass != sub) {
      continue;
    }
    if (seen++ != index) {
      continue;
    }
    if (out != 0) {
      *out = list[i];
    }
    return 1;
  }
  return 0;
}

void pci_log(void) {
  pci_dev_t list[PCI_MAX_DEVICES];
  int       n = pci_scan(list, PCI_MAX_DEVICES);
  int       i;

  kputs("[PCI] ");
  kputs_hex((UINT64)(UINT32)n);
  kputs(" devices\n");
  for (i = 0; i < n; i++) {
    kputs("[PCI]   ");
    kputs_hex((UINT64)list[i].bus);
    kputs(":");
    kputs_hex((UINT64)list[i].dev);
    kputs(".");
    kputs_hex((UINT64)list[i].func);
    kputs(" id ");
    kputs_hex((UINT64)list[i].vendor);
    kputs(":");
    kputs_hex((UINT64)list[i].device);
    kputs(" class ");
    kputs_hex((UINT64)list[i].class_code);
    kputs(":");
    kputs_hex((UINT64)list[i].subclass);
    kputs(" bar0 ");
    kputs_hex(list[i].bar[0]);
    kputs("\n");
  }
}

#else /* !CONFIG_NOVUS_PCI */

int pci_scan(pci_dev_t *out, int max) { (void)out; (void)max; return -1; }
UINT32 pci_read32(UINT8 b, UINT8 d, UINT8 f, UINT8 o) { (void)b;(void)d;(void)f;(void)o; return 0xFFFFFFFFu; }
UINT16 pci_read16(UINT8 b, UINT8 d, UINT8 f, UINT8 o) { (void)b;(void)d;(void)f;(void)o; return 0xFFFFu; }
UINT8  pci_read8(UINT8 b, UINT8 d, UINT8 f, UINT8 o) { (void)b;(void)d;(void)f;(void)o; return 0xFFu; }
void   pci_write32(UINT8 b, UINT8 d, UINT8 f, UINT8 o, UINT32 v) { (void)b;(void)d;(void)f;(void)o;(void)v; }
int pci_find(UINT16 v, UINT16 d, pci_dev_t *o) { (void)v;(void)d;(void)o; return 0; }
int pci_find_class(UINT8 c, UINT8 s, int i, pci_dev_t *o) { (void)c;(void)s;(void)i;(void)o; return 0; }
void pci_enable_device(const pci_dev_t *d) { (void)d; }
void pci_log(void) {}

#endif
