/**
 * Novus OS - pointing-device discovery (touchpad bring-up, evidence first).
 *
 * A laptop touchpad on a current Intel platform is not a PS/2 mouse. It is a
 * HID device sitting on one of the PCH's I2C controllers, described by ACPI
 * as a "PNP0C50" device whose _CRS carries a SerialBus resource naming the
 * controller and the 7-bit slave address. Neither the controller instance nor
 * the address is knowable from the PCI IDs alone: HP/Synaptics/ELAN all sit
 * behind the same LPSS block, at different addresses on different models.
 *
 * So the driver that will move the cursor starts with a read-only probe:
 *
 *   1. Find the firmware's DSDT and scan it for the HID-over-I2C markers.
 *      Around each hit the raw bytes are printed, because the resource
 *      template is small, self-describing and must be read in place - a
 *      parser written against a guessed layout is how a driver ends up
 *      poking an address that was never there.
 *   2. List every PCI serial-bus (class 0x0C/0x80) controller with its BARs,
 *      so the I2C instance the resource names can be matched to a bus/dev/fn.
 *
 * Nothing here writes to a device; it only reads firmware tables and PCI
 * configuration space, so it is safe to run on every boot.
 */

#include "uefi.h"
#include <stddef.h>
#include "acpi.h"
#include "pci.h"
#include "kernel.h"
#include "touchpad.h"

/* A DSDT is a few hundred KiB at most; anything far past that is not a table
 * whose length can be trusted as a loop bound. */
#define TP_MAX_TABLE (8u << 20)

static const char *const g_hex_digits = "0123456789abcdef";
static int g_dsdt_seen;
/* The slave address carried by the first I2C Serial Bus Connection Descriptor
 * in the firmware's tables; 0 when none parsed. */
static UINT8 g_slave_addr;
static int   g_slave_valid;

static void put_hex8(UINT8 v);

int touchpad_slave_addr(void) {
  return (g_slave_valid != 0) ? (int)g_slave_addr : -1;
}

/* ACPI 5.0 §6.4.3.7.1: Large item 0x8E whose body is
 *   [0] type (0x01 = I2C), [1] flags, [2] type-specific flags,
 *   [3] type-specific revision (0x01), [4..5] type-specific data length (6),
 *   [6..9] connection speed, [10..11] slave address.
 * Parsed forward by that layout instead of guessing from the surrounding
 * strings: the address has to be right the first time, because it is used to
 * poke a real bus. */
static void extract_serial_bus(const UINT8 *p, UINT32 len) {
  UINT32 i;

  for (i = 4u; i + 14u < len; i++) {
    const UINT8 *b = p + i + 3u;
    UINT32 dlen = (UINT32)p[i + 1u] | ((UINT32)p[i + 2u] << 8);
    UINT32 tslen;
    UINT8  addr;

    if (p[i] != 0x8Eu || b[0] != 0x01u || b[3] != 0x01u) {
      continue;
    }
    tslen = (UINT32)b[4] | ((UINT32)b[5] << 8);
    if (tslen != 6u || dlen < 14u) {
      continue;
    }
    addr = b[10];
    if (addr == 0u || addr >= 0x80u) {
      continue;
    }
    if (g_slave_valid == 0) {
      g_slave_addr = addr & 0x7Fu;
      g_slave_valid = 1;
      kputs("[TP] I2C serial bus: slave=0x");
      put_hex8(g_slave_addr);
      kputs("\n");
    }
  }
}

static void put_hex8(UINT8 v) {
  char text[3];

  text[0] = g_hex_digits[(v >> 4) & 0xFu];
  text[1] = g_hex_digits[v & 0xFu];
  text[2] = 0;
  kputs(text);
}

static int matches_at(const UINT8 *p, const char *s, UINT32 len) {
  UINT32 i;

  for (i = 0; i < len; i++) {
    if (p[i] != (UINT8)s[i]) {
      return 0;
    }
  }
  return 1;
}

/* Print the bytes bracketing a hit: the descriptor that carries the slave
 * address ends a few bytes before the name string, and the vendor/HID strings
 * that identify the panel follow it. */
