/**
 * Novus OS - block device abstraction, M2 Task 1 (extended in M3).
 *
 * This file defines the registration/transfer layer declared in
 * kernel/blkdev.h plus three backends: the M2 ramdisk, the M3 memory
 * image and the M3 firmware block device.
 *
 * Backend roadmap: M2 shipped only an in-RAM disk whose pages come from
 * the PMM and are reached through the direct physical map. M3 adds the
 * two backends the live/install story needs, and real host controllers
 * will follow in later milestones - an AHCI driver (PCI probe, ABAR/FIS
 * receive, command slots, DMA through the AHCI command list) and an
 * NVMe driver (PCI BAR mapping, admin queue setup, I/O
 * submission/completion queues, PRP lists). Each such driver will
 * implement only the two blk_read_fn/blk_write_fn hooks and call
 * blkdev_register(); every upper layer (partition scan, filesystem) is
 * written against this single abstraction and needs no change when the
 * hardware drivers land.
 *
 * The line that matters most in M3: firmware block devices are only
 * usable before ExitBootServices. They are registered with
 * BLKDEV_FLAG_BOOT_ONLY and blkdev_detach_boot_only() removes them just
 * before boot services are shut down, so the kernel stage can never
 * call through a protocol pointer that the firmware has invalidated.
 *
 * Ramdisk layout:
 * - Capacity up to 16 MiB == 4096 physical pages. Each page is taken
 *   individually from pmm_alloc_page(); its 32-bit page-frame number is
 *   stored in a static array, so the disk itself consumes no virtually
 *   contiguous mapping of its own.
 * - All access goes through phys_to_virt() (the PML4[256] direct map).
 *   A sector transfer is converted to a byte offset and is walked page
 *   by page, so requests that straddle 4096-byte page boundaries are
 *   handled as consecutive chunks with no alignment requirement on the
 *   caller buffer.
 */

#include "blkdev.h"
#include "kernel.h"
#include "mm.h"
#include "pmm.h"

#define RD_PAGE_SIZE   4096ULL
#define RD_MAX_BYTES   (16ULL * 1024ULL * 1024ULL) /* 16 MiB */
#define RD_MAX_PAGES   (RD_MAX_BYTES / RD_PAGE_SIZE) /* 4096 pages */

/* ------------------------------------------------------------------ */
/* Registration layer: fixed device table                              */
/* ------------------------------------------------------------------ */

static blkdev_t *blkdev_table[BLKDEV_MAX_DEVICES];
static UINTN     blkdev_used;

/* Device names are short static strings; a local compare keeps this
 * file independent of a general string library. */
static int name_eq(const char *a, const char *b) {
  UINTN i = 0;

  if (a == NULL || b == NULL) {
    return 0;
  }
  while (a[i] != '\0' && a[i] == b[i]) {
    i++;
  }
  return a[i] == b[i];
}

int blkdev_register(blkdev_t *dev) {
  UINTN i;

  /* The table stores caller-owned device objects by pointer, so hardware
   * backends keep their descriptors in static storage for the registered
   * device's lifetime. */
  if (dev == NULL || dev->name == NULL) {
    return -1;
  }
  /* A block size the layer cannot express in 512-byte sectors would make
   * every caller above it silently mis-address the device. Refuse it here
   * rather than hand out a disk that reads the wrong bytes. */
  if (dev->block_size != 0 &&
      (dev->block_size % BLKDEV_SECTOR_SIZE != 0 ||
       dev->block_size > BLKDEV_BLOCK_MAX)) {
    kputs("[BLK] unsupported block size, rejected ");
    kputs(dev->name);
    kputs("\n");
    return -1;
  }
  /* Replace a device that was registered under the same name: the boot
   * stage probes once, but a re-probe must not leave two entries
   * claiming "uefi0". */
  for (i = 0; i < blkdev_used; i++) {
    if (blkdev_table[i] == dev) {
      return (int)i;
    }
    if (name_eq(blkdev_table[i]->name, dev->name)) {
      blkdev_table[i] = dev;
      return (int)i;
    }
  }
  if (blkdev_used >= BLKDEV_MAX_DEVICES) {
    kputs("[BLK] device table full, rejected ");
    kputs(dev->name);
    kputs("\n");
    return -1;
  }
  blkdev_table[blkdev_used] = dev;
  blkdev_used++;
  return (int)(blkdev_used - 1);
}

