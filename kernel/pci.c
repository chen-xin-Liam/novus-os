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
#include "acpi.h"

#if CONFIG_NOVUS_PCI

#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

/* B-20: memory-mapped configuration space (ACPI MCFG), used in preference to
 * mechanism #1 whenever the firmware publishes it.
 *
 * The reason is measured, not theoretical: on the EliteBook one 256-bus
 * mechanism-#1 walk costs 927 ms (22 functions found, so ~8000 reads at
 * ~110 us each - the legacy 0xCF8/0xCFC pair is trapped by the platform
 * firmware). devtab scans once and every driver's pci_find() walked the whole
 * bus tree again: net alone re-walked it five times. ECAM turns each of those
 * reads into an uncached load from the reserved window. */
static UINT64 g_ecam;        /* base of the window's first bus */
static UINT8  g_ecam_first;
static UINT8  g_ecam_last;
static UINT64 g_ecam_cost;   /* measured cycles per config access, ECAM */
static UINT64 g_io_cost;     /* measured cycles per config access, #1 */

static void pci_ecam_bench(void);

static UINT64 pci_rdtsc(void) {
  UINT32 lo;
  UINT32 hi;

  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | (UINT64)lo;
}

/* Which mechanism was adopted, and what each one measured. Zero means the
 * machine never published MCFG. */
void pci_config_cost(UINT64 *ecam_cycles, UINT64 *io_cycles) {
  if (ecam_cycles != 0) {
    *ecam_cycles = g_ecam_cost;
  }
  if (io_cycles != 0) {
    *io_cycles = g_io_cost;
  }
}

int pci_ecam_info(UINT64 *base, UINT8 *first, UINT8 *last) {
  if (g_ecam == 0u) {
    return 0;
  }
  if (base != 0) {
    *base = g_ecam;
  }
  if (first != 0) {
    *first = g_ecam_first;
  }
  if (last != 0) {
    *last = g_ecam_last;
  }
  return 1;
}

int pci_ecam_init(UINT64 base, UINT8 first, UINT8 last) {
  if (base == 0u) {
    /* an explicit disable: back to mechanism #1 */
    g_ecam = 0u;
    g_ecam_first = 0u;
    g_ecam_last = 0u;
    return 0;
  }
  if (last < first) {
    return -1;
  }
  /* The window has to be reachable through the current mapping: ECAM is a
   * reserved MMIO range, described by the firmware memory map, so it is
   * inside the identity map on every machine that publishes MCFG. */
  g_ecam = base;
  g_ecam_first = first;
  g_ecam_last = last;
  return 0;
}

/* Read the first configuration entry out of an MCFG table and adopt it. */
int pci_ecam_from_mcfg(UINT64 mmap, UINT64 size, UINT64 stride) {
  const acpi_table_t *t = acpi_find_table(mmap, size, stride, "MCFG");
  const UINT8        *e;
  UINT32              lo;
  UINT32              hi;
  UINT64              base;
  UINT8               first;
  UINT8               last;

  if (t == 0 || t->length < 44u + 16u) {
    return -1;
  }
  /* The table is packed and may not be 8-byte aligned: read the address as
   * two dwords rather than dereferencing a UINT64 pointer. */
  e = (const UINT8 *)t + 44u;
  lo = (UINT32)e[0] | ((UINT32)e[1] << 8) | ((UINT32)e[2] << 16) |
       ((UINT32)e[3] << 24);
  hi = (UINT32)e[4] | ((UINT32)e[5] << 8) | ((UINT32)e[6] << 16) |
       ((UINT32)e[7] << 24);
  base = ((UINT64)hi << 32) | (UINT64)lo;
  first = e[10];
  last = e[11];
  if (pci_ecam_init(base, first, last) != 0) {
    return -1;
  }
  /* Keep it only if it is actually cheaper on this machine. */
  pci_ecam_bench();
  return (g_ecam != 0u) ? 0 : -1;
}