static void dump_window(const UINT8 *base, UINT32 hit, UINT32 len,
                        UINT32 before, UINT32 after) {
  UINT32 start = (hit > before) ? (hit - before) : 0u;
  UINT32 end = hit + after;
  UINT32 i;

  if (end > len) {
    end = len;
  }
  for (i = start; i < end; i++) {
    put_hex8(base[i]);
    if (((i - start) & 0xFu) == 0xFu) {
      kputs("\n         ");
    } else {
      kputs(" ");
    }
  }
  kputs("\n");
}

static UINT32 g_tp_tables;
static UINT32 g_tp_tables_hit;
static UINT32 g_tp_serial_total;
static UINT32 g_tp_pnp_total;

static void scan_table(const acpi_table_t *t, const char *name) {
  const UINT8 *p;
  UINT32 len;
  UINT32 i;
  UINT32 serial = 0;
  UINT32 pnp = 0;

  if (t == 0) {
    kputs("[TP] ACPI ");
    kputs(name);
    kputs(": not found\n");
    return;
  }
  len = t->length;
  if (len < 36u || len > TP_MAX_TABLE) {
    kputs("[TP] ACPI ");
    kputs(name);
    kputs(": implausible length ");
    kputs_hex(len);
    kputs("\n");
    return;
  }
  p = (const UINT8 *)t;
  /* Count first, print second. A per-table banner for each of the tables this
   * laptop publishes (24 SSDTs plus the DSDT, all of them empty of HID-over-
   * I2C descriptors) was 2.6 KB of the boot log, and a printed character costs
   * about 1.2 ms here - the banner alone was worth more than the scan it
   * announced. A table that carries nothing now says nothing. */
  g_tp_tables++;
  for (i = 0; i + 16u < len; i++) {
    if (matches_at(p + i, "I2cSerialBus", 11u)) {
      serial++;
    } else if (matches_at(p + i, "PNP0C50", 7u)) {
      pnp++;
    }
  }
  if (serial == 0u && pnp == 0u) {
    g_tp_serial_total += serial;
    extract_serial_bus(p, len);
    return;
  }
  g_tp_tables_hit++;
  g_tp_serial_total += serial;
  g_tp_pnp_total += pnp;

  kputs("[TP] ACPI ");
  kputs(name);
  kputs(" ");
  kputs_hex(len);
  kputs(" bytes: serialbus=");
  kputs_hex(serial);
  kputs(" hid-over-i2c=");
  kputs_hex(pnp);
  kputs("\n");

  serial = 0;
  pnp = 0;
  for (i = 0; i + 16u < len; i++) {
    if (matches_at(p + i, "I2cSerialBus", 11u)) {
      serial++;
      if (serial > 3u) {
        continue;
      }
      kputs("[TP] ");
      kputs(name);
      kputs(" SerialBus at +");
      kputs_hex(i);
      if (i >= 4u) {
        /* Type-specific data ends right before the name string: the 7-bit
         * address is the two bytes ahead of the resource-source string. */
        kputs(" slave=");
        kputs_hex((UINT64)p[i - 3u] | ((UINT64)p[i - 2u] << 8));
      }
      kputs("\n");
      dump_window(p, i, len, 24u, 40u);
    } else if (matches_at(p + i, "PNP0C50", 7u)) {
      pnp++;
      if (pnp > 2u) {
        continue;
      }
      kputs("[TP] ");
      kputs(name);
      kputs(" PNP0C50 at +");
      kputs_hex(i);
      kputs("\n");
      dump_window(p, i, len, 12u, 36u);
    }
  }
  extract_serial_bus(p, len);
}