blkdev_t *blkdev_get(void) {
  if (blkdev_used == 0) {
    return NULL;
  }
  return blkdev_table[0];
}

blkdev_t *blkdev_get_by_name(const char *name) {
  UINTN i;

  if (name == NULL) {
    return NULL;
  }
  for (i = 0; i < blkdev_used; i++) {
    if (name_eq(blkdev_table[i]->name, name)) {
      return blkdev_table[i];
    }
  }
  return NULL;
}

UINTN blkdev_count(void) {
  return blkdev_used;
}

blkdev_t *blkdev_get_at(UINTN index) {
  if (index >= blkdev_used) {
    return NULL;
  }
  return blkdev_table[index];
}

void blkdev_detach_boot_only(void) {
  UINTN i;
  UINTN kept = 0;

  for (i = 0; i < blkdev_used; i++) {
    if ((blkdev_table[i]->flags & BLKDEV_FLAG_BOOT_ONLY) != 0) {
      continue; /* firmware pointer, unusable from here on */
    }
    blkdev_table[kept] = blkdev_table[i];
    kept++;
  }
  blkdev_used = kept;
}

int blkdev_read(blkdev_t *dev, UINT64 sector, UINTN count, void *buf) {
  if (dev == NULL || dev->read == NULL) {
    return -1;
  }
  if (count == 0) {
    return 0;
  }
  /* Reject any [sector, sector + count) range past the device end. */
  if (sector > dev->sector_count ||
      (UINT64)count > dev->sector_count - sector) {
    return -1;
  }
  return dev->read(dev->ctx, sector, count, buf);
}

int blkdev_write(blkdev_t *dev, UINT64 sector, UINTN count,
                 const void *buf) {
  if (dev == NULL || dev->write == NULL) {
    return -1;
  }
  if ((dev->flags & BLKDEV_FLAG_READONLY) != 0) {
    return -1;
  }
  if (count == 0) {
    return 0;
  }
  if (sector > dev->sector_count ||
      (UINT64)count > dev->sector_count - sector) {
    return -1;
  }
  return dev->write(dev->ctx, sector, count, buf);
}

typedef struct {
  blkdev_t *parent;
  UINT64 first_sector;
  UINT64 sector_count;
} blkpart_ctx_t;

static blkpart_ctx_t blkpart_ctx[BLKDEV_MAX_DEVICES];
static blkdev_t blkpart_dev[BLKDEV_MAX_DEVICES];
static char blkpart_name[BLKDEV_MAX_DEVICES][16];
static UINTN blkpart_used;

static int blkpart_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  blkpart_ctx_t *part = (blkpart_ctx_t *)ctx;
  if (part == NULL || sector > part->sector_count ||
      (UINT64)count > part->sector_count - sector) {
    return -1;
  }
  return blkdev_read(part->parent, part->first_sector + sector, count, buf);
}

static int blkpart_write(void *ctx, UINT64 sector, UINTN count,
                         const void *buf) {
  blkpart_ctx_t *part = (blkpart_ctx_t *)ctx;
  if (part == NULL || sector > part->sector_count ||
      (UINT64)count > part->sector_count - sector) {
    return -1;
  }
  return blkdev_write(part->parent, part->first_sector + sector, count, buf);
}

