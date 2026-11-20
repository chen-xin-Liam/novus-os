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
/* Push the device's volatile write cache to the medium. Optional: a backend
 * that leaves it NULL has nothing to make durable (memory images) or never
 * learned the command. */
typedef int (*blk_flush_fn)(void *ctx);

/* Power states a disk can be reported in. Every backend that can answer
 * agrees on these four: spinning and ready, spun up but idle (heads
 * unloaded on some drives), stopped, and parked deep enough that only a
 * reset brings it back. The block layer only cares whether the medium is
 * at rest, so this is the whole vocabulary it needs. */
#define BLKDEV_POWER_UNKNOWN 0xFFFFFFFFu
#define BLKDEV_POWER_ACTIVE  0u
#define BLKDEV_POWER_IDLE    1u
#define BLKDEV_POWER_STANDBY 2u
#define BLKDEV_POWER_SLEEP   3u

/* Spin the medium down: unload the heads and stop the platter.
 *
 * Nothing is invalidated by this. ATA and SCSI both restart the drive on
 * the next media access, so a caller never has to wake the device before
 * a transfer - it only has to make sure none is in flight, which the
 * backend's own lock already covers. Optional: a backend with no moving
 * parts (memory images, flash) leaves this NULL, and blkdev_powerdown()
 * reports success for it.
 *
 * Returns 0 when the device is stopped (or had nothing to stop), -1 when
 * the backend or the device refused the command. */
typedef int (*blk_power_fn)(void *ctx);

/* Report the current state as one of BLKDEV_POWER_*. NULL when the
 * backend cannot read it; blkdev_power_state() then answers UNKNOWN. */
typedef int (*blk_power_state_fn)(void *ctx, UINT32 *state);

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
  /* NULL when the backend has no cache it can flush; see blkdev_flush(). */
  blk_flush_fn flush;
  /* NULL when the device has nothing to spin down or never learned how. */
  blk_power_fn powerdown;
  /* NULL when the backend cannot read its power state. */
  blk_power_state_fn power_state;
  /* Set by a backend whose spin-down may run from the timer interrupt:
   * one that only ever touches a register and polls, like AHCI and IDE.
   * USB mass storage keeps a multi-step Bulk-Only state machine and must
   * never be entered from an IRQ, so it leaves this clear and is swept
   * from task context instead. */
  int power_irq_safe;
  /* Bookkeeping owned by the block layer, not by the backend: the tick of
   * the last transfer this device saw and the last state observed. Both
   * are maintained by blkdev_read/write/flush and consulted by
   * blkdev_idle_sweep(), so a backend never has to initialise them. */
  UINT64 last_active_tick;
  UINT32 power; /* BLKDEV_POWER_* */
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

/* Make every write already acknowledged by this device durable.
 *
 * A disk reports a write complete once the bytes reach its write cache, not
 * the medium, so a filesystem commit (WAL marker, superblock pointer) is
 * durable only after a flush. Without one, a power loss right after
 * novusfs commits can leave a recovery pass replaying a transaction whose
 * records the device never stored - or with an older superblock pointer
 * that no longer matches the redo it just performed.
 *
 * Returns 0 when the cache was flushed, when the device has none (memory
 * images), when the backend never learned the command, or when the device
 * is read-only; -1 on a failed command. Inline for the same reason the
 * geometry helpers above are: kernel/gpt.c and the host mkdisk/mkfsnovus
 * tools include this header without the block layer behind it. */
static inline int blkdev_flush(blkdev_t *dev) {
  if (dev == NULL) {
    return -1;
  }
  if ((dev->flags & BLKDEV_FLAG_READONLY) != 0) {
    return 0;
  }
  if (dev->flush == NULL) {
    return 0;
  }
  return dev->flush(dev->ctx);
}

/* Spin this device down, if it can be. Calling it on a device that is
 * already stopped is harmless - the ATA command is a no-op there and
 * costs one round trip. Read-only media and backends without moving parts
 * report success without touching hardware.
 *
 * Inline for the same reason blkdev_flush() is: kernel/gpt.c and the host
 * mkdisk/mkfsnovus tools include this header with no block layer behind
 * it. */
static inline int blkdev_powerdown(blkdev_t *dev) {
  if (dev == NULL) {
    return -1;
  }
  if ((dev->flags & BLKDEV_FLAG_READONLY) != 0) {
    return 0;
  }
  if (dev->powerdown == NULL) {
    return 0;
  }
  return dev->powerdown(dev->ctx);
}

/* Ask the backend what state the medium is in. `*state` is set to
 * BLKDEV_POWER_UNKNOWN whenever the answer cannot be had, so callers can
 * print or decide on it without a second NULL check. */
static inline int blkdev_power_state(blkdev_t *dev, UINT32 *state) {
  if (state == NULL) {
    return -1;
  }
  *state = BLKDEV_POWER_UNKNOWN;
  if (dev == NULL || dev->power_state == NULL) {
    return -1;
  }
  return dev->power_state(dev->ctx, state);
}

/* --- automatic spin-down ------------------------------------------------ */

/* Stop every device that has not seen a transfer for the configured idle
 * timeout. Cheap when nothing qualifies - it walks the device table and
 * compares two integers.
 *
 * `irq_context` is 1 when the caller runs from an interrupt handler (the
 * timer tick, which is where this system actually gets its chance to look
 * at idle disks: a desktop shell always leaves a runnable task behind, so
 * the kernel idle loop below is unreachable). In that mode only backends
 * that set power_irq_safe are touched - a USB Bulk-Only transfer is a
 * multi-step state machine and must not be entered from an IRQ. */
void blkdev_idle_sweep(int irq_context);

/* Idle timeout in seconds before a device is spun down; 0 disables
 * automatic spin-down entirely. The default is BLKDEV_IDLE_SECONDS. */
void   blkdev_set_idle_timeout(UINT32 seconds);
UINT32 blkdev_idle_timeout(void);
#define BLKDEV_IDLE_SECONDS_DEFAULT 120u

/* Flush and then stop every device that supports it, regardless of the
 * idle timer. For a shutdown or suspend path, where the whole machine is
 * about to stop drawing power. Returns the number of devices stopped. */
UINTN blkdev_standby_all(void);

/* Register a bounded view of a parent device. Offsets and lengths use
 * the same 512-byte sectors as blkdev_read(); the range must fit the
 * parent and align to its logical block size. */
blkdev_t *blkdev_partition_init(const char *name, blkdev_t *parent,
                                UINT64 first_sector, UINT64 sector_count);

/* Remove a device from the table and, when it is the newest partition
 * view, return its backing slot for reuse. The installer creates one view
 * per attempt; without this, retrying a failed install would exhaust the
 * sixteen fixed slots. Returns 0 on success. */
int blkdev_release(blkdev_t *dev);

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
