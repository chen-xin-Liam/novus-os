/**
 * Novus OS - device table (M12 device manager).
 *
 * The machine's devices, normalised into one flat table the device manager
 * walks: a root node, one node per category, and one node per device. PCI
 * is scanned once at boot and cached here, because sizing a BAR writes
 * 0xFFFFFFFF over every window and must never run while a driver is
 * moving data. Everything else (block devices, network controllers, the
 * graphics outputs, the CPU, the ACPI firmware id, USB) is cheap to read
 * from its own subsystem each time the table is rebuilt.
 *
 * `parent` links a node to its category, so a client can build the tree
 * the Windows device manager shows without a second query. Property
 * fields are generated on demand per (device, tab): the value strings are
 * far longer than one 128-byte sysinfo reply, so the client pages through
 * them.
 */

#ifndef NOVUS_DEVTAB_H
#define NOVUS_DEVTAB_H

#include "uefi.h"

#define DEVTAB_MAX 48
#define DEVTAB_NONE 0xFFFFu /* `parent` of the root node */

/* Categories: also the tree's group rows. The client owns the labels, so
 * the ids are the ABI and the words are not. */
#define DEV_CAT_ROOT 0u
#define DEV_CAT_COMPUTER 1u
#define DEV_CAT_PROCESSOR 2u
#define DEV_CAT_FIRMWARE 3u
#define DEV_CAT_DISPLAY 4u
#define DEV_CAT_MONITOR 5u
#define DEV_CAT_DISK 6u
#define DEV_CAT_STORAGE 7u
#define DEV_CAT_NET 8u
#define DEV_CAT_USB 9u
#define DEV_CAT_HID 10u
#define DEV_CAT_INPUT 11u
#define DEV_CAT_SYSTEM 12u
#define DEV_CAT_UNKNOWN 13u
#define DEV_CAT_MAX 13u

/* Where a node's facts come from: the index into that subsystem's own
 * table is kept in devnode_t.idx. */
#define DEV_KIND_ROOT 0u
#define DEV_KIND_CATEGORY 1u
#define DEV_KIND_PCI 2u
#define DEV_KIND_BLK 3u
#define DEV_KIND_NET 4u
#define DEV_KIND_OUTPUT 5u
#define DEV_KIND_CPU 6u
#define DEV_KIND_FIRMWARE 7u
#define DEV_KIND_USB 8u
#define DEV_KIND_PS2 9u

/* The status column of the Windows device manager: a healthy device, one
 * that needs attention (no driver), and the rest. */
#define DEV_STATUS_OK 0u
#define DEV_STATUS_WARN 1u
#define DEV_STATUS_INFO 2u

/* Node flags (wire level, the client formats them). */
#define DEV_FLAG_REMOVABLE 0x01u
#define DEV_FLAG_PRIMARY 0x02u

/* Property tabs. */
#define DEV_TAB_GENERAL 0u
#define DEV_TAB_DRIVER 1u
#define DEV_TAB_DETAILS 2u
#define DEV_TAB_EVENTS 3u
#define DEV_TAB_RESOURCES 4u
#define DEV_TAB_MAX 4u

/* Field keys. The client maps them to labels and renders the value; a key
 * is stable wire data, so never renumber one. */
#define DEV_F_MANUF 1u
#define DEV_F_LOCATION 2u
#define DEV_F_PROVIDER 3u
#define DEV_F_DRVDATE 4u
#define DEV_F_DRVVER 5u
#define DEV_F_SIGNER 6u

#define DEV_F_DESC 10u
#define DEV_F_INSTPATH 11u
#define DEV_F_HWIDS 12u
#define DEV_F_COMPATID 13u
#define DEV_F_STATE 14u
#define DEV_F_PROBLEM 15u
#define DEV_F_CONFIG 16u
#define DEV_F_CLASS 17u
#define DEV_F_CLASSGUID 18u
#define DEV_F_INF 19u
#define DEV_F_SERVICE 20u
#define DEV_F_MATCHID 21u
#define DEV_F_RANK 22u
#define DEV_F_CAPACITY 23u
#define DEV_F_MAC 24u
#define DEV_F_LINK 25u
#define DEV_F_CORES 26u
#define DEV_F_CPUID 27u
#define DEV_F_RAM 28u
#define DEV_F_FWVER 29u

/* Events: two fields share a `group` (the event's row number). */
#define DEV_F_EVTIME 40u
#define DEV_F_EVDESC 41u

/* Resources: `group` is the BAR index, or 0 for the IRQ row. */
#define DEV_F_MEMRANGE 50u
#define DEV_F_IORANGE 51u
#define DEV_F_IRQ 52u
#define DEV_F_CONFLICT 53u

#define DEVTAB_VALUE_MAX 56u

typedef struct {
  UINT16 parent; /* DEVTAB_NONE on the root */
  UINT8  class;  /* DEV_CAT_* */
  UINT8  kind;   /* DEV_KIND_* */
  UINT8  status; /* DEV_STATUS_* */
  UINT8  idx;    /* index in the backing subsystem table */
  UINT8  flags;  /* DEV_FLAG_* */
  UINT8  pad;
  char   name[40];
} devnode_t;

typedef struct {
  UINT8 key;   /* DEV_F_* */
  UINT8 group; /* event row or BAR index, 0 when unused */
  char  value[DEVTAB_VALUE_MAX];
} devfield_t;

/* Scan and cache PCI (the one call with side effects) and record the boot
 * time the event page shows. Call it once, after every storage/USB driver
 * has finished probing and before the network stack starts: no device is
 * transferring at that point, so the BAR probe cannot disturb a DMA. */
void devtab_init(UINT64 mmap, UINT64 size, UINT64 stride);

/* Rebuild the dynamic part of the table (block devices, network, outputs,
 * USB). Safe at any time: PCI comes from the boot-time cache. */
void devtab_refresh(void);

int devtab_count(void);
const devnode_t *devtab_at(int index);

/* Property fields of (dev, tab), in display order. A tab holds at most
 * DEVTAB_FIELD_MAX rows; the client pages through them because one sysinfo
 * reply cannot carry them all. */
int devtab_field_count(int dev, UINT8 tab);
int devtab_field_at(int dev, UINT8 tab, int index, devfield_t *out);

#endif /* NOVUS_DEVTAB_H */