static volatile UINT8 *ecam_at(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
  UINT64 base = g_ecam + (((UINT64)(UINT8)(bus - g_ecam_first)) << 20);

  base |= ((UINT64)(dev & 0x1Fu)) << 15;
  base |= ((UINT64)(func & 0x07u)) << 12;
  base |= (UINT64)(off & 0xFCu);
  return (volatile UINT8 *)(UINTN)base;
}

static int ecam_covers(UINT8 bus) {
  return (g_ecam != 0u && bus >= g_ecam_first && bus <= g_ecam_last) ? 1 : 0;
}

static UINT32 addr_reg(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
  return (UINT32)(0x80000000u | ((UINT32)bus << 16) | ((UINT32)dev << 11) |
                  ((UINT32)func << 8) | ((UINT32)off & 0xFCu));
}

/* Which mechanism is cheaper here is a property of the machine, not of the
 * spec: a laptop's firmware traps 0xCF8/0xCFC (~110 us per pair) so ECAM wins
 * by two orders of magnitude, while an emulator's MMIO exit can cost more than
 * its port fast path. Measure 32 accesses of each and keep the winner. */
static void pci_ecam_bench(void) {
  UINT64 t0;
  UINT64 t1;
  UINT64 t2;
  UINT32 sink = 0u;
  int    i;

  if (g_ecam == 0u) {
    return;
  }
  /* Correctness gate before speed: the window must describe the same machine
   * the legacy ports do. A firmware that publishes an MCFG entry the kernel
   * cannot interpret (wrong base, a segment that is not bus 0) would otherwise
   * turn every device into "absent" - the entire boot loses its disk, its
   * audio, its xHCI. One read of bus-first/dev0/fn0 through both paths says
   * whether they agree. */
  {
    volatile UINT32 *p = (volatile UINT32 *)(UINTN)ecam_at(g_ecam_first, 0u, 0u, 0u);
    UINT32           via_ecam = *p;
    UINT32           via_io;

    io_out32(PCI_CONFIG_ADDR, addr_reg(g_ecam_first, 0u, 0u, 0u));
    via_io = io_in32(PCI_CONFIG_DATA);

    if (via_ecam != via_io) {
      g_ecam = 0u;
      g_ecam_first = 0u;
      g_ecam_last = 0u;
      return;
    }
  }
  t0 = pci_rdtsc();
  for (i = 0; i < 32; i++) {
    volatile UINT32 *p =
        (volatile UINT32 *)(UINTN)ecam_at(g_ecam_first, 0u, 0u, 0u);

    sink ^= *p;
  }
  t1 = pci_rdtsc();
  for (i = 0; i < 32; i++) {
    io_out32(PCI_CONFIG_ADDR, addr_reg(g_ecam_first, 0u, 0u, 0u));
    sink ^= io_in32(PCI_CONFIG_DATA);
  }
  t2 = pci_rdtsc();
  if (sink == 0xDEADBEEFu) {
    kputs(""); /* keep the compiler from dropping the reads */
  }
  g_ecam_cost = (t1 - t0) / 32u;
  g_io_cost = (t2 - t1) / 32u;
  if (g_ecam_cost >= g_io_cost) {
    g_ecam = 0u;
    g_ecam_first = 0u;
    g_ecam_last = 0u;
  }
}

UINT32 pci_read32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
  if (ecam_covers(bus) != 0) {
    volatile UINT8 *p = ecam_at(bus, dev, func, off);
    UINT32          v;

    __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"(p) : "memory");
    return v;
  }
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
  if (ecam_covers(bus) != 0) {
    volatile UINT8 *p = ecam_at(bus, dev, func, off);

    __asm__ volatile("movl %0, (%1)" : : "r"(val), "r"(p) : "memory");
    return;
  }
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
  /* Size = the lowest set bit of the mask. Inverting the mask and adding
   * one gives that bit; masking the inverted value first - which this did
   * before - throws it away and reports a size that is 1 too small for
   * anything that is not a power of two in the low nibble: a 16 MiB BAR
   * came back as 16 MiB - 15 bytes. */
  size = (UINT64)(~(mask_lo & 0xFFFFFFF0u)) + 1u;
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

