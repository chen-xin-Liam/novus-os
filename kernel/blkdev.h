/**
 * Novus OS - block device abstraction, M2 Task 1 (extended in M3).
 *
 * Every device exposed through this layer is addressed in fixed
 * BLKDEV_SECTOR_SIZE (512-byte) sectors: the same logical granularity
 * used by legacy MBR/GPT partitioning and by UEFI block I/O. Higher
 * layers (filesystems, partition maps) talk only to this API and never
 * touch the backing hardware directly.
 *
 * M2 defined a single device slot. M3 needs several at once - the live
 * root image, the installed system partition and an install target - so
 * the slot became a small fixed table. blkdev_get() still answers with
 * the first registered device, which is what the M2 NovusFS selftest
 * relies on; every new caller addresses devices by name or index.
 *
 * Two backends were added for M3 and they live on opposite sides of
 * ExitBootServices:
 *
 * - blkdev_uefi_probe(): registers every firmware block device. The
 *   hooks call EFI_BLOCK_IO_PROTOCOL, so the devices are only usable
 *   while boot services are alive. They are flagged BLKDEV_FLAG_BOOT_ONLY
 *   and blkdev_detach_boot_only() drops them before ExitBootServices,
 *   which is what keeps the table free of dangling firmware pointers in
 *   the kernel stage. This is why Merkle verification and installation
 *   happen in the boot stage (README 2.2).
 * - blkdev_memimg_init(): adopts a region of memory that the boot stage
 *   already filled, so the kernel can mount the live root without a
 *   storage driver of its own.
 *
 * Real AHCI and NVMe drivers arriving in later milestones register
 * themselves through the exact same blkdev_t interface, so no upper
 * layer changes when hardware storage appears (see the note at the top
 * of kernel/blkdev.c).
 */

#ifndef NOVUS_BLKDEV_H
#define NOVUS_BLKDEV_H

#include "uefi.h"

#ifndef NULL
#define NULL ((VOID *)0)
#endif

/* Logical sector size of every registered device. This is the unit the
 * whole block layer - and every layer above it - counts in, whatever the
 * hardware underneath is really made of. */
#define BLKDEV_SECTOR_SIZE 512

/* Largest device block size the firmware backend will drive. NVMe
 * namespaces and some SAS/SATA drives present 4096-byte logical blocks
 * ("4Kn"); anything coarser than this is not addressable in 512-byte
 * units without a larger bounce buffer, so it is refused rather than
 * silently mis-read. */
#define BLKDEV_BLOCK_MAX 4096

/* Capacity of the device table. A current desktop presents more devices
 * than it used to: one or two NVMe namespaces, an AHCI disk, the optical
 * drive and a card reader all show up as block I/O handles, and the
 * installer needs to see the target as well as the medium it booted
 * from. */
#define BLKDEV_MAX_DEVICES 16

/* Device properties, reported by the backend and used to pick an
 * install target (a writable, non-removable whole disk). */
#define BLKDEV_FLAG_BOOT_ONLY   0x00000001u /* dies at ExitBootServices */
#define BLKDEV_FLAG_READONLY    0x00000002u /* media or backend is RO */
#define BLKDEV_FLAG_REMOVABLE   0x00000004u /* CD-ROM, card reader, ... */
#define BLKDEV_FLAG_PARTITION   0x00000008u /* logical partition, not a disk */

/* Backend hooks. `ctx` is the per-device private pointer stored in the
 * blkdev_t. Both transfer `count` contiguous sectors starting at
 * `sector`; return 0 on success and -1 on any error (bad device state,
 * out-of-range sector range, backend failure). */
typedef int (*blk_read_fn)(void *ctx, UINT64 sector, UINTN count, void *buf);
typedef int (*blk_write_fn)(void *ctx, UINT64 sector, UINTN count,
                            const void *buf);

typedef struct blkdev {
  const char  *name;         /* short static name, e.g. "ramdisk" */
  UINT64       sector_count; /* total addressable 512-byte sectors */
  UINT32       block_size;   /* real logical block size of the hardware;
                              * 512 on anything that is not a 4Kn drive.
                              * Transfers are still counted in 512-byte
                              * sectors - a backend whose blocks are
                              * larger splits them itself. */
  UINT32       flags;        /* BLKDEV_FLAG_* */
  void        *ctx;          /* backend-private data */
  blk_read_fn  read;
  blk_write_fn write;
} blkdev_t;

/* Hardware block size in bytes, how many 512-byte sectors that is, and
 * the two conversions every on-disk format needs: GPT, FAT and partition
 * tables all count in *their own* blocks, which on a 4Kn disk are eight
 * 512-byte sectors each. Addressing a 4Kn disk's LBA 1 as sector 1 reads
 * the middle of its boot block, which is how a drive ends up looking
 * unpartitioned - or, for the installer, unusable.
 *
 * A device that never filled the field in is assumed to be 512: that is
 * what every backend but the firmware one is.
 *
 * These are inline because kernel/gpt.c is linked into the host mkdisk
 * tool on its own, with none of the block layer around it. */
static inline UINT32 blkdev_block_size(const blkdev_t *dev) {
  if (dev == NULL || dev->block_size == 0) {
    return BLKDEV_SECTOR_SIZE;
  }
  return dev->block_size;
}

