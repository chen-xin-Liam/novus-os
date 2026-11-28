/**
 * Novus OS - HID over I2C touchpad driver.
 *
 * The touchpad on this class of laptop is not on the PS/2 aux port: it is a
 * HID device on one of the PCH's LPSS I2C controllers (DesignWare based),
 * described by ACPI as PNP0C50 with an I2C SerialBus resource naming the
 * 7-bit slave address. The firmware has already configured that controller
 * (it used the touchpad until the moment we took over), so the driver only
 * has to: pick the controller, talk the I2C-HID protocol to the address the
 * firmware named, read its report descriptor, and poll the input register.
 *
 * Two rules shape the code:
 *
 *   1. Every wait is bounded. This runs on the machine that has already
 *      hung twice inside a hardware path, and an unbounded spin on a wedged
 *      I2C controller is a boot that never finishes. A timeout here means
 *      "no device", not "wait longer".
 *   2. Nothing is poked outside the controller's own DesignWare register
 *      block. The LPSS private register block differs per SoC and a wrong
 *      write there takes the whole platform with it; the firmware already
 *      brought the block up, and if it did not, the transfers fail and the
 *      driver stands down.
 */

#include "uefi.h"
#include <stddef.h>
#include "kernel.h"
#include "pci.h"
#include "mm.h"
#include "touchpad.h"
#include "i2c_hid.h"
#include "nd.h"
#include "nd_input.h"

/* ---- DesignWare I2C master registers (offsets from BAR0) -------------- */
#define DW_IC_CON            0x00u
#define DW_IC_TAR            0x04u
#define DW_IC_DATA_CMD       0x10u
#define DW_IC_SS_SCL_HCNT    0x14u
#define DW_IC_SS_SCL_LCNT    0x18u
#define DW_IC_FS_SCL_HCNT    0x1Cu
#define DW_IC_FS_SCL_LCNT    0x20u
#define DW_IC_INTR_MASK      0x30u
#define DW_IC_RAW_INTR_STAT  0x34u
#define DW_IC_CLR_INTR       0x40u
#define DW_IC_CLR_TX_ABRT    0x54u
#define DW_IC_ENABLE         0x6Cu
#define DW_IC_STATUS         0x70u
#define DW_IC_TXFLR          0x74u
#define DW_IC_ENABLE_STATUS  0x9Cu
#define DW_IC_COMP_PARAM_1   0xF4u

#define IC_CON_MASTER        0x0001u
#define IC_CON_SPEED_STD     0x0002u   /* bits 1:2 = 01 */
#define IC_CON_SPEED_FAST    0x0004u   /* bits 1:2 = 10 */
#define IC_CON_RESTART_EN    0x0020u
#define IC_CON_SLAVE_DISABLE 0x0040u

#define IC_CMD_READ          0x0100u
#define IC_CMD_STOP          0x0200u
#define IC_CMD_RESTART       0x0400u

#define IC_STATUS_ACT        0x0001u
#define IC_STATUS_TFNF       0x0002u
#define IC_STATUS_TFE        0x0004u
#define IC_STATUS_RFNE       0x0008u
#define IC_STATUS_MST_ACT    0x0020u

/* TX_ABRT is bit 4 on every DW revision this code has met; it is only used
 * to report a failed transfer, never to decide that one succeeded, so a
 * wrong bit costs a log line and not a wrong read. */
#define IC_RAW_TX_ABRT       0x0010u

/* One wait, worst case, is a few microseconds. This is the "give up" number:
 * large enough for a slow 100 kHz transfer, small enough that a wedged
 * controller costs milliseconds and not seconds. */
#define I2C_SPINS            400000u

/* ---- I2C-HID protocol ------------------------------------------------- */

#define HID_DESC_REG         0x0001u   /* the usual _DSM value, see probe */
#define HID_INPUT_REG        0x0003u
#define HID_OUTPUT_REG       0x0004u
#define HID_COMMAND_REG      0x0005u
#define HID_DATA_REG         0x0006u
#define HID_REPORT_DESC_REG  0x0020u

#define IHID_MAX_FIELDS      12u
#define IHID_MAX_REPORT      16u