blkdev_t *blkdev_partition_init(const char *name, blkdev_t *parent,
                                UINT64 first_sector, UINT64 sector_count) {
  UINTN name_len = 0;
  UINT32 spb;
  blkpart_ctx_t *ctx;
  blkdev_t *dev;

  /* Partition offsets use the public 512-byte sector unit, but a view
   * must begin and end on parent-block boundaries for whole-block devices.
   * Otherwise a partition edge could split a parent's physical block. */
  if (name == NULL || parent == NULL || sector_count == 0 ||
      first_sector > parent->sector_count ||
      sector_count > parent->sector_count - first_sector ||
      blkpart_used >= BLKDEV_MAX_DEVICES) {
    return NULL;
  }
  spb = blkdev_sectors_per_block(parent);
  if (spb == 0 || first_sector % spb != 0 || sector_count % spb != 0) {
    return NULL;
  }
  while (name[name_len] != '\0' && name_len < sizeof(blkpart_name[0]) - 1u) {
    blkpart_name[blkpart_used][name_len] = name[name_len];
    name_len++;
  }
  if (name[name_len] != '\0') {
    return NULL;
  }
  blkpart_name[blkpart_used][name_len] = '\0';
  ctx = &blkpart_ctx[blkpart_used];
  dev = &blkpart_dev[blkpart_used];
  ctx->parent = parent;
  ctx->first_sector = first_sector;
  ctx->sector_count = sector_count;
  dev->name = blkpart_name[blkpart_used];
  dev->sector_count = sector_count;
  dev->block_size = blkdev_block_size(parent);
  dev->flags = BLKDEV_FLAG_PARTITION |
               (parent->flags & (BLKDEV_FLAG_READONLY |
                                 BLKDEV_FLAG_REMOVABLE |
                                 BLKDEV_FLAG_BOOT_ONLY));
  /* Carry these parent restrictions into the view so detach/lookup policy
   * sees the same lifetime and write constraints as the parent device.
   * The partition view must not weaken the owner's hardware policy. */
  dev->ctx = ctx;
  dev->read = blkpart_read;
  dev->write = blkpart_write;
  if (blkdev_register(dev) < 0) {
    memset(ctx, 0, sizeof(*ctx));
    memset(dev, 0, sizeof(*dev));
    memset(blkpart_name[blkpart_used], 0, sizeof(blkpart_name[0]));
    return NULL;
  }
  blkpart_used++;
  return dev;
}

