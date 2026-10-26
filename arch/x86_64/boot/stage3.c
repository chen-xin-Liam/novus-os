/**
 * Novus OS - M3 boot-stage orchestration (A core, BSP).
 *
 * Everything that touches real hardware happens here, before
 * ExitBootServices. Once boot services are gone the firmware's block I/O
 * and FAT drivers are gone with them, so no later stage can read a disk
 * or a file - which is why this file, and not the kernel, is where the
 * system partition is located, where its Merkle tree is verified, where
 * the live root is read off the ESP, and where an install is performed.
 *
 * What it leaves behind for the kernel is written into the spec 2.3
 * shared memory block: the integrity verdict, the root hash, and the
 * physical range of the reserved pages holding the live root image.
 *
 * Ordering rules that matter:
 *
 *   - block devices are probed first, because everything else needs one;
 *   - the NovusFS allocator is pointed at AllocatePool for the duration
 *     of the boot stage and restored to the kernel heap before the
 *     kernel starts, or the kernel would call into dead boot services;
 *   - boot-only devices are detached as the very last step before
 *     ExitBootServices, so no firmware pointer survives into the kernel.
 *
 * Nothing here halts on failure. A missing system partition, a failed
 * verification or an unreadable ESP is reported and the boot continues
 * in whatever degraded form is left (README 3.4, 17).
 */

#include "bootio.h"
#include "uefi.h"
#include "kernel.h"
#include "nd.h"
#include "shared_mem.h"
#include "blkdev.h"
#include "gpt.h"
#include "novusfs.h"
#include "nkm.h"

#ifndef CONFIG_NOVUS_MERKLE
#define CONFIG_NOVUS_MERKLE 1
#endif
#ifndef CONFIG_NOVUS_NKM
#define CONFIG_NOVUS_NKM 1
#endif
#ifndef CONFIG_NOVUS_INSTALLER
#define CONFIG_NOVUS_INSTALLER 1
#endif
/* The text installer is the fallback for a machine with no framebuffer.
 * The default path boots the kernel and shows the graphical wizard, so
 * this stays off unless a config turns it on. */
#ifndef CONFIG_NOVUS_INSTALLER_TEXT
#define CONFIG_NOVUS_INSTALLER_TEXT 0
#endif

/* EFI_ALLOCATE_TYPE: 0 == AllocateAnyPages. */
#define NOVUS_EFI_ALLOCATE_ANY_PAGES 0

static EFI_HANDLE         g_image;
static EFI_SYSTEM_TABLE  *g_st;
static EFI_BOOT_SERVICES *g_bs;

static EFI_FILE_PROTOCOL *g_esp_root;

/* Live root: reserved pages, handed to the kernel through shared
 * memory. Reserved memory is not reported as conventional, so the PMM
 * will not hand it out again after ExitBootServices. */
static UINT64 g_live_base;
static UINT64 g_live_size;

/* ESP template for the in-system installer (BOOTX64.EFI + SYSTEM.IMG).
 * Staged for the same reason as the live root: after ExitBootServices
 * nothing can read the live medium any more. */
static UINT64 g_esp_tpl_base;
static UINT64 g_esp_tpl_size;

/* Root USB port the live medium was booted from (0 = not a USB device).
 * The kernel's USB driver marks the disk on that port as the running
 * system, so the installer can refuse to write over it. */
static UINT32 g_boot_usb_port;
static UINT32 g_boot_is_usb;

/* ------------------------------------------------------------------ */
/* Allocator for filesystem buffers                                    */
/* ------------------------------------------------------------------ */
/* The boot stage has no kernel heap. NovusFS takes its allocator
 * through an explicit setter, so this is where EFI AllocatePool is
 * spliced in - and restored again in stage3_finish(). */

/* Large EFI pool allocations are unreliable; track anything over 1 MiB
 * and use AllocatePages / FreePages for it. */
#define EFI_BIG_THRESHOLD (1u << 20)

typedef struct {
  VOID   *ptr;
  UINT64  pages;
} efi_big_alloc_t;

static efi_big_alloc_t efi_big_allocs[8];
static UINTN           efi_big_alloc_count = 0;

static void *efi_alloc(UINT64 bytes) {
  VOID *p = NULL;
  UINTN n = (UINTN)bytes;

  if (g_bs == NULL) {
    return NULL;
  }
  if (n == 0) {
    n = 1;
  }
  if (bytes >= EFI_BIG_THRESHOLD && efi_big_alloc_count < 8) {
    EFI_PHYSICAL_ADDRESS addr = 0;
    UINT64               pages = (bytes + 4095u) / 4096u;

    if (g_bs->AllocatePages(NOVUS_EFI_ALLOCATE_ANY_PAGES, EfiLoaderData,
                            (UINTN)pages, &addr) == EFI_SUCCESS) {
      efi_big_allocs[efi_big_alloc_count].ptr = (VOID *)(UINTN)addr;
      efi_big_allocs[efi_big_alloc_count].pages = pages;
      efi_big_alloc_count++;
      return (VOID *)(UINTN)addr;
    }
  }
  if (g_bs->AllocatePool(EfiLoaderData, n, &p) != EFI_SUCCESS) {
    return NULL;
  }
  return p;
}

static void efi_free(void *ptr) {
  UINTN i;

  if (g_bs == NULL || ptr == NULL) {
    return;
  }
  for (i = 0; i < efi_big_alloc_count; i++) {
    if (efi_big_allocs[i].ptr == ptr) {
      g_bs->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)ptr,
                      (UINTN)efi_big_allocs[i].pages);
      efi_big_alloc_count--;
      if (i < efi_big_alloc_count) {
        efi_big_allocs[i] = efi_big_allocs[efi_big_alloc_count];
      }
      return;
    }
  }
  if (g_bs != NULL && ptr != NULL) {
    g_bs->FreePool(ptr);
  }
}

/* ------------------------------------------------------------------ */
/* EFI system partition: files through the firmware FAT driver         */
/* ------------------------------------------------------------------ */

static EFI_STATUS open_by_guid(EFI_HANDLE handle, EFI_GUID *guid,
                               VOID **out) {
  return g_bs->OpenProtocol(handle, guid, out, g_image, NULL,
                            EFI_OPEN_PROTOCOL_GET_PROTOCOL);
}

/* Open the FAT volume the image itself was loaded from. Using the
 * loaded-image DeviceHandle rather than the first SimpleFileSystem
 * instance is what makes the choice correct on a machine with several
 * FAT volumes - an internal disk and a USB stick, say. */
/* Which of the two ways of reaching the ESP was used. It is reported
 * because the difference is not academic: a firmware that offers no
 * SimpleFileSystem for the boot image is not broken, it just means every
 * file the loader needs has to come through the built-in FAT reader, and
 * on a machine that then cannot read something, this line says which
 * half of the pair is missing. */
static const char *g_esp_via = "none";

/* The root port of a USB device path: the first messaging node of type
 * USB carries the port on the hub it hangs off. Zero when the live medium
 * is not USB (a CD or a hard disk). */
static void say(const CHAR16 *s);
static void say_line(const CHAR16 *s);

/* Is the volume we booted from sitting on a USB controller?
 *
 * The loaded image only carries a file path node, but the volume handle
 * knows the whole chain - the controller and the port the medium sits on.
 * That is the address the kernel's USB driver compares against. */