typedef struct {
  UINT8  id;          /* report id this field lives in, 0 for the plain one */
  UINT8  kind;        /* 1 = X, 2 = Y, 3 = tip switch, 4 = button 1..3 */
  UINT8  page;        /* usage page of the element */
  UINT16 usage;       /* usage of the element */
  UINT32 bit_off;     /* bit offset inside the report payload */
  UINT32 size;        /* bits per element (8, 16, ...) */
  UINT32 count;       /* elements in this field */
  INT32  lmin;
  INT32  lmax;
} ihid_field_t;

static struct {
  int      state;          /* -1 unprobed, 0 failed, 1 polling */
  UINT64   base;           /* device-window VA of the winning controller */
  UINT8    addr;           /* 7-bit slave address */
  UINT16   desc_len;
  UINT16   rep_desc_len;
  UINT16   rep_desc_reg;
  UINT16   input_reg;
  UINT16   max_input;
  UINT16   cmd_reg;
  UINT16   data_reg;
  UINT32   vendor;
  UINT32   product;
  ihid_field_t f[IHID_MAX_FIELDS];
  UINT32   nf;
  int      have_x;
  int      have_y;
  int      have_tip;
  UINT8    report_size;    /* bytes the largest report of interest occupies */
  UINT32   fails;
  UINT32   buses;          /* serial-bus controllers the PCI scan found */
  INT32    last_x;         /* last absolute position reported, screen space */
  INT32    last_y;
  UINT8    last_buttons;
  UINT8    buf[64];
  UINT8    rd[256];
} g_ih;