int blkdev_release(blkdev_t *dev) {
  UINTN i;
  UINTN j;

  if (dev == NULL) {
    return -1;
  }
  for (i = 0; i < blkdev_used; i++) {
    if (blkdev_table[i] == dev) {
      break;
    }
  }
  if (i >= blkdev_used) {
    return -1;
  }
  for (j = i; j + 1u < blkdev_used; j++) {
    blkdev_table[j] = blkdev_table[j + 1u];
  }
  blkdev_table[--blkdev_used] = NULL;

  /* Partition views live in a bump-allocated static pool. Freeing the
   * newest one restores the bump pointer; older ones are detached and
   * unusable until the next boot. */
  if (dev >= &blkpart_dev[0] && dev < &blkpart_dev[BLKDEV_MAX_DEVICES]) {
    UINTN idx = (UINTN)(dev - &blkpart_dev[0]);

    memset(&blkpart_ctx[idx], 0, sizeof(blkpart_ctx[idx]));
    memset(blkpart_name[idx], 0, sizeof(blkpart_name[idx]));
    memset(&blkpart_dev[idx], 0, sizeof(blkpart_dev[idx]));
    if (idx + 1u == blkpart_used) {
      blkpart_used = idx;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Ramdisk backend                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
  UINT64 page_count;
  UINT32 pfn[RD_MAX_PAGES]; /* physical page-frame numbers (phys/4096) */
} ramdisk_ctx_t;

static ramdisk_ctx_t ramdisk_ctx; /* .bss: 16 KiB PFN table + counter */
static blkdev_t      ramdisk_dev;

/* Core transfer walk. `is_write` selects memcpy direction; the host
 * buffer is typed as void* and only read when is_write != 0. */
static int ramdisk_xfer(ramdisk_ctx_t *rd, UINT64 sector, UINTN count,
                        void *host, int is_write) {
  UINT64 offset = sector * (UINT64)BLKDEV_SECTOR_SIZE;
  UINT64 remaining = (UINT64)count * (UINT64)BLKDEV_SECTOR_SIZE;
  UINT8 *dst_or_src = (UINT8 *)host;

  while (remaining > 0) {
    UINT64 page_idx = offset / RD_PAGE_SIZE;
    UINT64 page_off = offset % RD_PAGE_SIZE;
    UINT64 chunk = RD_PAGE_SIZE - page_off;
    UINT64 phys;
    UINT8 *page_va;

    if (chunk > remaining) {
      chunk = remaining;
    }
    if (page_idx >= rd->page_count) {
      return -1; /* defensive: range was already validated above */
    }

    phys = (UINT64)rd->pfn[page_idx] * RD_PAGE_SIZE;
    page_va = (UINT8 *)phys_to_virt(phys) + page_off;

    if (is_write) {
      memcpy(page_va, dst_or_src, chunk);
    } else {
      memcpy(dst_or_src, page_va, chunk);
    }

    dst_or_src += chunk;
    offset += chunk;
    remaining -= chunk;
  }
  return 0;
}

static int ramdisk_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  ramdisk_ctx_t *rd = (ramdisk_ctx_t *)ctx;
  if (rd == NULL) {
    return -1;
  }
  return ramdisk_xfer(rd, sector, count, buf, 0);
}

static int ramdisk_write(void *ctx, UINT64 sector, UINTN count,
                         const void *buf) {
  ramdisk_ctx_t *rd = (ramdisk_ctx_t *)ctx;
  if (rd == NULL) {
    return -1;
  }
  /* ramdisk_xfer only reads the host pointer in write mode. */
  return ramdisk_xfer(rd, sector, count, (void *)buf, 1);
}

blkdev_t *ramdisk_init(UINT64 bytes) {
  UINT64 page_count;
  UINT64 i;

  if (bytes == 0 || bytes > RD_MAX_BYTES ||
      (bytes & (RD_PAGE_SIZE - 1)) != 0) {
    return NULL;
  }
  page_count = bytes / RD_PAGE_SIZE;

  /* Allocate backing pages one at a time from the PMM. */
  for (i = 0; i < page_count; i++) {
    void *phys = pmm_alloc_page();
    if (phys == NULL) {
      /* Exhausted: hand every previously allocated page back. */
      UINT64 j;
      for (j = 0; j < i; j++) {
        pmm_free_page((void *)((UINT64)ramdisk_ctx.pfn[j] * RD_PAGE_SIZE));
        ramdisk_ctx.pfn[j] = 0;
      }
      ramdisk_ctx.page_count = 0;
      return NULL;
    }
    ramdisk_ctx.pfn[i] = (UINT32)((UINT64)phys / RD_PAGE_SIZE);
  }
  ramdisk_ctx.page_count = page_count;

  ramdisk_dev.name = "ramdisk";
  ramdisk_dev.sector_count = bytes / (UINT64)BLKDEV_SECTOR_SIZE;
  ramdisk_dev.block_size = BLKDEV_SECTOR_SIZE; /* memory: any granularity */
  ramdisk_dev.flags = 0; /* writable, non-removable, survives EBS */
  ramdisk_dev.ctx = &ramdisk_ctx;
  ramdisk_dev.read = ramdisk_read;
  ramdisk_dev.write = ramdisk_write;

  if (blkdev_register(&ramdisk_dev) < 0) {
    return NULL;
  }
  return &ramdisk_dev;
}

/* ------------------------------------------------------------------ */
/* Memory-image backend (M3, kernel stage)                             */
/* ------------------------------------------------------------------ */
/* The boot stage reads the live root off the EFI system partition into
 * reserved pages and publishes the range through shared memory; this
 * backend adopts it so the kernel can mount a filesystem without owning
 * a storage driver. The region is linearly mapped, so a transfer is one
 * memcpy - the page walk the ramdisk needs is unnecessary here. */

#define MEMIMG_MAX_DEVICES 2

typedef struct {
  UINT8  *base;  /* virtual address of the region */
  UINT64  bytes;
} memimg_ctx_t;

static memimg_ctx_t memimg_ctxs[MEMIMG_MAX_DEVICES];
static blkdev_t     memimg_devs[MEMIMG_MAX_DEVICES];
static UINTN        memimg_used;

static int memimg_xfer(memimg_ctx_t *m, UINT64 sector, UINTN count,
                       void *host, int is_write) {
  UINT64 offset = sector * (UINT64)BLKDEV_SECTOR_SIZE;
  UINT64 len = (UINT64)count * (UINT64)BLKDEV_SECTOR_SIZE;

  /* blkdev_read/write already range-check against sector_count, which
   * was derived from bytes; this is the belt-and-braces version. */
  if (m == NULL || m->base == NULL || offset + len > m->bytes) {
    return -1;
  }
  if (is_write) {
    memcpy(m->base + offset, host, len);
  } else {
    memcpy(host, m->base + offset, len);
  }
  return 0;
}

static int memimg_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  return memimg_xfer((memimg_ctx_t *)ctx, sector, count, buf, 0);
}