static int pci_walk(pci_dev_t *out, int max) {
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

/* B-21: one walk, cached.
 *
 * Every caller used to walk the tree itself: devtab once, then net five times
 * (one per driver probe), hda, i2c, touchpad, the input layer. On a laptop
 * where a single config read costs ~110 us, each extra walk is another ~900 ms
 * of boot; even with ECAM adopted a walk is real work. The tree does not change
 * under us during boot, so the first walk is kept and everyone else looks it up.
 * pci_scan_invalidate() re-walks, for the post-boot hot-plug path. */
static pci_dev_t g_pool[PCI_SHARED_MAX];
static int       g_pool_n;
static int       g_pool_valid;

static int pci_walk(pci_dev_t *out, int max);

void pci_scan_invalidate(void) {
  g_pool_valid = 0;
  g_pool_n = 0;
}

/* Shared, non-reentrant scan table. Lives in .bss because a 160-entry
 * pci_dev_t array is about 20 KiB - far too much for the boot stack - and
 * because the whole point is to see functions a 32-entry scan cuts off. */
pci_dev_t *pci_scan_shared(int *n) {
  if (g_pool_valid == 0) {
    g_pool_n = pci_walk(g_pool, PCI_SHARED_MAX);
    g_pool_valid = 1;
  }
  if (n != 0) {
    *n = g_pool_n;
  }
  return g_pool;
}

int pci_scan(pci_dev_t *out, int max) {
  const pci_dev_t *src;
  int              n;

  if (out == 0 || max <= 0) {
    return -1;
  }
  src = pci_scan_shared(&n);
  if (n > max) {
    n = max;
  }
  for (int i = 0; i < n; i++) {
    out[i] = src[i];
  }
  return n;
}

int pci_find(UINT16 vendor, UINT16 device, pci_dev_t *out) {
  const pci_dev_t *list = pci_scan_shared(&g_pool_n);
  int              i;

  for (i = 0; i < g_pool_n; i++) {
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
  const pci_dev_t *list = pci_scan_shared(&g_pool_n);
  int              i;
  int              seen = 0;

  for (i = 0; i < g_pool_n; i++) {
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

/* Compact text assembly for the log lines that carry a whole table: append
 * stops at the buffer end so a truncated line is still a valid string, and
 * pc_hex drops the leading zeros kputs_hex insists on. */
static void pc_append(char *dst, UINT32 cap, UINT32 *n, const char *s) {
  while (*s != '\0' && (*n + 1u) < cap) {
    dst[*n] = *s;
    (*n)++;
    s++;
  }
  dst[*n] = '\0';
}

static void pc_hex(char *dst, UINT32 cap, UINT32 *n, UINT64 v) {
  char b[17];
  int  k = 0;
  int  i;

  if (v == 0u) {
    pc_append(dst, cap, n, "0");
    return;
  }
  while (v != 0u && k < 16) {
    UINT32 d = (UINT32)(v & 0xFu);

    b[k] = (char)((d < 10u) ? ('0' + (int)d) : ('a' + (int)d - 10));
    k++;
    v >>= 4;
  }
  for (i = k - 1; i >= 0; i--) {
    char one[2];

    one[0] = b[i];
    one[1] = '\0';
    pc_append(dst, cap, n, one);
  }
}

/* Two fixed hex digits, so class:subclass reads as 0600 rather than "60". */
static void pc_hex2(char *dst, UINT32 cap, UINT32 *n, UINT64 v) {
  char b[3];
  int  i;

  for (i = 1; i >= 0; i--) {
    UINT32 d = (UINT32)((v >> (4 * i)) & 0xFu);

    b[1 - i] = (char)((d < 10u) ? ('0' + (int)d) : ('a' + (int)d - 10));
  }
  b[2] = '\0';
  pc_append(dst, cap, n, b);
}

void pci_log(void) {
  static char      line[896];
  const pci_dev_t *list;
  UINT32           n = 0u;
  int              count;
  int              i;

  list = pci_scan_shared(&count);

  /* One line, unpadded numbers. The earlier form printed a zero-padded 16-digit
   * hex for every field of every device - about 230 characters per device,
   * 4.3 KB for this machine's 22. At the laptop's measured ~1.2 ms per printed
   * character (serial at 115200 plus the splash panel repaint) that line alone
   * was the whole 7.3 s "network probes" window; the numbers were never the
   * cost, the padding was. The same facts stay readable in a fifth of the
   * space, and the full table is still one panel away in the device manager. */
  pc_append(line, sizeof(line), &n, "[PCI] ");
  pc_hex(line, sizeof(line), &n, (UINT64)(UINT32)count);
  pc_append(line, sizeof(line), &n, " devices:");
  for (i = 0; i < count; i++) {
    if (n + 40u >= sizeof(line)) {
      pc_append(line, sizeof(line), &n, " ...");
      break;
    }
    pc_append(line, sizeof(line), &n, " ");
    pc_hex(line, sizeof(line), &n, (UINT64)list[i].bus);
    pc_append(line, sizeof(line), &n, ":");
    pc_hex(line, sizeof(line), &n, (UINT64)list[i].dev);
    pc_append(line, sizeof(line), &n, ".");
    pc_hex(line, sizeof(line), &n, (UINT64)list[i].func);
    pc_append(line, sizeof(line), &n, "=");
    pc_hex(line, sizeof(line), &n, (UINT64)list[i].vendor);
    pc_append(line, sizeof(line), &n, ":");
    pc_hex(line, sizeof(line), &n, (UINT64)list[i].device);
    pc_append(line, sizeof(line), &n, "/");
    pc_hex2(line, sizeof(line), &n, (UINT64)list[i].class_code);
    pc_hex2(line, sizeof(line), &n, (UINT64)list[i].subclass);
    if (list[i].bar[0] != 0u) {
      pc_append(line, sizeof(line), &n, "@");
      pc_hex(line, sizeof(line), &n, list[i].bar[0]);
    }
  }
  pc_append(line, sizeof(line), &n, "\n");
  kputs(line);
}

#else /* !CONFIG_NOVUS_PCI */

int pci_scan(pci_dev_t *out, int max) { (void)out; (void)max; return -1; }
UINT32 pci_read32(UINT8 b, UINT8 d, UINT8 f, UINT8 o) { (void)b;(void)d;(void)f;(void)o; return 0xFFFFFFFFu; }
UINT16 pci_read16(UINT8 b, UINT8 d, UINT8 f, UINT8 o) { (void)b;(void)d;(void)f;(void)o; return 0xFFFFu; }
UINT8  pci_read8(UINT8 b, UINT8 d, UINT8 f, UINT8 o) { (void)b;(void)d;(void)f;(void)o; return 0xFFu; }
void   pci_write32(UINT8 b, UINT8 d, UINT8 f, UINT8 o, UINT32 v) { (void)b;(void)d;(void)f;(void)o;(void)v; }
int pci_find(UINT16 v, UINT16 d, pci_dev_t *o) { (void)v;(void)d;(void)o; return 0; }
pci_dev_t *pci_scan_shared(int *n) { if (n != 0) { *n = 0; } return 0; }
int pci_find_class(UINT8 c, UINT8 s, int i, pci_dev_t *o) { (void)c;(void)s;(void)i;(void)o; return 0; }
void pci_enable_device(const pci_dev_t *d) { (void)d; }
void pci_log(void) {}

#endif