static void ih_dec(UINT32 v);
static UINT16 ih_u16(const UINT8 *p) {
  return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

/* The controller is touched through a device-window mapping that has to be
 * uncacheable. The identity map this kernel builds is write-back, so a write
 * to IC_ENABLE stays in a cache line until something evicts it, and the read
 * of IC_ENABLE_STATUS returns the stale zero the driver's own write left
 * there - a controller that "refuses to enable" on real hardware while QEMU,
 * whose memory has no cache, accepts every write immediately and sets every
 * status bit at once. That is why the laptop reports `i2c=0 bus=3`: three
 * serial-bus functions found, none usable. mm_map_phys(..., MM_MAP_UC) is the
 * same mechanism usb.c uses for its register window. */
static UINT64 dw_rd(UINT64 base, UINT32 off) {
  return *(volatile UINT32 *)(UINTN)(base + off);
}

static void dw_wr(UINT64 base, UINT32 off, UINT32 v) {
  *(volatile UINT32 *)(UINTN)(base + off) = v;
}

/* Intel LPSS keeps a block of private registers 0x200 above the DesignWare
 * core: clock gating at +0x00 and a reset word at +0x04 with the function's
 * reset line in bit 1. A controller that the firmware powered down answers
 * every read with 0xFF and refuses to set IC_ENABLE, which is what made the
 * first probe on this laptop report "not a live master" instead of a device.
 * Clearing the reset bit and opening the gate is what the platform driver
 * does before it ever touches the core. */
#define LPSS_PRIV_BASE 0x200u
#define LPSS_PRIV_CLKGATE 0x00u
#define LPSS_PRIV_RESETS 0x04u
#define LPSS_RESET_FUNC (1u << 1)

static void lpss_wake(UINT64 base) {
  UINT32 gate = dw_rd(base, LPSS_PRIV_BASE + LPSS_PRIV_CLKGATE);
  UINT32 rst = dw_rd(base, LPSS_PRIV_BASE + LPSS_PRIV_RESETS);

  if (gate == 0xFFFFFFFFu || rst == 0xFFFFFFFFu) {
    return;
  }
  dw_wr(base, LPSS_PRIV_BASE + LPSS_PRIV_CLKGATE, 0u);
  dw_wr(base, LPSS_PRIV_BASE + LPSS_PRIV_RESETS, rst & ~LPSS_RESET_FUNC);
}

/* Wait for (dw_rd(status) & mask) == want; 0 on success. */
static int dw_wait(UINT64 base, UINT32 mask, UINT32 want) {
  UINT32 spin = I2C_SPINS;

  while (spin-- != 0u) {
    if ((dw_rd(base, DW_IC_STATUS) & mask) == want) {
      return 0;
    }
  }
  return -1;
}

/* Bring the master out of reset and leave it enabled, standard mode. The SCL
 * counts are read back first: if the firmware already programmed a plausible
 * pair, it is kept, because recomputing them needs the block's input clock,
 * which the controller does not report. */
static int dw_init(UINT64 base) {
  UINT32 comp;
  UINT32 con;
  UINT32 spin;

  lpss_wake(base);
  comp = dw_rd(base, DW_IC_COMP_PARAM_1);
  if (comp == 0u || comp == 0xFFFFFFFFu) {
    return -1;
  }
  con = dw_rd(base, DW_IC_CON);
  if (con == 0xFFFFFFFFu) {
    return -1;
  }

  dw_wr(base, DW_IC_ENABLE, 0u);
  spin = I2C_SPINS;
  while (spin-- != 0u) {
    if ((dw_rd(base, DW_IC_ENABLE_STATUS) & 1u) == 0u) {
      break;
    }
  }

  con |= IC_CON_MASTER | IC_CON_RESTART_EN | IC_CON_SLAVE_DISABLE;
  if ((con & 0x0006u) == 0u) {
    con |= IC_CON_SPEED_STD;
  }
  dw_wr(base, DW_IC_CON, con);
  dw_wr(base, DW_IC_INTR_MASK, 0u); /* polling, not interrupts */

  dw_wr(base, DW_IC_ENABLE, 1u);
  spin = I2C_SPINS;
  while (spin-- != 0u) {
    if ((dw_rd(base, DW_IC_ENABLE_STATUS) & 1u) != 0u) {
      return 0;
    }
  }
  return -1;
}

/* One combined transfer: write `wlen` bytes, then read `rlen` bytes with a
 * repeated start. Byte-at-a-time through the FIFOs, so it works regardless of
 * the core's FIFO depth. */
static int dw_xfer(UINT64 base, UINT8 addr, const UINT8 *wr, UINT32 wlen,
                   UINT8 *rd, UINT32 rlen) {
  UINT32 i;
  UINT32 spin;

  if (wlen + rlen == 0u) {
    return -1;
  }
  if ((dw_rd(base, DW_IC_ENABLE) & 1u) == 0u) {
    dw_wr(base, DW_IC_ENABLE, 1u);
  }
  dw_wr(base, DW_IC_TAR, (UINT32)addr & 0x7Fu);
  (void)dw_rd(base, DW_IC_CLR_INTR);
  (void)dw_rd(base, DW_IC_CLR_TX_ABRT);

  for (i = 0u; i < wlen; i++) {
    UINT32 cmd;

    if (dw_wait(base, IC_STATUS_TFNF, IC_STATUS_TFNF) != 0) {
      return -1;
    }
    cmd = (UINT32)wr[i];
    if ((i + 1u == wlen) && rlen == 0u) {
      cmd |= IC_CMD_STOP;
    }
    dw_wr(base, DW_IC_DATA_CMD, cmd);
  }

  for (i = 0u; i < rlen; i++) {
    UINT32 cmd = IC_CMD_READ;

    if (dw_wait(base, IC_STATUS_TFNF, IC_STATUS_TFNF) != 0) {
      return -1;
    }
    if (i == 0u) {
      cmd |= IC_CMD_RESTART;
    }
    if (i + 1u == rlen) {
      cmd |= IC_CMD_STOP;
    }
    dw_wr(base, DW_IC_DATA_CMD, cmd);
    if (dw_wait(base, IC_STATUS_RFNE, IC_STATUS_RFNE) != 0) {
      return -1;
    }
    rd[i] = (UINT8)(dw_rd(base, DW_IC_DATA_CMD) & 0xFFu);
  }

  /* Let the stop condition finish before the next transfer starts. */
  spin = I2C_SPINS;
  while (spin-- != 0u) {
    if ((dw_rd(base, DW_IC_STATUS) & IC_STATUS_ACT) == 0u) {
      break;
    }
  }
  if ((dw_rd(base, DW_IC_RAW_INTR_STAT) & IC_RAW_TX_ABRT) != 0u) {
    (void)dw_rd(base, DW_IC_CLR_TX_ABRT);
    return -1;
  }
  return 0;
}

/* Read `len` bytes of a 16-bit-addressed I2C-HID register. */
static int ih_read_reg(UINT16 reg, UINT8 *out, UINT32 len) {
  UINT8 cmd[2];

  cmd[0] = (UINT8)(reg & 0xFFu);
  cmd[1] = (UINT8)((reg >> 8) & 0xFFu);
  return dw_xfer(g_ih.base, g_ih.addr, cmd, 2u, out, len);
}

/* Pull one element out of a report. HID packs fields little-endian from the
 * start of the report payload (after the report id byte). */
static UINT32 ih_value(const UINT8 *rep, UINT32 len, const ihid_field_t *f,
                       UINT32 elem) {
  UINT32 bit = f->bit_off + elem * f->size;
  UINT32 v = 0u;
  UINT32 i;

  for (i = 0u; i < f->size; i++) {
    UINT32 b = bit + i;
    UINT32 byte = b >> 3;
    UINT32 bit_in_byte = b & 7u;

    if (byte >= len) {
      return v;
    }
    if (((rep[byte] >> bit_in_byte) & 1u) != 0u) {
      v |= (1u << i);
    }
  }
  /* Sign-extend when the logical range starts below zero. */
  if (f->lmin < 0 && f->size < 32u && f->size > 0u) {
    UINT32 sign = 1u << (f->size - 1u);

    if ((v & sign) != 0u) {
      v |= ~((sign << 1) - 1u);
    }
  }
  return v;
}

/* ---- report descriptor ------------------------------------------------- */

typedef struct {
  UINT32 usage_page;
  UINT32 lmin;
  UINT32 lmax;
  INT32  slmin;
  INT32  slmax;
  UINT32 size;
  UINT32 count;
  UINT32 id;
  UINT32 usages[4];
  UINT32 nusages;
  UINT32 bit[8];      /* running bit offset per report id */
} ih_desc_t;

static void ih_add_field(ih_desc_t *d, UINT32 bit_off) {
  UINT32 i;

  if (g_ih.nf >= IHID_MAX_FIELDS) {
    return;
  }
  for (i = 0u; i < d->nusages && i < 4u; i++) {
    UINT32 page = d->usages[i] >> 16;
    UINT32 usage = d->usages[i] & 0xFFFFu;
    ihid_field_t *f = &g_ih.f[g_ih.nf];
    UINT8 kind = 0u;

    if (page == 0x01u && usage == 0x30u) {
      kind = 1u; /* Generic Desktop X */
    } else if (page == 0x01u && usage == 0x31u) {
      kind = 2u; /* Generic Desktop Y */
    } else if (page == 0x0Du && usage == 0x42u) {
      kind = 3u; /* Digitizer tip switch */
    } else if (page == 0x09u && usage >= 0x01u && usage <= 0x03u) {
      kind = (UINT8)(3u + usage); /* button 1..3 -> kinds 4..6 */
    }
    if (kind == 0u) {
      continue;
    }
    f->id = (UINT8)d->id;
    f->kind = kind;
    f->page = (UINT8)page;
    f->usage = (UINT16)usage;
    f->bit_off = bit_off + i * d->size;
    f->size = d->size;
    f->count = 1u;
    f->lmin = d->slmin;
    f->lmax = d->slmax;
    g_ih.nf++;
    if (kind == 1u) {
      g_ih.have_x = 1;
    } else if (kind == 2u) {
      g_ih.have_y = 1;
    } else if (kind == 3u) {
      g_ih.have_tip = 1;
    }
  }
}

/* Scan a HID report descriptor just far enough to place X, Y, the tip switch
 * and the three buttons in the report payload. Anything it does not know is
 * skipped by its declared size, which is what makes a full descriptor
 * survivable. */
static void ih_parse_desc(const UINT8 *p, UINT32 len) {
  ih_desc_t d;
  UINT32 i = 0u;

  memset(&d, 0, sizeof(d));
  g_ih.nf = 0u;
  g_ih.have_x = 0;
  g_ih.have_y = 0;
  g_ih.have_tip = 0;

  while (i < len) {
    UINT8  prefix = p[i];
    UINT32 size = (UINT32)(prefix & 0x03u);
    UINT32 type = (UINT32)((prefix >> 2) & 0x03u);
    UINT32 tag = (UINT32)(prefix >> 4);
    UINT32 data = 0u;
    UINT32 width = (size == 3u) ? 4u : size;
    UINT32 k;

    i++;
    if (i + width > len) {
      break;
    }
    for (k = 0u; k < width; k++) {
      data |= (UINT32)p[i + k] << (8u * k);
    }
    i += width;

    if (type == 1u) { /* global */
      switch (tag) {
        case 0x0u: d.usage_page = data; break;
        case 0x1u: d.slmin = (INT32)data; if (size == 1u && (data & 0x80u)) { d.slmin = (INT32)(data | 0xFFFFFF00u); } d.lmin = (UINT32)d.slmin; break;
        case 0x2u: d.slmax = (INT32)data; if (size == 1u && (data & 0x80u)) { d.slmax = (INT32)(data | 0xFFFFFF00u); } d.lmax = (UINT32)d.slmax; break;
        case 0x7u: d.size = data; break;
        case 0x8u: d.id = data; break;
        case 0x9u: d.count = data; break;
        default: break;
      }
    } else if (type == 2u) { /* local */
      if (tag == 0x0u) { /* usage */
        if (d.nusages < 4u) {
          /* A 4-byte usage carries the page in its high half; a short one
           * inherits the current usage page. */
          if (size == 2u) {
            d.usages[d.nusages++] = (d.usage_page << 16) | (data & 0xFFFFu);
          } else {
            d.usages[d.nusages++] = data;
          }
        }
      }
    } else if (type == 0u) { /* main */
      if (tag == 0x8u) { /* input */
        UINT32 bits = d.size * d.count;
        UINT32 base = d.bit[d.id & 7u];

        ih_add_field(&d, base);
        d.bit[d.id & 7u] = base + bits;
        d.nusages = 0u;
        if ((UINT32)((base + bits + 7u) / 8u) > (UINT32)g_ih.report_size) {
          g_ih.report_size = (UINT8)((base + bits + 7u) / 8u);
        }
      } else if (tag == 0x9u || tag == 0xBu) { /* output / feature */
        d.bit[d.id & 7u] += d.size * d.count;
        d.nusages = 0u;
      } else if (tag == 0xAu) { /* collection */
        d.nusages = 0u;
      } else if (tag == 0xCu) { /* end collection */
        d.nusages = 0u;
      }
    }
  }
  /* The report id byte itself is not part of the payload. */
  if (g_ih.report_size < 2u) {
    g_ih.report_size = 4u;
  }
  if (g_ih.report_size + 1u > g_ih.max_input) {
    g_ih.report_size = (UINT8)(g_ih.max_input > 1u ? g_ih.max_input - 1u : 4u);
  }
}

/* ---- device bring-up --------------------------------------------------- */

/* Read the 30-byte HID descriptor and keep the registers it names. */
static int ih_read_descriptor(void) {
  UINT8 hdr[4];

  memset(g_ih.rd, 0, sizeof(g_ih.rd));
  if (ih_read_reg(HID_DESC_REG, hdr, 4u) != 0) {
    return -1;
  }
  g_ih.desc_len = ih_u16(hdr);
  if (g_ih.desc_len < 0x0004u || g_ih.desc_len > 0x00FFu) {
    return -1;
  }
  if (ih_u16(hdr + 2u) != 0x0100u) {
    /* bcdVersion must be 1.00 for a device that speaks this protocol. */
    return -1;
  }
  memset(g_ih.buf, 0, sizeof(g_ih.buf));
  if (ih_read_reg(HID_DESC_REG, g_ih.buf,
                  (UINT32)(g_ih.desc_len > sizeof(g_ih.buf)
                               ? sizeof(g_ih.buf) : g_ih.desc_len)) != 0) {
    return -1;
  }
  g_ih.rep_desc_len = ih_u16(g_ih.buf + 4u);
  g_ih.rep_desc_reg = ih_u16(g_ih.buf + 6u);
  g_ih.input_reg = ih_u16(g_ih.buf + 8u);
  g_ih.max_input = ih_u16(g_ih.buf + 10u);
  g_ih.cmd_reg = ih_u16(g_ih.buf + 14u);
  g_ih.data_reg = ih_u16(g_ih.buf + 16u);
  g_ih.vendor = ih_u16(g_ih.buf + 18u);
  g_ih.product = ih_u16(g_ih.buf + 20u);
  if (g_ih.rep_desc_reg == 0u || g_ih.rep_desc_len == 0u ||
      g_ih.rep_desc_len > sizeof(g_ih.rd) || g_ih.input_reg == 0u ||
      g_ih.max_input == 0u || g_ih.max_input > 60u) {
    return -1;
  }
  return 0;
}

static int ih_read_report_desc(void) {
  if (ih_read_reg(g_ih.rep_desc_reg, g_ih.rd,
                  (UINT32)g_ih.rep_desc_len) != 0) {
    return -1;
  }
  ih_parse_desc(g_ih.rd, (UINT32)g_ih.rep_desc_len);
  return 0;
}

/* Ask the device to leave power state D3. Best effort: a device that ignores
 * the command still answers reads, one that does not gets a log line. */
static void ih_power_on(void) {
  UINT8 both[4];

  if (g_ih.cmd_reg == 0u || g_ih.data_reg == 0u) {
    return;
  }
  /* Command register first, then SET_POWER(D0) in the data register: one
   * combined write, the way the protocol defines the pair. */
  both[0] = (UINT8)(g_ih.cmd_reg & 0xFFu);
  both[1] = (UINT8)((g_ih.cmd_reg >> 8) & 0xFFu);
  both[2] = 0x00u;
  both[3] = 0x00u;
  (void)dw_xfer(g_ih.base, g_ih.addr, both, 4u, 0, 0);
}

static int ih_probe_one(UINT64 base, UINT8 addr) {
  g_ih.base = base;
  g_ih.addr = addr;
  memset(&g_ih.f, 0, sizeof(g_ih.f));
  g_ih.nf = 0u;
  g_ih.report_size = 0u;
  g_ih.have_x = 0;
  g_ih.have_y = 0;
  g_ih.have_tip = 0;

  if (ih_read_descriptor() != 0) {
    return -1;
  }
  if (ih_read_report_desc() != 0) {
    return -1;
  }
  /* A device with no X/Y field in its report descriptor is not a pointer. */
  if (g_ih.have_x == 0 && g_ih.have_y == 0) {
    return -1;
  }
  ih_power_on();
  return 0;
}

int i2c_hid_init(void) {
  pci_dev_t *list;
  static const UINT8 fallback[] = { 0x2Cu, 0x15u, 0x2Du, 0x20u, 0x10u, 0x3Cu };
  int n;
  int i;
  UINT32 a;
  UINT32 seen = 0u;

  g_ih.state = 0;
  g_ih.buses = 0u;
  g_ih.fails = 0u;
  g_ih.last_x = -1;
  g_ih.last_y = -1;
  g_ih.last_buttons = 0u;

  /* Shared scan table: a laptop has more than 32 PCI functions, and the
   * serial-bus controllers sit near the end of the walk. A 32-entry table
   * reported "bus=3" on the real machine while the rest were simply not
   * enumerated. */
  list = pci_scan_shared(&n);
  if (list == NULL) {
    n = 0;
  }
  for (i = 0; i < n; i++) {
    UINT64 base;
    UINT64 bar;
    UINT32 sz;
    int    c;

    if (list[i].class_code != 0x0Cu || list[i].subclass != 0x80u) {
      continue;
    }
    seen++;
    g_ih.buses = seen;
    /* One line per bus controller, so a photo of the live log on the real
     * machine says exactly what the firmware laid out. */
    kputs("[I2C] controller ");
    ih_dec((UINT32)i);
    kputs(" bar0=");
    kputs_hex(list[i].bar64[0]);
    kputs(" size=");
    kputs_hex(list[i].bar_size[0]);
    kputs("\n");
    /* 64-bit BARs are normal on recent Intel parts and a 32-bit BAR may also
     * sit above 4 GiB, where the identity map does not reach. mm_map_phys
     * takes a 64-bit physical address, so neither case has to be skipped any
     * more; what used to be refused here is now simply mapped. */
    bar = (list[i].bar64[0] != 0u) ? list[i].bar64[0] : (UINT64)list[i].bar[0];
    if (bar == 0u) {
      kputs("[I2C] no assigned BAR, skipped\n");
      continue;
    }
    sz = (UINT32)list[i].bar_size[0];
    if (sz != 0u && sz < 0x100u) {
      continue;
    }
    pci_enable_device(&list[i]);
    /* A function the firmware parked in D3 hot answers nothing until it is
     * put back in D0; PMCSR's low two bits are the power state. */
    {
      UINT32 pm = pci_read32(list[i].bus, list[i].dev, list[i].func, 0x84u);

      if ((pm & 3u) == 3u) {
        pci_write32(list[i].bus, list[i].dev, list[i].func, 0x84u,
                    pm & 0xFFFFFFFCu);
        kputs("[I2C] controller was in D3, sent to D0\n");
      }
    }
    {
      /* 4 KiB covers the DesignWare core (0x00..0x9C) and the LPSS private
       * block at 0x200; the mapping itself is 2 MiB granules, so this is the
       * minimum honest size, not a guess. */
      void *win = mm_map_phys(bar, 0x1000u, MM_MAP_UC);

      if (win == 0) {
        kputs("[I2C] controller window could not be mapped\n");
        continue;
      }
      base = (UINT64)(UINTN)win + (bar & 0x1FFFFFull);
    }
    if (dw_init(base) != 0) {
      kputs("[I2C] controller ");
      kputs_hex(base);
      kputs(": not a live master\n");
      continue;
    }
    /* The address the firmware's _CRS named is the one to try first; the
     * fallbacks cover a DSDT whose resource template was not parseable. */
    a = (UINT32)touchpad_slave_addr();
    if (a < 0x80u && ih_probe_one(base, (UINT8)a) == 0) {
      g_ih.state = 1;
      g_ih.fails = 0u;
      kputs("[I2C] HID device at controller ");
      kputs_hex(base);
      kputs(" addr=");
      kputs_hex((UINT64)g_ih.addr);
      kputs(" vendor=");
      kputs_hex(g_ih.vendor);
      kputs(" product=");
      kputs_hex(g_ih.product);
      kputs("\n[I2C] report desc=");
      ih_dec(g_ih.rep_desc_len);
      kputs(" bytes, input reg=");
      ih_dec(g_ih.input_reg);
      kputs(", X=");
      ih_dec((UINT32)g_ih.have_x);
      kputs(" Y=");
      ih_dec((UINT32)g_ih.have_y);
      kputs(" tip=");
      ih_dec((UINT32)g_ih.have_tip);
      kputs("\n");
      return 1;
    }
    c = 0;
    while (fallback[c] != 0u) {
      if (fallback[c] != (UINT8)a &&
          ih_probe_one(base, fallback[c]) == 0) {
        g_ih.state = 1;
        g_ih.fails = 0u;
        kputs("[I2C] HID device at controller ");
        kputs_hex(base);
        kputs(" addr=");
        kputs_hex((UINT64)g_ih.addr);
        kputs(" vendor=");
        kputs_hex(g_ih.vendor);
        kputs(" product=");
        kputs_hex(g_ih.product);
        kputs("\n");
        return 1;
      }
      c++;
    }
  }
  kputs("[I2C] HID-over-I2C buses seen=");
  ih_dec(seen);
  kputs(", no device answered\n");
  return 0;
}

/* ---- polling ----------------------------------------------------------- */

static UINT8 ih_buttons(const UINT8 *rep, UINT32 len) {
  UINT8 buttons = 0u;
  UINT32 i;

  for (i = 0u; i < g_ih.nf; i++) {
    const ihid_field_t *f = &g_ih.f[i];

    if (f->kind == 3u) { /* tip switch: contact down -> left button */
      if (ih_value(rep, len, f, 0u) != 0u) {
        buttons |= ND_BUTTON_LEFT;
      }
    } else if (f->kind >= 4u && f->kind <= 6u) {
      if (ih_value(rep, len, f, 0u) != 0u) {
        if (f->kind == 4u) {
          buttons |= ND_BUTTON_LEFT;
        } else if (f->kind == 5u) {
          buttons |= ND_BUTTON_RIGHT;
        } else {
          buttons |= ND_BUTTON_MIDDLE;
        }
      }
    }
  }
  return buttons;
}

static void ih_feed_report(const UINT8 *rep, UINT32 len) {
  const nd_display_t *d = nd_display();
  UINT32 w = 1024u;
  UINT32 h = 768u;
  INT32  sx;
  INT32  sy;
  UINT8  buttons;
  UINT32 i;

  if (d != 0 && d->available != 0u) {
    w = d->width;
    h = d->height;
  }
  if (w == 0u || h == 0u) {
    return;
  }
  if (g_ih.have_x == 0 || g_ih.have_y == 0) {
    return;
  }
  sx = 0;
  sy = 0;
  for (i = 0u; i < g_ih.nf; i++) {
    const ihid_field_t *f = &g_ih.f[i];
    UINT32 v;
    INT32  range;

    if (f->kind != 1u && f->kind != 2u) {
      continue;
    }
    v = ih_value(rep, len, f, 0u);
    range = f->lmax - f->lmin;
    if (range == 0) {
      continue;
    }
    if (f->kind == 1u) {
      sx = (INT32)(((INT32)v - f->lmin) * (INT32)(w - 1u) / range);
    } else {
      sy = (INT32)(((INT32)v - f->lmin) * (INT32)(h - 1u) / range);
    }
  }
  if (sx < 0) {
    sx = 0;
  }
  if (sy < 0) {
    sy = 0;
  }
  if ((UINT32)sx > w - 1u) {
    sx = (INT32)(w - 1u);
  }
  if ((UINT32)sy > h - 1u) {
    sy = (INT32)(h - 1u);
  }

  buttons = ih_buttons(rep, len);
  if (g_ih.last_x < 0 || g_ih.last_y < 0) {
    g_ih.last_x = sx;
    g_ih.last_y = sy;
  }
  if (sx != g_ih.last_x || sy != g_ih.last_y || buttons != g_ih.last_buttons) {
    nd_input_feed_mouse_src(ND_INPUT_SRC_TOUCHPAD, sx - g_ih.last_x,
                            sy - g_ih.last_y, buttons, 1);
    g_ih.last_x = sx;
    g_ih.last_y = sy;
    g_ih.last_buttons = buttons;
  }
}

void i2c_hid_poll(void) {
  UINT32 len;
  UINT32 want;

  if (g_ih.state != 1) {
    return;
  }
  /* The input register returns a 16-bit length followed by that many bytes of
   * report data; a zero length means nothing has happened since last poll. */
  want = 2u + (UINT32)g_ih.max_input;
  if (want > sizeof(g_ih.buf)) {
    want = sizeof(g_ih.buf);
  }
  if (ih_read_reg(g_ih.input_reg, g_ih.buf, want) != 0) {
    g_ih.fails++;
    if (g_ih.fails >= 20u) {
      kputs("[I2C] device stopped answering; polling disabled\n");
      g_ih.state = 0;
    }
    return;
  }
  g_ih.fails = 0u;
  len = ih_u16(g_ih.buf);
  if (len == 0u) {
    return;
  }
  if (len > want - 2u) {
    len = want - 2u;
  }
  ih_feed_report(g_ih.buf + 2, len);
}

int i2c_hid_state(void) { return g_ih.state; }

int i2c_hid_buses(void) { return (int)g_ih.buses; }

/* Decimal, for the log lines. */
static void ih_dec(UINT32 v) {
  char text[12];
  UINT32 n = 0u;

  if (v == 0u) {
    kputs("0");
    return;
  }
  while (v != 0u && n < 11u) {
    text[n++] = (char)('0' + (char)(v % 10u));
    v /= 10u;
  }
  while (n != 0u) {
    char one[2];

    one[0] = text[--n];
    one[1] = 0;
    kputs(one);
  }
}