static int memimg_write(void *ctx, UINT64 sector, UINTN count,
                        const void *buf) {
  return memimg_xfer((memimg_ctx_t *)ctx, sector, count, (void *)buf, 1);
}

blkdev_t *blkdev_memimg_init(const char *name, UINT64 phys_base,
                             UINT64 bytes) {
  memimg_ctx_t *m;
  blkdev_t     *d;

  if (name == NULL || bytes == 0 ||
      (bytes % (UINT64)BLKDEV_SECTOR_SIZE) != 0 ||
      memimg_used >= MEMIMG_MAX_DEVICES) {
    return NULL;
  }

  m = &memimg_ctxs[memimg_used];
  d = &memimg_devs[memimg_used];

  m->base = (UINT8 *)phys_to_virt(phys_base);
  m->bytes = bytes;

  d->name = name;
  d->sector_count = bytes / (UINT64)BLKDEV_SECTOR_SIZE;
  d->block_size = BLKDEV_SECTOR_SIZE; /* memory: any granularity */
  d->flags = 0;
  d->ctx = m;
  d->read = memimg_read;
  d->write = memimg_write;

  if (blkdev_register(d) < 0) {
    return NULL;
  }
  memimg_used++;
  return d;
}

/* ------------------------------------------------------------------ */
/* Firmware block-device backend (M3, boot stage only)                 */
/* ------------------------------------------------------------------ */
/* Every entry is BLKDEV_FLAG_BOOT_ONLY: the hooks call
 * EFI_BLOCK_IO_PROTOCOL, which the firmware tears down at
 * ExitBootServices. blkdev_detach_boot_only() removes them before that
 * happens, so the kernel stage never sees one.
 *
 * MediaId is re-read on every transfer rather than cached: it changes
 * whenever the media does, and using a stale id makes the firmware
 * reject the request. */

typedef struct {
  EFI_BLOCK_IO_PROTOCOL *bio;
} uefi_ctx_t;

static uefi_ctx_t uefi_ctxs[BLKDEV_MAX_DEVICES];
static blkdev_t   uefi_devs[BLKDEV_MAX_DEVICES];

static const char *const uefi_names[BLKDEV_MAX_DEVICES] = {
    "uefi0",  "uefi1",  "uefi2",  "uefi3",  "uefi4",  "uefi5",
    "uefi6",  "uefi7",  "uefi8",  "uefi9",  "uefi10", "uefi11",
    "uefi12", "uefi13", "uefi14", "uefi15"};

/* One hardware block of scratch space. The firmware only moves whole
 * blocks, so a request that starts or ends in the middle of one - which
 * is what reading a 512-byte sector out of a 4Kn disk looks like - has to
 * go through here: the block comes in, the wanted part is taken out of it
 * or patched into it, and on a write the block goes back. Sized for
 * BLKDEV_BLOCK_MAX, the largest block this layer accepts. */
static UINT8 uefi_bounce[BLKDEV_BLOCK_MAX];

/* Move `count` 512-byte sectors to/from `buf`, in whatever block size the
 * device speaks. MediaId is re-read on every call rather than cached: it
 * changes whenever the media does, and a stale id makes the firmware
 * refuse the request. */
