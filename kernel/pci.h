/**
 * Novus OS - PCI bus enumeration (M10 network milestone).
 *
 * Enough of PCI to bring a network card up: walk bus/device/function,
 * read the identification registers, size the BARs and turn on memory
 * decoding plus bus mastering so the device can DMA.
 *
 * Mechanism #1 (ports 0xCF8/0xCFC) is used: every PIIX/Q35 machine has it
 * and it needs no ACPI tables. MMIO BARs are reached through the kernel's
 * identity map of the low 4 GiB (kernel/mm.c maps all of it), so a BAR
 * below 4 GiB needs no extra mapping; BARs above are reported and skipped.
 */

#ifndef NOVUS_PCI_H
#define NOVUS_PCI_H

#include "uefi.h"

#define PCI_MAX_DEVICES 16

typedef struct {
  UINT8  bus;
  UINT8  dev;
  UINT8  func;
  UINT16 vendor;
  UINT16 device;
  UINT8  class_code;
  UINT8  subclass;
  UINT8  prog_if;
  UINT8  irq;
  UINT64 bar[6]; /* physical base of each memory BAR, 0 when absent */
} pci_dev_t;

int    pci_scan(pci_dev_t *out, int max);
UINT32 pci_read32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off);
UINT16 pci_read16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off);
UINT8  pci_read8(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off);
void   pci_write32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off, UINT32 val);

int pci_find(UINT16 vendor, UINT16 device, pci_dev_t *out);
int pci_find_class(UINT8 cls, UINT8 sub, int index, pci_dev_t *out);

/* Memory space decoding + bus mastering (DMA). */
void pci_enable_device(const pci_dev_t *d);

/* Boot log: one line per device (id, class, bar0). */
void pci_log(void);

#endif /* NOVUS_PCI_H */
