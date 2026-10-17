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

/* Logical sector size of every registered device. */
#define BLKDEV_SECTOR_SIZE 512

/* Capacity of the device table. The boot stage needs at most a handful:
 * firmware block devices plus the live root. */
#define BLKDEV_MAX_DEVICES 8

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
  UINT32       flags;        /* BLKDEV_FLAG_* */
  void        *ctx;          /* backend-private data */
  blk_read_fn  read;
  blk_write_fn write;
} blkdev_t;

/* Register a block device. Returns its index in the table, or -1 when
 * the table is full or `dev` is NULL. The device object must outlive its
 * registration (backends use static storage). */
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
 * "uefi0", "uefi1", ... with BLKDEV_FLAG_BOOT_ONLY set. Devices whose
 * media block size is not BLKDEV_SECTOR_SIZE are skipped: Novus
 * addresses everything in 512-byte sectors and a CD-ROM (2048-byte
 * blocks) is reached through the firmware FAT driver instead, not
 * through block I/O.
 *
 * Returns the number of devices registered (0 on failure), which is
 * also 0 when boot services are gone. */
int blkdev_uefi_probe(EFI_SYSTEM_TABLE *st);

#endif /* NOVUS_BLKDEV_H */