static int uefi_xfer(uefi_ctx_t *u, UINT64 sector, UINTN count, void *buf,
                     int is_write) {
  UINT32       block;
  UINT64       byte_off;
  UINT64       bytes;
  UINT8       *p = (UINT8 *)buf;
  UINT64       done = 0;

  if (u == NULL || u->bio == NULL || u->bio->Media == NULL) {
    return -1;
  }
  block = u->bio->Media->BlockSize;
  if (block == 0 || (block % BLKDEV_SECTOR_SIZE) != 0 ||
      block > BLKDEV_BLOCK_MAX) {
    return -1;
  }
  if (is_write && u->bio->Media->ReadOnly) {
    return -1;
  }
  bytes = (UINT64)count * (UINT64)BLKDEV_SECTOR_SIZE;
  if (bytes == 0) {
    return 0;
  }
  byte_off = sector * (UINT64)BLKDEV_SECTOR_SIZE;

  while (done < bytes) {
    UINT64      cur = byte_off + done;
    UINT64      blk = cur / block;
    UINT64      in_blk = cur % block;
    UINT64      chunk = block - in_blk;
    EFI_STATUS  status;
    UINTN       moved;

    if (chunk > bytes - done) {
      chunk = bytes - done;
    }

    if (in_blk == 0 && chunk == block) {
      /* Block-aligned head (or middle): hand every whole block to the
       * firmware in one call. This is the common case - a filesystem
       * reads in block-sized runs - and it is the fast one. */
      moved = (UINTN)((bytes - done) / block);
      if (is_write) {
        status = u->bio->WriteBlocks(u->bio, u->bio->Media->MediaId, blk,
                                     moved * block, p + done);
      } else {
        status = u->bio->ReadBlocks(u->bio, u->bio->Media->MediaId, blk,
                                    moved * block, p + done);
      }
      if (EFI_ERROR(status)) {
        return -1;
      }
      done += (UINT64)moved * block;
      continue;
    }

    /* Partial block: read it, then read out of it or write back into it. */
    status = u->bio->ReadBlocks(u->bio, u->bio->Media->MediaId, blk, block,
                                uefi_bounce);
    if (EFI_ERROR(status)) {
      return -1;
    }
    if (is_write) {
      memcpy(uefi_bounce + in_blk, p + done, (UINTN)chunk);
      /* ReadBlocks/WriteBlocks take a non-const buffer; the block layer
       * guarantees the source is not modified. */
      status = u->bio->WriteBlocks(u->bio, u->bio->Media->MediaId, blk,
                                   block, uefi_bounce);
      if (EFI_ERROR(status)) {
        return -1;
      }
    } else {
      memcpy(p + done, uefi_bounce + in_blk, (UINTN)chunk);
    }
    done += chunk;
  }
  return 0;
}

static int uefi_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  return uefi_xfer((uefi_ctx_t *)ctx, sector, count, buf, 0);
}

static int uefi_write(void *ctx, UINT64 sector, UINTN count,
                      const void *buf) {
  return uefi_xfer((uefi_ctx_t *)ctx, sector, count, (void *)buf, 1);
}

static int uefi_boot_dev = -1;

int blkdev_uefi_boot_device(void) {
  return uefi_boot_dev;
}

/* The handle Novus was loaded from, so the probe can tell which device
 * is the boot medium. A firmware is free to answer with the volume the
 * loader came from rather than the whole disk, and either is fine: the
 * answer is only used to label a device in the log and to keep the
 * installer from writing over the medium it is running from. */
static EFI_HANDLE uefi_image_device(EFI_SYSTEM_TABLE *st, EFI_HANDLE image) {
  EFI_GUID                 lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL *li = NULL;

  if (st == NULL || st->BootServices == NULL || image == NULL ||
      st->BootServices->OpenProtocol == NULL) {
    return NULL;
  }
  if (EFI_ERROR(st->BootServices->OpenProtocol(
          image, &lip_guid, (VOID **)&li, image, NULL,
          EFI_OPEN_PROTOCOL_GET_PROTOCOL)) ||
      li == NULL) {
    return NULL;
  }
  return li->DeviceHandle;
}