static UINT32 volume_usb_port(EFI_HANDLE volume) {
  EFI_GUID                   dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
  EFI_DEVICE_PATH_PROTOCOL  *dp = NULL;
  UINT8                     *p;

  if (volume == NULL ||
      open_by_guid(volume, &dp_guid, (VOID **)&dp) != EFI_SUCCESS ||
      dp == NULL) {
    return 0;
  }
  p = (UINT8 *)dp;
  while (p[0] != 0x7Fu) { /* END_ENTIRE_DEVICE_PATH */
    UINT16 len = (UINT16)p[2] | (UINT16)((UINT16)p[3] << 8);

    if (len < 4u) {
      break;
    }
    if (p[0] == 0x03u && p[1] == 0x05u) { /* MESSAGING / USB */
      g_boot_is_usb = 1;
      return (UINT32)p[4];
    }
    p += len;
  }
  return 0;
}

static int esp_open(void) {
  EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL       *li = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;

  if (open_by_guid(g_image, &lip_guid, (VOID **)&li) == EFI_SUCCESS &&
      li != NULL && li->DeviceHandle != NULL) {
    if (open_by_guid(li->DeviceHandle, &sfs_guid, (VOID **)&sfs) ==
            EFI_SUCCESS &&
        sfs != NULL) {
      if (sfs->OpenVolume(sfs, &g_esp_root) == EFI_SUCCESS &&
          g_esp_root != NULL) {
        g_esp_via = "firmware FS on the boot device";
        g_boot_usb_port = volume_usb_port(li->DeviceHandle);
        return 0;
      }
    }
  }

  /* Fallback: any SimpleFileSystem instance. Still better than not
   * booting, and the log says so. */
  if (g_bs->LocateProtocol(&sfs_guid, NULL, (VOID **)&sfs) == EFI_SUCCESS &&
      sfs != NULL && sfs->OpenVolume(sfs, &g_esp_root) == EFI_SUCCESS &&
      g_esp_root != NULL) {
    g_esp_via = "firmware FS, first volume offered";
    return 0;
  }
  g_esp_root = NULL;
  g_esp_via = "none";
  return -1;
}

/* Forward: the built-in FAT32 reader lives below the partition
 * wrapper, but esp_read may call it as a fallback. */
static int fat32_read_file(const CHAR16 *path, UINT8 **out_buf,
                           UINT64 *out_len);

/* Read a whole file into a freshly allocated pool buffer. Uses the
 * firmware's SimpleFileSystem when it is available; otherwise falls
 * back to the built-in FAT32 reader, which is needed on firmwares that
 * only expose Block I/O for the boot device. */
/* Forward: the console helpers live further down, below the FAT reader
 * this file already declares ahead of its own definition. */
static void esp_report_route(const char *via, blkdev_t *dev);

static int esp_read(const CHAR16 *path, UINT8 **out_buf, UINT64 *out_len) {
  EFI_GUID           info_guid = EFI_FILE_INFO_ID;
  UINT8              info_buf[512];
  UINTN              info_size = sizeof(info_buf);
  EFI_FILE_PROTOCOL *f = NULL;
  UINT64             size;
  UINT8             *buf;
  UINTN              got;

  if (g_esp_root == NULL) {
    return fat32_read_file(path, out_buf, out_len);
  }
  if (g_esp_root->Open(g_esp_root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ,
                       0) != EFI_SUCCESS ||
      f == NULL) {
    return fat32_read_file(path, out_buf, out_len);
  }
  if (f->GetInfo(f, &info_guid, &info_size, info_buf) != EFI_SUCCESS) {
    f->Close(f);
    return fat32_read_file(path, out_buf, out_len);
  }
  size = ((EFI_FILE_INFO *)info_buf)->FileSize;
  if (size == 0 || size > (64u * 1024u * 1024u)) {
    f->Close(f);
    return -1;
  }
  buf = (UINT8 *)efi_alloc(size);
  if (buf == NULL) {
    f->Close(f);
    return -1;
  }
  got = (UINTN)size;
  if (f->Read(f, &got, buf) != EFI_SUCCESS || (UINT64)got != size) {
    efi_free(buf);
    f->Close(f);
    return -1;
  }
  f->Close(f);
  *out_buf = buf;
  *out_len = size;
  esp_report_route(g_esp_via, NULL);
  return 0;
}

/* Read a file into reserved pages. The bytes come through pool memory
 * first because a file handle reads into an ordinary buffer; the copy is
 * what makes the final home survive ExitBootServices. */