static inline UINT32 blkdev_sectors_per_block(const blkdev_t *dev) {
  return blkdev_block_size(dev) / BLKDEV_SECTOR_SIZE;
}

static inline UINT64 blkdev_lba_to_sector(const blkdev_t *dev, UINT64 lba) {
  return lba * (UINT64)blkdev_sectors_per_block(dev);
}

static inline UINT64 blkdev_lba_count(const blkdev_t *dev,
                                      UINT64 lba_count) {
  return lba_count * (UINT64)blkdev_sectors_per_block(dev);
}

/* Register a block device. Returns its index in the table, or -1 when
 * the table is full, `dev` is NULL, or the block size is not a multiple
 * of 512 no larger than BLKDEV_BLOCK_MAX. The device object must outlive
 * its registration (backends use static storage). */
int blkdev_register(blkdev_t *dev);

/* Return the first registered device, or NULL if none. Kept for the M2
 * NovusFS selftest; new code should use blkdev_get_by_name(). */
blkdev_t *blkdev_get(void);

/* Look a device up by the name its backend gave it ("ramdisk", "live",
 * "uefi0", ...). Returns NULL when no device carries that name. */
blkdev_t *blkdev_get_by_name(const char *name);

/* Table accessors for enumeration. */
UINTN     blkdev_count(void);
blkdev_t *blkdev_get_at(UINTN index);

/* Drop every BLKDEV_FLAG_BOOT_ONLY device. The boot stage must call
 * this before ExitBootServices: firmware protocol pointers are invalid
 * afterwards and a call through them would fault rather than fail. */
void blkdev_detach_boot_only(void);

/* Sector-range wrappers around the backend hooks. They validate the
 * device and reject ranges crossing dev->sector_count; a zero count is
 * a successful no-op. Writes to a read-only device are rejected.
 * Returns 0 on success, -1 on error. */
int blkdev_read(blkdev_t *dev, UINT64 sector, UINTN count, void *buf);
int blkdev_write(blkdev_t *dev, UINT64 sector, UINTN count,
                 const void *buf);

/* Register a bounded view of a parent device. Offsets and lengths use
 * the same 512-byte sectors as blkdev_read(); the range must fit the
 * parent and align to its logical block size. */
blkdev_t *blkdev_partition_init(const char *name, blkdev_t *parent,
                                UINT64 first_sector, UINT64 sector_count);

/* Ramdisk backend.
 *
 * Allocates `bytes` of backing storage one 4096-byte page at a time via
 * pmm_alloc_page(), records the physical pages and reaches them through
 * the direct physical map (phys_to_virt). `bytes` must be non-zero,
 * 4096-aligned and at most 16 MiB. Registers the resulting device and
 * returns it, or NULL on bad arguments / PMM exhaustion (pages already
 * allocated are handed back to the PMM on failure). */
blkdev_t *ramdisk_init(UINT64 bytes);

/* Memory-image backend (M3, kernel stage).
 *
 * Adopts a region of physical memory that already holds a filesystem
 * image - the boot stage reads the live root off the EFI system
 * partition into reserved pages and hands the range over through shared
 * memory. The region is reached through the direct physical map, so
 * this backend is kernel-stage only: the boot stage still runs on the
 * firmware's identity mapping, where that address is unmapped.
 *
 * `bytes` must be non-zero and 512-aligned. Returns NULL on bad
 * arguments or when no slot is free. */
blkdev_t *blkdev_memimg_init(const char *name, UINT64 phys_base,
                             UINT64 bytes);

/* Firmware block-device backend (M3, boot stage only).
 *
 * Enumerates every EFI_BLOCK_IO_PROTOCOL handle and registers it as
 * "uefi0", "uefi1", ... with BLKDEV_FLAG_BOOT_ONLY set. Whatever the
 * firmware has a driver for - IDE, AHCI/SATA, NVMe, USB, virtio - arrives
 * here already abstracted, so there is no per-controller code to write:
 * the device is used through the same protocol on all of them.
 *
 * Devices are no longer filtered by block size. A 4Kn NVMe namespace
 * (4096-byte blocks) or an optical disc (2048-byte blocks) is registered
 * like any other and its transfers are split into whole hardware blocks
 * behind the 512-byte-sector interface; skipping them meant a machine
 * whose only disk is 4Kn had no disk at all as far as Novus could tell,
 * and the installer could never offer it as a target.
 *
 * Returns the number of devices registered (0 on failure), which is
 * also 0 when boot services are gone.
 *
 * `image` is this loader's own image handle. It is optional but worth
 * passing: it is how the probe works out which of the devices Novus
 * actually booted from, which the boot stage reports and the installer
 * uses to avoid offering the medium it is running from as a target. */
int blkdev_uefi_probe(EFI_SYSTEM_TABLE *st, EFI_HANDLE image);

/* Index of the device Novus was started from, or -1 when it could not be
 * determined (no image handle was passed, or the firmware did not give
 * the boot device a Block I/O protocol of its own). Block devices only,
 * so it is a blkdev_get_at() index, not a device name. */
int blkdev_uefi_boot_device(void);

#endif /* NOVUS_BLKDEV_H */