int blkdev_uefi_probe(EFI_SYSTEM_TABLE *st, EFI_HANDLE image) {
  EFI_GUID        guid = EFI_BLOCK_IO_PROTOCOL_GUID;
  EFI_HANDLE     *handles = NULL;
  EFI_HANDLE      boot_handle;
  EFI_STATUS      status;
  UINTN           count = 0;
  UINTN           i;
  UINTN           registered = 0;

  uefi_boot_dev = -1;
  if (st == NULL || st->BootServices == NULL ||
      st->BootServices->LocateHandleBuffer == NULL) {
    return 0;
  }
  boot_handle = uefi_image_device(st, image);

  /* Force the firmware to enumerate and connect every driver it can find
   * before we ask for handles. Without this, OVMF started from a CD-ROM
   * may never even create Block I/O handles for IDE/virtio disks that are
   * present but not part of the boot path. */
  if (st->BootServices->ConnectController != NULL) {
    st->BootServices->ConnectController(NULL, NULL, NULL, TRUE);
  }

  status = st->BootServices->LocateHandleBuffer(ByProtocol, &guid, NULL,
                                                &count, &handles);
  if (EFI_ERROR(status) || handles == NULL || count == 0) {
    return 0;
  }

  for (i = 0; i < count; i++) {
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;
    uefi_ctx_t            *u;
    blkdev_t              *d;
    UINT32                 flags = BLKDEV_FLAG_BOOT_ONLY;

    /* Ask the firmware to bind every protocol driver that claims this
     * handle. For hard-disk and CD emulation this is what makes the
     * SimpleFileSystem protocol appear on the virtual disk, without it
     * the loader cannot read files even though Block I/O is already
     * there. */
    st->BootServices->ConnectController(handles[i], NULL, NULL, TRUE);
    status = st->BootServices->HandleProtocol(handles[i], &guid,
                                            (VOID **)&bio);
    if (EFI_ERROR(status) || bio == NULL || bio->Media == NULL) {
      continue;
    }
    if (!bio->Media->MediaPresent) {
      continue; /* no media in the drive */
    }
    /* Block sizes other than 512 are hardware, not an error: a 4Kn NVMe
     * namespace and an optical disc both work through uefi_xfer(), which
     * splits a 512-byte-sector request into whole device blocks. Devices
     * coarser than BLKDEV_BLOCK_MAX cannot be addressed at all and are
     * the only ones turned away here. */
    if (bio->Media->BlockSize == 0 ||
        (bio->Media->BlockSize % BLKDEV_SECTOR_SIZE) != 0 ||
        bio->Media->BlockSize > BLKDEV_BLOCK_MAX) {
      continue;
    }
    if (registered >= BLKDEV_MAX_DEVICES) {
      break;
    }
    if (bio->Media->ReadOnly) {
      flags |= BLKDEV_FLAG_READONLY;
    }
    if (bio->Media->RemovableMedia) {
      flags |= BLKDEV_FLAG_REMOVABLE;
    }
    if (bio->Media->LogicalPartition) {
      flags |= BLKDEV_FLAG_PARTITION;
    }

    u = &uefi_ctxs[registered];
    d = &uefi_devs[registered];
    u->bio = bio;

    d->name = uefi_names[registered];
    /* LastBlock counts hardware blocks; the table below counts 512-byte
     * sectors, so the two only agree when the block is 512 bytes. */
    d->sector_count = ((UINT64)bio->Media->LastBlock + 1u) *
                      (UINT64)(bio->Media->BlockSize / BLKDEV_SECTOR_SIZE);
    d->block_size = bio->Media->BlockSize;
    d->flags = flags;
    d->ctx = u;
    d->read = uefi_read;
    d->write = uefi_write;

    if (boot_handle != NULL && handles[i] == boot_handle) {
      uefi_boot_dev = (int)registered;
    }

    if (blkdev_register(d) < 0) {
      break;
    }
    registered++;
  }

  st->BootServices->FreePool(handles);
  return (int)registered;
}
