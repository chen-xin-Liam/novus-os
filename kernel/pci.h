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

/* A current desktop board exposes more than sixteen functions (bridges,
 * xHCI, SATA, the GPU, audio, SMBus, ...), and the device manager lists
 * everything the bus has: the table is large enough that a whole machine
 * fits instead of the first sixteen functions. */
#define PCI_MAX_DEVICES 32

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
  UINT64 bar[6];     /* reachable base (< 4 GiB), 0 when absent or above  */
  UINT64 bar64[6];   /* the full 64-bit base the firmware programmed       */
  UINT64 bar_size[6];/* length of each memory BAR in bytes, 0 when not one */
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

/* Move a memory BAR to another physical address. The firmware sometimes
 * places a 64-bit BAR above the 4 GiB window the kernel's identity map
 * covers; the kernel then owns the device and has to give it an address
 * it can actually reach. The caller provides the new base; the window
 * must already be reserved by the caller. */
int pci_bar_assign(pci_dev_t *d, UINT8 index, UINT64 addr);

/* Boot log: one line per device (id, class, bar0). */
void pci_log(void);

#endif /* NOVUS_PCI_H */