static int esp_read_reserved(const CHAR16 *path, UINT64 *out_base,
                             UINT64 *out_size) {
  UINT8               *tmp = NULL;
  UINT64               len = 0;
  UINT64               pages;
  EFI_PHYSICAL_ADDRESS addr = 0;

  if (esp_read(path, &tmp, &len) != 0) {
    return -1;
  }
  pages = (len + 4095u) / 4096u;
  if (g_bs->AllocatePages(NOVUS_EFI_ALLOCATE_ANY_PAGES, EfiReservedMemoryType,
                          (UINTN)pages, &addr) != EFI_SUCCESS) {
    efi_free(tmp);
    return -1;
  }
  memcpy((VOID *)(UINTN)addr, tmp, len);
  efi_free(tmp);
  *out_base = (UINT64)addr;
  *out_size = len;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Built-in FAT32 reader                                               */
/* ------------------------------------------------------------------ */
/* Some firmwares (notably OVMF under hard-disk emulation) expose the
 * boot device as Block I/O but do not bind SimpleFileSystem. Rather than
 * fail to load the live root, this tiny reader parses the FAT32 volume
 * directly. It only handles the image this very boot stage produces:
 * FAT32, 512-byte sectors, 8.3 short names in the root directory. */

typedef struct {
  blkdev_t *dev;
  UINT32    bytes_per_sector;
  UINT32    sectors_per_block; /* bytes_per_sector / 512: a FAT sector on
                                * a 4Kn volume is eight block-layer ones */
  UINT32    sectors_per_cluster;
  UINT32    fat_start;
  UINT32    data_start;
  /* Root directory: FAT16 uses a fixed sector range, FAT32 uses a
   * cluster chain starting at root_cluster. */
  UINT32    root_lba;
  UINT32    root_sectors;
  UINT32    root_cluster;
  UINT32    fat_type; /* 16 or 32 */
  UINT8    *fat;
  UINT32    fat_sectors;
} fat32_ctx_t;

/* `lba` and `count` are in the volume's own sectors, which is what every
 * BPB field is counted in; the block layer counts 512-byte sectors. They
 * are the same number except on a 4Kn volume, where they differ by eight
 * and reading LBA 1 as sector 1 fetches the wrong bytes. */
static int fat32_read_sectors(fat32_ctx_t *ctx, UINT64 lba, UINTN count,
                              void *buf) {
  return blkdev_read(ctx->dev, lba * (UINT64)ctx->sectors_per_block,
                     count * (UINTN)ctx->sectors_per_block, buf);
}

static UINT32 fat32_get(fat32_ctx_t *ctx, UINT32 clus) {
  UINT8 *p = ctx->fat + (UINT64)clus * (ctx->fat_type == 16 ? 2u : 4u);

  if (ctx->fat_type == 16) {
    UINT32 v = (UINT32)p[0] | ((UINT32)p[1] << 8);

    return (v >= 0xfff8u) ? 0x0fffffffu : v;
  }
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static UINT64 fat32_cluster_lba(fat32_ctx_t *ctx, UINT32 clus) {
  return (UINT64)ctx->data_start +
         ((UINT64)clus - 2u) * (UINT64)ctx->sectors_per_cluster;
}

/* Convert a CHAR16 path into an 8.3 short-name buffer (11 bytes, no
 * dot, spaces padded). Only the characters the boot stage uses are
 * supported; any exotic name is rejected. */
static int fat32_short_name(const CHAR16 *path, UINT8 *out) {
  UINTN i;
  UINTN dot = 0;

  for (i = 0; i < 11; i++) {
    out[i] = ' ';
  }
  for (i = 0; i < 11 && path[i] != 0; i++) {
    if (path[i] == L'.') {
      dot = i;
      break;
    }
  }
  if (dot == 0) {
    for (i = 0; i < 11 && path[i] != 0; i++) {
      out[i] = (UINT8)(UINT16)path[i];
    }
  } else {
    for (i = 0; i < dot && i < 8; i++) {
      out[i] = (UINT8)(UINT16)path[i];
    }
    for (i = dot + 1; path[i] != 0 && (i - dot - 1) < 3; i++) {
      out[8 + (i - dot - 1)] = (UINT8)(UINT16)path[i];
    }
  }
  for (i = 0; i < 11; i++) {
    if (out[i] >= 'a' && out[i] <= 'z') {
      out[i] = (UINT8)(out[i] - 'a' + 'A');
    }
  }
  return 0;
}

/* Find a root-directory entry. `cluster_out` and `size_out` are filled
 * on success. Returns 0 when found, -1 otherwise. */
static int fat32_scan_dir(const UINT8 *target, UINT32 *cluster_out,
                          UINT64 *size_out, const UINT8 *buf,
                          UINTN entries) {
  UINTN ent;

  for (ent = 0; ent < entries; ent++) {
    const UINT8 *e = buf + ent * 32u;

    if (e[0] == 0x00u) {
      return -1;
    }
    if (e[0] == 0xe5u || (e[11] & 0x0fu) == 0x0fu) {
      continue; /* deleted or long-name slot */
    }
    if ((e[11] & 0x10u) != 0u) {
      continue; /* directory */
    }
    if (memcmp(e, target, 11) == 0) {
      UINT32 cl = (UINT32)e[26] | ((UINT32)e[27] << 8) |
                  ((UINT32)e[20] << 16) | ((UINT32)e[21] << 24);
      UINT64 sz = (UINT64)e[28] | ((UINT64)e[29] << 8) |
                  ((UINT64)e[30] << 16) | ((UINT64)e[31] << 24);

      *cluster_out = cl;
      *size_out = sz;
      return 0;
    }
  }
  return -1;
}

static int fat32_find_root(fat32_ctx_t *ctx, const CHAR16 *path,
                           UINT32 *cluster_out, UINT64 *size_out) {
  UINT8  target[11];
  UINT8 *buf = NULL;

  if (fat32_short_name(path, target) != 0) {
    return -1;
  }

  if (ctx->fat_type == 16) {
    /* FAT16 root directory is a fixed contiguous range. */
    buf = (UINT8 *)efi_alloc((UINT64)ctx->root_sectors * ctx->bytes_per_sector);
    if (buf == NULL) {
      return -1;
    }
    if (fat32_read_sectors(ctx, ctx->root_lba, ctx->root_sectors, buf) != 0) {
      efi_free(buf);
      return -1;
    }
    if (fat32_scan_dir(target, cluster_out, size_out, buf,
                       (ctx->root_sectors * ctx->bytes_per_sector) / 32u) ==
        0) {
      efi_free(buf);
      return 0;
    }
    efi_free(buf);
    return -1;
  }

  /* FAT32 root directory is a cluster chain. */
  {
    UINT32 clus = ctx->root_cluster;

    buf = (UINT8 *)efi_alloc(ctx->bytes_per_sector * ctx->sectors_per_cluster);
    if (buf == NULL) {
      return -1;
    }
    while (clus < 0x0ffffff8u) {
      UINT64 lba = fat32_cluster_lba(ctx, clus);

      if (fat32_read_sectors(ctx, lba, ctx->sectors_per_cluster, buf) != 0) {
        efi_free(buf);
        return -1;
      }
      if (fat32_scan_dir(target, cluster_out, size_out, buf,
                         (ctx->bytes_per_sector * ctx->sectors_per_cluster) /
                             32u) == 0) {
        efi_free(buf);
        return 0;
      }
      clus = fat32_get(ctx, clus) & 0x0fffffffu;
    }
    efi_free(buf);
    return -1;
  }
}

static int fat32_init(fat32_ctx_t *ctx) {
  UINT8    bpb[512];
  UINT32   total_sectors;
  UINT32   fat_sectors;
  UINT32   root_entries;
  UINT32   reserved_sectors;
  UINT32   num_fats;
  UINT32   bps;
  UINTN    i;

  memset(ctx, 0, sizeof(*ctx));
  /* The first uefi* device is not necessarily the boot volume: a second
   * raw target disk may have been enumerated before the CD's virtual
   * hard disk. Scan every device and pick the one whose first sector is
   * a plausible FAT boot sector. A disk with an accidental 0x55/0xAA
   * but nonsense fields must be skipped. */
  for (i = 0; i < (UINTN)blkdev_count(); i++) {
    blkdev_t *d = blkdev_get_at(i);
    UINT32    rs;
    UINT32    nf;
    UINT32    ts;
    UINT32    fsz32;
    UINT32    fsz16;
    UINT32    rc;

    if (d == NULL || d->sector_count == 0) {
      continue;
    }
    ctx->dev = d;
    if (fat32_read_sectors(ctx, 0, 1, bpb) != 0) {
      continue;
    }
    if (bpb[510] != 0x55u || bpb[511] != 0xaau) {
      continue;
    }
    rs = (UINT32)bpb[14] | ((UINT32)bpb[15] << 8);
    nf = (UINT32)bpb[16];
    root_entries = (UINT32)bpb[17] | ((UINT32)bpb[18] << 8);
    fsz16 = (UINT32)bpb[22] | ((UINT32)bpb[23] << 8);
    fsz32 = (UINT32)bpb[36] | ((UINT32)bpb[37] << 8) |
            ((UINT32)bpb[38] << 16) | ((UINT32)bpb[39] << 24);
    rc = (UINT32)bpb[44] | ((UINT32)bpb[45] << 8) |
         ((UINT32)bpb[46] << 16) | ((UINT32)bpb[47] << 24);
    ts = (UINT32)bpb[19] | ((UINT32)bpb[20] << 8);
    if (ts == 0) {
      ts = (UINT32)bpb[32] | ((UINT32)bpb[33] << 8) |
           ((UINT32)bpb[34] << 16) | ((UINT32)bpb[35] << 24);
    }
    if (ts == 0) {
      ts = (UINT32)bpb[72] | ((UINT32)bpb[73] << 8) |
           ((UINT32)bpb[74] << 16) | ((UINT32)bpb[75] << 24);
    }
    /* bytes-per-sector has to be a power of two the block layer can
     * express; a 4Kn volume names 4096. `ts` counts those sectors, so it
     * is compared against the device's capacity converted the same way. */
    bps = (UINT32)bpb[11] | ((UINT32)bpb[12] << 8);
    if (bps == 0u || (bps & (bps - 1u)) != 0u ||
        bps < BLKDEV_SECTOR_SIZE || bps > BLKDEV_BLOCK_MAX) {
      continue;
    }
    if (bpb[13] == 0u || rs == 0u || nf == 0u || nf > 2u || ts == 0u ||
        (UINT64)ts > d->sector_count / (UINT64)(bps / BLKDEV_SECTOR_SIZE)) {
      continue;
    }
    {
      /* FAT16: root_entries != 0 and fat_size_16 != 0.
       * FAT32: root_entries == 0 and fat_size_32 != 0 and root_cluster >= 2. */
      if (root_entries != 0 && fsz16 != 0) {
        ctx->fat_type = 16;
        break;
      }
      if (root_entries == 0 && fsz32 != 0 && rc >= 2u) {
        ctx->fat_type = 32;
        break;
      }
    }
  }
  if (ctx->dev == NULL || ctx->fat_type == 0) {
    return -1;
  }
  ctx->bytes_per_sector = (UINT32)bpb[11] | ((UINT32)bpb[12] << 8);
  ctx->sectors_per_cluster = (UINT32)bpb[13];
  reserved_sectors = (UINT32)bpb[14] | ((UINT32)bpb[15] << 8);
  num_fats = (UINT32)bpb[16];
  total_sectors = (UINT32)bpb[19] | ((UINT32)bpb[20] << 8);
  if (total_sectors == 0) {
    total_sectors = (UINT32)bpb[32] | ((UINT32)bpb[33] << 8) |
                    ((UINT32)bpb[34] << 16) | ((UINT32)bpb[35] << 24);
  }
  if (total_sectors == 0) {
    total_sectors = (UINT32)bpb[72] | ((UINT32)bpb[73] << 8) |
                    ((UINT32)bpb[74] << 16) | ((UINT32)bpb[75] << 24);
  }
  if (ctx->fat_type == 16) {
    fat_sectors = (UINT32)bpb[22] | ((UINT32)bpb[23] << 8);
    root_entries = (UINT32)bpb[17] | ((UINT32)bpb[18] << 8);
  } else {
    fat_sectors = (UINT32)bpb[36] | ((UINT32)bpb[37] << 8) |
                  ((UINT32)bpb[38] << 16) | ((UINT32)bpb[39] << 24);
    root_entries = 0;
    ctx->root_cluster = (UINT32)bpb[44] | ((UINT32)bpb[45] << 8) |
                        ((UINT32)bpb[46] << 16) | ((UINT32)bpb[47] << 24);
  }
  /* A volume formatted with 4096-byte sectors is normal on a 4Kn disk,
   * and the reader handles any power-of-two sector size the BPB can name
   * that the block layer can express. It is not 512-specific. */
  if (ctx->bytes_per_sector == 0 ||
      (ctx->bytes_per_sector & (ctx->bytes_per_sector - 1u)) != 0 ||
      ctx->bytes_per_sector < BLKDEV_SECTOR_SIZE ||
      ctx->bytes_per_sector > BLKDEV_BLOCK_MAX ||
      ctx->sectors_per_cluster == 0 || fat_sectors == 0 ||
      total_sectors == 0) {
    return -1;
  }
  ctx->sectors_per_block = ctx->bytes_per_sector / BLKDEV_SECTOR_SIZE;
  ctx->fat_start = reserved_sectors;
  ctx->fat_sectors = fat_sectors;
  ctx->fat = (UINT8 *)efi_alloc((UINT64)fat_sectors * ctx->bytes_per_sector);
  if (ctx->fat == NULL) {
    return -1;
  }
  if (fat32_read_sectors(ctx, ctx->fat_start, fat_sectors, ctx->fat) != 0) {
    efi_free(ctx->fat);
    ctx->fat = NULL;
    return -1;
  }
  if (ctx->fat_type == 16) {
    ctx->root_lba = reserved_sectors + num_fats * fat_sectors;
    ctx->root_sectors = (root_entries * 32u + ctx->bytes_per_sector - 1u) /
                        ctx->bytes_per_sector;
    ctx->data_start = ctx->root_lba + ctx->root_sectors;
  } else {
    ctx->data_start = reserved_sectors + num_fats * fat_sectors;
  }
  return 0;
}

static void fat32_close(fat32_ctx_t *ctx) {
  if (ctx->fat != NULL) {
    efi_free(ctx->fat);
    ctx->fat = NULL;
  }
}

/* Read an entire file from the FAT32 volume into an EfiLoaderData
 * buffer. Returns 0 on success. The mkfat this boot stage pairs with
 * allocates clusters sequentially, so runs of consecutive clusters are
 * read in one Block I/O call rather than one sector at a time. */
static int fat32_read_file(const CHAR16 *path, UINT8 **out_buf,
                         UINT64 *out_len) {
  fat32_ctx_t ctx;
  UINT32      clus;
  UINT64      size;
  UINT8      *buf;
  UINT64      wrote;

  if (fat32_init(&ctx) != 0) {
    return -1;
  }
  esp_report_route("built-in FAT reader", ctx.dev);
  if (fat32_find_root(&ctx, path, &clus, &size) != 0 || size == 0 ||
      size > (64u * 1024u * 1024u)) {
    fat32_close(&ctx);
    return -1;
  }
  buf = (UINT8 *)efi_alloc(size);
  if (buf == NULL) {
    fat32_close(&ctx);
    return -1;
  }
  wrote = 0;
  while (wrote < size && clus < 0x0ffffff8u) {
    UINT32 run_start = clus;
    UINT32 run_len = 1;
    UINT32 next;
    UINT64 chunk;

    /* Gather a run of consecutive clusters. UEFI Block I/O does not
     * guarantee huge transfers, so 4 KiB (8 sectors) is a conservative
     * size that any firmware should accept while still cutting the call
     * count compared to one sector at a time. */
    while (run_len < 8u) {
      next = fat32_get(&ctx, run_start + run_len - 1u) & 0x0fffffffu;
      if (next != run_start + run_len) {
        break;
      }
      run_len++;
    }

    chunk = (UINT64)run_len * ctx.bytes_per_sector;
    if (size - wrote < chunk) {
      chunk = size - wrote;
    }
    if (fat32_read_sectors(&ctx, fat32_cluster_lba(&ctx, run_start),
                           run_len, buf + wrote) != 0) {
      efi_free(buf);
      fat32_close(&ctx);
      return -1;
    }
    wrote += chunk;
    clus = fat32_get(&ctx, run_start + run_len - 1u) & 0x0fffffffu;
  }
  fat32_close(&ctx);
  *out_buf = buf;
  *out_len = size;
  esp_report_route("built-in FAT reader", ctx.dev);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Partition wrapper                                                   */
/* ------------------------------------------------------------------ */
/* NovusFS addresses its device from sector 0, but a partition starts
 * somewhere in the middle of a disk. This thin wrapper adds the offset
 * so the filesystem never has to know about partition tables. */

typedef struct {
  blkdev_t *parent;
  UINT64    base;
  UINT64    sectors;
} stage3_part_ctx_t;

static int part_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  stage3_part_ctx_t *c = (stage3_part_ctx_t *)ctx;

  if (sector + (UINT64)count > c->sectors) {
    return -1;
  }
  return blkdev_read(c->parent, c->base + sector, count, buf);
}

static int part_write(void *ctx, UINT64 sector, UINTN count,
                      const void *buf) {
  stage3_part_ctx_t *c = (stage3_part_ctx_t *)ctx;

  if (sector + (UINT64)count > c->sectors) {
    return -1;
  }
  return blkdev_write(c->parent, c->base + sector, count, buf);
}

/* ------------------------------------------------------------------ */
/* Logging helpers                                                     */
/* ------------------------------------------------------------------ */

static void say(const CHAR16 *s) {
  boot_puts((CHAR16 *)s);
}

/* Device names and the like are plain ASCII; the console speaks UTF-16.
 * Wide enough for the longest thing passed to it, which is a sentence
 * describing how the ESP is being reached. */
static void say_ascii(const char *s) {
  CHAR16 buf[64];
  UINTN  i = 0;

  if (s == NULL) {
    return;
  }
  while (s[i] != '\0' && i + 1u < (UINTN)(sizeof(buf) / sizeof(buf[0]))) {
    buf[i] = (CHAR16)s[i];
    i++;
  }
  buf[i] = 0;
  boot_puts(buf);
}

static void say_line(const CHAR16 *s) {
  boot_puts((CHAR16 *)s);
  boot_puts(L"\r\n");
}

/* Report the ESP path once, the first time either route is used, so the
 * log says how the loader is reaching its files on this machine. */
static void esp_report_route(const char *via, blkdev_t *dev) {
  static int said = 0;

  if (said != 0) {
    return;
  }
  said = 1;
  say(L"[BOOT] reading the ESP via ");
  say_ascii(via);
  if (dev != NULL) {
    say(L" on ");
    say_ascii(dev->name);
  }
  say_line(L"");
}

/* Render the first `n` bytes as upper-case hex. Truncated on purpose:
 * a full 32-byte root on every boot is noise, and the first eight bytes
 * are enough to recognise a hash across runs. */
static void hex8(const UINT8 *bytes, UINTN n) {
  static const CHAR16 digits[] = L"0123456789ABCDEF";
  CHAR16 buf[3];
  UINTN  i;

  buf[2] = 0;
  for (i = 0; i < n; i++) {
    buf[0] = digits[(bytes[i] >> 4) & 0x0fu];
    buf[1] = digits[bytes[i] & 0x0fu];
    boot_puts(buf);
  }
}

/* ------------------------------------------------------------------ */
/* Integrity verification                                              */
/* ------------------------------------------------------------------ */

/* Find the Novus system partition, mount it and verify it.
 * Returns 1 when a partition was found and checked, 0 when none exists.
 * The outcome is published into shared memory either way. */
static int syspart_verify(void) {
  shared_mem_t *shm = shared_mem_get();
  UINTN         i;

  shm->merkle_status = SHM_MERKLE_UNKNOWN;

  for (i = 0; i < blkdev_count(); i++) {
    blkdev_t   *disk = blkdev_get_at(i);
    gpt_table_t gpt;
    gpt_part_t  part;

    if (disk == NULL) {
      continue;
    }
    if (gpt_read(disk, &gpt) != 0) {
      continue; /* not a GPT disk: possibly an untouched install target */
    }
    /* A disk with a table but no NovusFS partition on it is worth one
     * line: it is the case where the system is installed and the boot
     * still falls through to live, and "nothing found" would send anyone
     * looking at the wrong disk. */
    if (gpt_find_novusfs(&gpt, &part) != 0) {
      say(L"[MRKL] ");
      say_ascii(disk->name);
      say(L": ");
      boot_put_dec(gpt.count);
      say_line(L" partitions, none holds NovusFS");
      continue;
    }
    {
      static stage3_part_ctx_t pctx;
      static blkdev_t          pdev;
      static novusfs_fs_t      fs;
      UINT8  root[NOVUSFS_MERKLE_ROOT_LEN];
      UINT32 status;

      pctx.parent = disk;
      pctx.base = part.first_sector; /* LBA converted to 512-byte sectors */
      pctx.sectors = part.sector_count;
      pdev.name = "syspart";
      pdev.sector_count = part.sector_count;
      pdev.block_size = BLKDEV_SECTOR_SIZE; /* a window over the parent */
      pdev.flags = BLKDEV_FLAG_PARTITION;
      pdev.ctx = &pctx;
      pdev.read = part_read;
      pdev.write = part_write;
      if (blkdev_register(&pdev) < 0) {
        continue;
      }

      say_line(L"[MRKL] system partition found");
      if (novusfs_mount(&fs, &pdev) != 0) {
        say_line(L"[MRKL]   mount failed: UNREADABLE");
        shm->merkle_status = SHM_MERKLE_UNREADABLE;
        novusfs_unmount(&fs);
        return 1;
      }

      status = novusfs_merkle_verify(&fs, root);
      memcpy(shm->merkle_root, root, NOVUSFS_MERKLE_ROOT_LEN);
      shm->merkle_height = fs.sb.merkle_height;
      shm->merkle_leaf_count = fs.sb.merkle_leaf_count;
      shm->merkle_status =
          (status == NOVUSFS_MERKLE_OK)          ? SHM_MERKLE_PASS
          : (status == NOVUSFS_MERKLE_MISMATCH)  ? SHM_MERKLE_MISMATCH
          : (status == NOVUSFS_MERKLE_NOT_BUILT) ? SHM_MERKLE_NOT_BUILT
                                                 : SHM_MERKLE_UNREADABLE;

      say(L"[MRKL]   root ");
      hex8(root, 8);
      say(L"... ");
      switch (shm->merkle_status) {
      case SHM_MERKLE_PASS:
        say_line(L"PASS");
        break;
      case SHM_MERKLE_MISMATCH:
        say_line(L"MISMATCH - content does not match the stored root");
        break;
      case SHM_MERKLE_NOT_BUILT:
        say_line(L"NOT BUILT - no root hash stored yet");
        break;
      default:
        say_line(L"UNREADABLE");
        break;
      }
      novusfs_unmount(&fs);
      return 1;
    }
  }

  say_line(L"[MRKL] no Novus system partition found (live boot)");
  return 0;
}

/* ------------------------------------------------------------------ */
/* Signed modules                                                      */
/* ------------------------------------------------------------------ */
/* Modules live on the ESP under NOVUS, with an 8.3 name ending in
 * .NKM. They are read and verified here rather than in the kernel for
 * the same reason everything else disk-related is: the firmware FAT
 * driver stops working at ExitBootServices. */

/* Stage a module for the kernel.
 *
 * The boot stage is the only phase that can read the ESP (the firmware's
 * FAT driver dies at ExitBootServices), but it is not the phase that can
 * load a module: mapping segments needs the PMM and the heap, and both
 * belong to the kernel stage. So the image is read into reserved pages
 * here - the same trick that carries the live root across - and the
 * kernel does the actual load from those pages. */
static void modules_load(void) {
  shared_mem_t *shm = shared_mem_get();
  UINT64        base = 0;
  UINT64        size = 0;

  shm->nkm_status = SHM_NKM_UNKNOWN;
  shm->nkm_loaded = 0;
  shm->nkm_img_base = 0;
  shm->nkm_img_size = 0;

  if (g_esp_root == NULL) {
    return;
  }
  if (esp_read_reserved(L"TEST.NKM", &base, &size) != 0) {
    say_line(L"[NKM ] no TEST.NKM on the ESP");
    return;
  }
  shm->nkm_img_base = base;
  shm->nkm_img_size = size;
  say(L"[NKM ] TEST.NKM staged: ");
  boot_put_dec(size / 1024u);
  say_line(L" KiB for the kernel stage");
}

/* M4: the NUC application, staged exactly like the module - into reserved
 * pages, published through shared memory - because the kernel cannot read
 * the ESP once boot services are gone. A medium without an application is
 * not an error: the scheduler still comes up with its idle task. */
static void app_load(void) {
  shared_mem_t *shm = shared_mem_get();
  UINT64        base = 0;
  UINT64        size = 0;

  shm->app_img_base = 0;
  shm->app_img_size = 0;
  if (g_esp_root == NULL) {
    return;
  }
  if (esp_read_reserved(L"DEMO.NUC", &base, &size) != 0) {
    say_line(L"[NUC ] no DEMO.NUC on the ESP");
    return;
  }
  shm->app_img_base = base;
  shm->app_img_size = size;
  say(L"[NUC ] DEMO.NUC staged: ");
  boot_put_dec(size);
  say_line(L" bytes for the kernel stage");
}

/* ------------------------------------------------------------------ */
/* Wallpaper package                                                   */
/* ------------------------------------------------------------------ */
/* The photos ship as one file on the ESP instead of inside the loader, so
 * the PE image stays small and a medium that does not carry the file still
 * boots. It is staged exactly like the module and the application - into
 * reserved pages, published through shared memory - because the kernel
 * cannot read the ESP once boot services are gone. A missing package is
 * not an error: the desktop then paints a flat colour. */

static void wpkg_load(void) {
  shared_mem_t *shm = shared_mem_get();
  UINT64        base = 0;
  UINT64        size = 0;

  shm->wpkg_img_base = 0;
  shm->wpkg_img_size = 0;
  if (g_esp_root == NULL) {
    return;
  }
  if (esp_read_reserved(L"WPAPERS.PKG", &base, &size) != 0) {
    say_line(L"[WPKG] no WPAPERS.PKG: flat desktop colour");
    return;
  }
  shm->wpkg_img_base = base;
  shm->wpkg_img_size = size;
  say(L"[WPKG] wallpaper package staged: ");
  boot_put_dec(size);
  say_line(L" bytes for the kernel stage");
}

/* ------------------------------------------------------------------ */
/* Installer                                                           */
/* ------------------------------------------------------------------ */
#if CONFIG_NOVUS_INSTALLER && CONFIG_NOVUS_INSTALLER_TEXT

/* A disk the installer must keep its hands off. Two cases, both cheap to
 * recognise and both exactly the disks that must not be overwritten:
 *
 *   - a superfloppy FAT volume (a boot sector and no partition table),
 *     which is what the live medium itself - and a stick of files - looks
 *     like. Booting from a hard disk and then installing "to the first
 *     writable disk" would otherwise wipe the medium we are running from;
 *   - a disk whose GPT already holds an EFI system partition, i.e. some
 *     other operating system, or the Novus installation we booted from.
 *
 * An unreadable disk is refused too: guessing here is not worth a
 * destroyed volume. */
static int install_disk_is_foreign(blkdev_t *d) {
  static UINT8 first[BLKDEV_SECTOR_SIZE];
  gpt_table_t  gpt;
  gpt_part_t   part;

  if (blkdev_read(d, 0, 1, first) != 0) {
    return 1;
  }
  if (first[510] == 0x55u && first[511] == 0xaau &&
      (memcmp(first + 54, "FAT16   ", 8) == 0 ||
       memcmp(first + 54, "FAT32   ", 8) == 0 ||
       memcmp(first + 82, "FAT32   ", 8) == 0)) {
    return 1;
  }
  if (gpt_read(d, &gpt) == 0 && gpt_find_esp(&gpt, &part) == 0) {
    return 1;
  }
  return 0;
}

/* Pick a plausible target: a whole disk (not a partition), writable, not
 * removable and not already somebody's system. Returns NULL when there is
 * none. */
/* Say why a device was not offered. "No writable whole disk found" is
 * true and useless on a machine with two disks in it: what is needed is
 * which disk, and what about it ruled it out - because the usual answer
 * is that the target is fine and the rule is what needs looking at. */
static void install_skip(blkdev_t *d, const char *why) {
  say(L"[INST]   skip ");
  say_ascii(d->name);
  say(L" (");
  boot_put_dec(d->sector_count / 2048u);
  say(L" MiB): ");
  say_ascii(why);
  say_line(L"");
}

static blkdev_t *install_pick_target(void) {
  UINTN i;

  for (i = 0; i < blkdev_count(); i++) {
    blkdev_t *d = blkdev_get_at(i);

    if (d == NULL) {
      continue;
    }
    if ((d->flags & BLKDEV_FLAG_PARTITION) != 0) {
      install_skip(d, "it is a partition, not a whole disk");
      continue;
    }
    if ((d->flags & BLKDEV_FLAG_READONLY) != 0) {
      install_skip(d, "read-only");
      continue;
    }
    if ((d->flags & BLKDEV_FLAG_REMOVABLE) != 0) {
      install_skip(d, "removable media");
      continue;
    }
    /* A device too small to hold a GPT and both partitions is not a
     * usable target however willing it looks. */
    if (d->sector_count < 4096u) {
      install_skip(d, "too small");
      continue;
    }
    if (install_disk_is_foreign(d)) {
      install_skip(d, "holds a filesystem Novus does not own");
      continue;
    }
    return d;
  }
  return NULL;
}

/* Copy `bytes` from `src` into `dev` starting at LBA `lba`. */
static int install_blob(blkdev_t *dev, UINT64 lba, const UINT8 *src,
                        UINT64 bytes) {
  UINT64 done = 0;

  while (done < bytes) {
    UINTN  chunk = 8u; /* 4 KiB per call keeps the request small */
    UINT64 remaining = (bytes - done) / BLKDEV_SECTOR_SIZE;

    if ((UINT64)chunk > remaining) {
      chunk = (UINTN)remaining;
    }
    if (chunk == 0) {
      /* Tail that does not fill a sector: pad and write one. */
      UINT8  tail[BLKDEV_SECTOR_SIZE];
      UINT64 left = bytes - done;

      if (left > BLKDEV_SECTOR_SIZE) {
        return -1;
      }
      memset(tail, 0, sizeof(tail));
      memcpy(tail, src + done, left);
      return blkdev_write(dev, lba + done / BLKDEV_SECTOR_SIZE, 1, tail);
    }
    if (blkdev_write(dev, lba + done / BLKDEV_SECTOR_SIZE, chunk,
                     src + done) != 0) {
      return -1;
    }
    done += (UINT64)chunk * BLKDEV_SECTOR_SIZE;
  }
  return 0;
}

/* Lay Novus out on a whole disk: a GPT with an EFI system partition and
 * a NovusFS system partition, the two payloads copied off the boot
 * volume, and the system partition's integrity tree built on the spot.
 *
 * Both payloads are read through the file protocol rather than through a
 * block device of the boot volume: whether one exists depends on how the
 * firmware chose to present the image it booted us from, while the file
 * protocol is what the loader was itself read through and therefore
 * always there.
 *
 * Every write goes through the firmware block I/O protocol, so this can
 * only run before ExitBootServices - which is exactly where it runs. */
static int install_run(void) {
  blkdev_t       *disk;
  UINT8          *sys_blob = NULL;
  UINT8          *esp_blob = NULL;
  UINT64          sys_bytes = 0;
  UINT64          esp_bytes = 0;
  UINT64          esp_lba;
  UINT64          esp_sectors;
  UINT64          sys_lba;
  UINT64          sys_sectors;
  gpt_part_spec_t specs[2];
  int             rc = -1;

  disk = install_pick_target();
  if (disk == NULL) {
    say_line(L"[INST] no writable whole disk found");
    return -1;
  }
  say(L"[INST] target ");
  boot_put_dec(disk->sector_count);
  say_line(L" sectors");

  if (esp_read(L"SYSTEM.IMG", &sys_blob, &sys_bytes) != 0) {
    say_line(L"[INST] SYSTEM.IMG missing: cannot build a system");
    return -1;
  }

  /* Both payloads start on a 1 MiB boundary, which every alignment
   * convention in the firmware and in NovusFS is happy with.
   *
   * The ESP image is read the same way as the system image rather than
   * streamed off the boot volume through its own block device: whether
   * the firmware even *gives* us a block device for the boot image
   * depends on how it was started - an El Torito no-emulation image is
   * handed over as a filesystem and nothing more on some firmwares. The
   * file protocol is the one interface that is guaranteed to be there,
   * since the loader itself was read through it. */
  esp_lba = 2048u;
  if (esp_read(L"ESP.IMG", &esp_blob, &esp_bytes) != 0) {
    say_line(L"[INST] ESP.IMG missing: cannot build an ESP");
    return -1;
  }
  esp_sectors = (esp_bytes + BLKDEV_SECTOR_SIZE - 1u) / BLKDEV_SECTOR_SIZE;
  /* Round both partitions up to a whole MiB. Besides keeping the
   * filesystem's block count clean, it keeps them on a device-block
   * boundary on a 4Kn disk (1 MiB is a multiple of every block size in
   * use), which a partition table has to be: gpt_write() refuses a range
   * the disk cannot address rather than write one no firmware accepts. */
  esp_sectors = (esp_sectors + 2047u) & ~(UINT64)2047u;
  sys_lba = esp_lba + esp_sectors;
  sys_sectors = (sys_bytes + BLKDEV_SECTOR_SIZE - 1u) / BLKDEV_SECTOR_SIZE;
  sys_sectors = (sys_sectors + 2047u) & ~(UINT64)2047u;

  if (esp_lba + esp_sectors + sys_sectors + 64u >= disk->sector_count) {
    say_line(L"[INST] target disk too small");
    goto done;
  }

  memset(specs, 0, sizeof(specs));
  {
    static const UINT8 esp_guid[16]     = GPT_TYPE_ESP_BYTES;
    static const UINT8 novusfs_guid[16] = GPT_TYPE_NOVUSFS_BYTES;
    UINTN i;

    memcpy(specs[0].type_guid, esp_guid, 16);
    specs[0].first_lba = esp_lba;
    specs[0].last_lba = esp_lba + esp_sectors - 1u;
    for (i = 0; i < 4; i++) {
      specs[0].name[i] = L"EFI "[i];
    }
    memcpy(specs[1].type_guid, novusfs_guid, 16);
    specs[1].first_lba = sys_lba;
    specs[1].last_lba = sys_lba + sys_sectors - 1u;
    for (i = 0; i < 6; i++) {
      specs[1].name[i] = L"NOVUS "[i];
    }
  }

  if (gpt_write(disk, specs, 2) != 0) {
    say_line(L"[INST] GPT write failed");
    goto done;
  }
  say_line(L"[INST] partition table written");

  /* Write both payloads now that the table is on disk. The ESP goes
   * first: it is what a firmware looks at when it boots the result. */
  if (install_blob(disk, esp_lba, esp_blob, esp_bytes) != 0) {
    say_line(L"[INST] ESP payload write failed");
    goto done;
  }
  if (install_blob(disk, sys_lba, sys_blob, sys_bytes) != 0) {
    say_line(L"[INST] system payload write failed");
    goto done;
  }
  say_line(L"[INST] partitions written");

  /* Build the integrity tree over the freshly written system
   * partition: an installed system must boot holding a valid root hash,
   * not discover on its first boot that it does not have one. */
  {
    static stage3_part_ctx_t pctx;
    static blkdev_t          pdev;
    static novusfs_fs_t      fs;
    UINT8 root[NOVUSFS_MERKLE_ROOT_LEN];

    pctx.parent = disk;
    pctx.base = sys_lba;
    pctx.sectors = sys_sectors;
    pdev.name = "instpart";
    pdev.sector_count = sys_sectors;
    pdev.flags = BLKDEV_FLAG_PARTITION;
    pdev.ctx = &pctx;
    pdev.read = part_read;
    pdev.write = part_write;
    if (blkdev_register(&pdev) < 0) {
      say_line(L"[INST] cannot register the new partition");
      goto done;
    }
    if (novusfs_mount(&fs, &pdev) != 0) {
      say_line(L"[INST] cannot mount the new system partition");
      goto done;
    }
    if (novusfs_merkle_build(&fs, root) != 0) {
      say_line(L"[INST] merkle build failed");
      novusfs_unmount(&fs);
      goto done;
    }
    say(L"[INST] merkle root ");
    hex8(root, 8);
    say_line(L"... written");
    novusfs_unmount(&fs);
  }

  say_line(L"[INST] install complete");
  rc = 0;

done:
  if (sys_blob != NULL) {
    efi_free(sys_blob);
  }
  if (esp_blob != NULL) {
    efi_free(esp_blob);
  }
  return rc;
}

#endif /* CONFIG_NOVUS_INSTALLER && CONFIG_NOVUS_INSTALLER_TEXT */

/* ------------------------------------------------------------------ */
/* Live menu                                                           */
/* ------------------------------------------------------------------ */
/* A text menu on the firmware console. It is not a shell: it offers
 * exactly the operations this milestone implements, and it never blocks
 * forever - an unattended boot falls through to the kernel once the
 * input goes quiet. */

static EFI_INPUT_KEY menu_wait_key(void) {
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin =
      (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)g_st->ConIn;
  EFI_INPUT_KEY key;
  UINTN         spins;

  key.ScanCode = 0;
  key.UnicodeChar = 0;
  if (conin == NULL || conin->ReadKeyStroke == NULL) {
    return key;
  }
  /* Bounded poll. WaitForEvent would be tidier, but a bounded poll
   * keeps the menu from hanging on a firmware that never signals. */
  for (spins = 0; spins < 20000u; spins++) {
    if (conin->ReadKeyStroke(conin, &key) == EFI_SUCCESS) {
      return key;
    }
    g_bs->Stall(1000u); /* 1 ms; twenty seconds at most */
  }
  return key;
}

static void menu_show(void) {
  say_line(L"");
  say_line(L"  ================================================");
  say_line(L"   Novus OS - live system");
  say_line(L"  ================================================");
  say_line(L"   1  Verify the system partition (Merkle)");
  say_line(L"   2  Load and verify signed modules (NKM)");
  say_line(L"   3  Install Novus (graphical wizard)");
  say_line(L"   4  Continue into the kernel");
  say_line(L"");
  say(L"  choice [4]: ");
}

/* The unattended path. A machine with no keyboard - or a scripted run -
 * drops INSTALL.YES in the root of the ESP and gets the installer straight
 * away instead of the menu. The marker is deliberately absent from any
 * image the build produces: the default is the menu, and skipping it only
 * skips the prompt, never the installer's own checks on the target disk.
 * The name has to be 8.3 because the built-in FAT reader does not walk
 * subdirectories. */
#define MENU_MARKER_INSTALL L"INSTALL.YES"

static int menu_marker_present(void) {
  UINT8 *buf = NULL;
  UINT64 len = 0;

  if (esp_read(MENU_MARKER_INSTALL, &buf, &len) != 0) {
    return 0;
  }
  efi_free(buf);
  return 1;
}

static void menu_run(void) {
  EFI_INPUT_KEY key;

#if CONFIG_NOVUS_INSTALLER
  if (menu_marker_present()) {
#if CONFIG_NOVUS_INSTALLER_TEXT
    say_line(L"[MENU] INSTALL.YES on the ESP: running the text installer");
    (VOID)install_run();
#else
    say_line(L"[MENU] INSTALL.YES on the ESP: booting the graphical installer");
    shared_mem_get()->boot_mode = SHM_BOOT_MODE_INSTALL;
#endif
    return;
  }
#endif

  menu_show();
  key = menu_wait_key();
  if (key.UnicodeChar == 0) {
    say_line(L"4 (no input)");
    return;
  }
  say_line(L"");

  switch (key.UnicodeChar) {
  case L'1':
    if (syspart_verify() == 0) {
      say_line(L"[MENU] nothing to verify: no system partition");
    }
    break;
  case L'2':
    modules_load();
    break;
  case L'3':
#if CONFIG_NOVUS_INSTALLER
#if CONFIG_NOVUS_INSTALLER_TEXT
    (VOID)install_run();
#else
    say_line(L"[MENU] booting the graphical installer");
    shared_mem_get()->boot_mode = SHM_BOOT_MODE_INSTALL;
#endif
#else
    say_line(L"[MENU] installer not built in");
#endif
    break;
  default:
    break;
  }
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

void stage3_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  g_image = image;
  g_st = st;
  g_bs = st->BootServices;

  /* Point NovusFS at EFI pool memory for the whole boot stage. */
  novusfs_set_allocator(efi_alloc, efi_free);
}

/* One line per device, with the two facts that decide whether the rest
 * of the boot can work at all: the block size, and whether this is the
 * device we were started from.
 *
 * This list is the answer to nearly every "it does not boot on my
 * machine" report. A machine that shows no disk here has no disk as far
 * as Novus is concerned - the firmware did not give us one, or it was
 * refused for a block size we cannot address - and no amount of work
 * further up the boot path will change that. */
static void report_block_devices(UINTN ndev) {
  UINTN i;
  int   boot = blkdev_uefi_boot_device();

  say(L"[BOOT] block devices: ");
  boot_put_dec(ndev);
  say_line(L"");

  for (i = 0; i < ndev; i++) {
    blkdev_t *d = blkdev_get_at(i);

    if (d == NULL) {
      continue;
    }
    say(L"[BOOT]   ");
    say_ascii(d->name);
    say(L"  ");
    boot_put_dec(blkdev_block_size(d));
    say(L"B blocks, ");
    boot_put_dec((UINT64)(d->sector_count / 2048u));
    say(L" MiB");
    if ((d->flags & BLKDEV_FLAG_READONLY) != 0) {
      say(L", read-only");
    }
    if ((d->flags & BLKDEV_FLAG_REMOVABLE) != 0) {
      say(L", removable");
    }
    if ((d->flags & BLKDEV_FLAG_PARTITION) != 0) {
      say(L", partition");
    }
    if (boot >= 0 && i == (UINTN)boot) {
      say(L"  <- booted from this");
    }
    say_line(L"");
  }
  if (ndev == 0u) {
    say_line(L"[BOOT]   none: the firmware offered no block device at all");
  }
}

/* Probe devices and open the ESP. This is the part the menu needs, so it
 * runs before the menu; the payload reads below are the slow part and run
 * after it, under the progress bar. Safe to call when either is missing. */
void stage3_prepare(void) {
  shared_mem_t *shm = shared_mem_get();
  UINTN         ndev;

  ndev = (UINTN)blkdev_uefi_probe(g_st, g_image);
  report_block_devices(ndev);

  if (esp_open() == 0) {
    say_line(L"[BOOT] EFI system partition opened");
  } else {
    say_line(L"[BOOT] WARN no EFI system partition: live root unavailable");
  }

  shm->boot_mode = SHM_BOOT_MODE_LIVE;
}

/* Everything that reads the medium: the integrity check, the signed
 * modules, the application, the wallpaper package, the live root and the
 * install ESP template. Each step advances the boot progress bar, because
 * this is where a live boot spends its time.
 *
 * The log still goes out on COM1; the screen belongs to the splash by now,
 * so nothing here may write the firmware console. */
void stage3_load(void) {
  nd_boot_stage(12, "校验系统");
#if CONFIG_NOVUS_MERKLE
  (VOID)syspart_verify();
#endif

#if CONFIG_NOVUS_NKM
  nd_boot_stage(26, "加载内核模块");
  modules_load();
#endif

  /* M4: stage the NUC application next to the module. */
  nd_boot_stage(38, "加载应用程序");
  app_load();

  /* The photo package: one file, same staging path as everything else. */
  nd_boot_stage(50, "加载壁纸");
  wpkg_load();

  /* Live root: adopt the NovusFS image off the ESP into reserved pages.
   * Without it the kernel has no filesystem at all, so a failure here is
   * worth saying loudly - but it is still not fatal. */
  nd_boot_stage(64, "挂载系统镜像");
  if (esp_read_reserved(L"SYSTEM.IMG", &g_live_base, &g_live_size) ==
      0) {
    say(L"[BOOT] live root: ");
    boot_put_dec(g_live_size / 1024u);
    say(L" KiB at ");
    boot_put_hex(g_live_base);
    say_line(L"");
  } else {
    say_line(L"[BOOT] WARN live root not loaded");
    g_live_base = 0;
    g_live_size = 0;
  }

  /* The installer writes a complete ESP, including the boot loader, so
   * its template has to come off the live medium while the firmware file
   * protocol still exists. Without it the graphical installer still runs
   * and says so, instead of failing halfway through a disk. */
  nd_boot_stage(78, "准备安装介质");
  if (esp_read_reserved(L"ESP.IMG", &g_esp_tpl_base, &g_esp_tpl_size) == 0) {
    say(L"[BOOT] install ESP template: ");
    boot_put_dec(g_esp_tpl_size / 1024u);
    say_line(L" KiB staged");
  } else {
    say_line(L"[BOOT] WARN install ESP template not staged");
    g_esp_tpl_base = 0;
    g_esp_tpl_size = 0;
  }
}

void stage3_menu(void) {
  menu_run();
}

/* Publish the live root range and cut every tie to the firmware. Must
 * run immediately before ExitBootServices. */
void stage3_finish(void) {
  shared_mem_t *shm = shared_mem_get();

  shm->live_root_base = g_live_base;
  shm->live_root_size = g_live_size;
  shm->esp_img_base = g_esp_tpl_base;
  shm->esp_img_size = g_esp_tpl_size;
  shm->boot_usb_port = g_boot_usb_port;
  shm->install_flags = (g_esp_tpl_base != 0 && g_esp_tpl_size != 0 &&
                        g_live_base != 0 && g_live_size != 0)
                           ? SHM_INSTALL_FLAG_PAYLOADS
                           : 0u;
  if (g_boot_is_usb) {
    shm->install_flags |= SHM_INSTALL_FLAG_BOOT_USB;
    shm->boot_usb_port = g_boot_usb_port;
    say_line(L"[BOOT] live medium is on USB");
  }

  /* Restore the kernel heap allocator: from here on AllocatePool is
   * gone, and NovusFS must never call into it. */
  novusfs_set_allocator(NULL, NULL);

  /* Detach every boot-only device so no firmware protocol pointer
   * survives into the kernel, where calling it would fault. */
  blkdev_detach_boot_only();

  if (g_esp_root != NULL) {
    g_esp_root->Close(g_esp_root);
    g_esp_root = NULL;
  }
}