static void probe_pci(void) {
  pci_dev_t list[PCI_MAX_DEVICES];
  int n = pci_scan(list, PCI_MAX_DEVICES);
  int count = 0;
  int i;

  for (i = 0; i < n; i++) {
    if (list[i].class_code != 0x0Cu || list[i].subclass != 0x80u) {
      continue;
    }
    count++;
    kputs("[TP] serial-bus ");
    put_hex8(list[i].bus);
    kputs(":");
    put_hex8(list[i].dev);
    kputs(".");
    put_hex8(list[i].func);
    kputs(" id=");
    kputs_hex(list[i].vendor);
    kputs(":");
    kputs_hex(list[i].device);
    kputs(" bar0=");
    kputs_hex(list[i].bar[0]);
    kputs("/");
    kputs_hex(list[i].bar_size[0]);
    kputs(" bar1=");
    kputs_hex(list[i].bar[1]);
    kputs("/");
    kputs_hex(list[i].bar_size[1]);
    kputs("\n");
  }
  kputs("[TP] serial-bus controllers=");
  kputs_hex(count);
  kputs(" of ");
  kputs_hex(n);
  kputs(" pci functions\n");
}

/* ACPI tables reach the touchpad two ways: the DSDT is pointed at by the FADT
 * (it is not itself listed in the root table), and the SSDT set is listed in
 * the root table. Both are scanned, because which table carries the I2C-HID
 * device depends on the vendor's split. */
static void tp_handle_fadt(const acpi_table_t *t) {
  const UINT8 *raw = (const UINT8 *)t;
  UINT32 dsdt32 = 0;

  if (g_dsdt_seen || t->length < 44u) {
    return;
  }
  /* FADT offsets: the 32-bit DSDT pointer is at 0x28, the 64-bit X_DSDT at
   * 0x8C (past the generic-address reset register). The 32-bit one is
   * authoritative whenever it is non-zero; the extended one only exists on
   * ACPI 2.0+ firmware and is the fallback. */
  memcpy(&dsdt32, raw + 0x28, sizeof(dsdt32));
  if (dsdt32 != 0) {
    const acpi_table_t *table = acpi_validate_table((UINT64)dsdt32, "DSDT");
    if (table != NULL) {
      g_dsdt_seen = 1;
      scan_table(table, "DSDT");
      return;
    }
  }
  if (t->length >= 0x94u) {
    UINT64 dsdt64 = 0;

    memcpy(&dsdt64, raw + 0x8C, sizeof(dsdt64));
    if (dsdt64 != 0) {
      const acpi_table_t *table = acpi_validate_table(dsdt64, "DSDT");
      if (table != NULL) {
        g_dsdt_seen = 1;
        scan_table(table, "DSDT");
      }
    }
  }
}

static void tp_visit_table(const acpi_table_t *t, void *ctx) {
  (void)ctx;
  if (memcmp(t->signature, "SSDT", 4) == 0) {
    scan_table(t, "SSDT");
  } else if (memcmp(t->signature, "FACP", 4) == 0) {
    tp_handle_fadt(t);
  }
}

void touchpad_probe(UINT64 mmap, UINT64 size, UINT64 stride) {
  const acpi_table_t *facp;
  UINTN visited;

  /* The DSDT is not listed in the root table, so the proven single-table
   * lookup comes first: even when the root walk below finds nothing, the
   * firmware's main description of the platform is still scanned. */
  facp = acpi_find_table(mmap, size, stride, "FACP");
  if (facp != NULL) {
    tp_handle_fadt(facp);
  } else {
    kputs("[TP] FACP: not found\n");
  }

  visited = acpi_scan_all(mmap, size, stride, tp_visit_table, NULL);
  /* One aggregate line in place of a banner per table: visited is what the
   * root table listed, scanned also counts the DSDT reached through the FADT,
   * with-hits is how many of them actually carried an I2C-HID descriptor. */
  kputs("[TP] acpi tables visited=");
  kputs_hex(visited);
  kputs(" scanned=");
  kputs_hex((UINT64)g_tp_tables);
  kputs(" with-hits=");
  kputs_hex((UINT64)g_tp_tables_hit);
  kputs(" serialbus=");
  kputs_hex((UINT64)g_tp_serial_total);
  kputs(" hid-over-i2c=");
  kputs_hex((UINT64)g_tp_pnp_total);
  kputs("\n");
  if (!g_dsdt_seen) {
    kputs("[TP] DSDT: not reachable through FADT\n");
  }
  probe_pci();
}
