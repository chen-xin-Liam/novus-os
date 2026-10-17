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

/* ------------------------------------------------------------------ */
/* Allocator for filesystem buffers                                    */
/* ------------------------------------------------------------------ */
/* The boot stage has no kernel heap. NovusFS takes its allocator
 * through an explicit setter, so this is where EFI AllocatePool is
 * spliced in - and restored again in stage3_finish(). */

static void *efi_alloc(UINT64 bytes) {
  VOID *p = NULL;
  UINTN n = (UINTN)bytes;

  if (g_bs == NULL) {
    return NULL;
  }
  if (n == 0) {
    n = 1;
  }
  if (g_bs->AllocatePool(EfiLoaderData, n, &p) != EFI_SUCCESS) {
    return NULL;
  }
  return p;
}

static void efi_free(void *ptr) {
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
        return 0;
      }
    }
  }

  /* Fallback: any SimpleFileSystem instance. Still better than not
   * booting, and the log says so. */
  if (g_bs->LocateProtocol(&sfs_guid, NULL, (VOID **)&sfs) == EFI_SUCCESS &&
      sfs != NULL && sfs->OpenVolume(sfs, &g_esp_root) == EFI_SUCCESS &&
      g_esp_root != NULL) {
    return 0;
  }
  g_esp_root = NULL;
  return -1;
}

/* Read a whole file into a freshly allocated pool buffer. */
static int esp_read(const CHAR16 *path, UINT8 **out_buf, UINT64 *out_len) {
  EFI_GUID           info_guid = EFI_FILE_INFO_ID;
  UINT8              info_buf[512];
  UINTN              info_size = sizeof(info_buf);
  EFI_FILE_PROTOCOL *f = NULL;
  UINT64             size;
  UINT8             *buf;
  UINTN              got;

  if (g_esp_root == NULL) {
    return -1;
  }
  if (g_esp_root->Open(g_esp_root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ,
                       0) != EFI_SUCCESS ||
      f == NULL) {
    return -1;
  }
  if (f->GetInfo(f, &info_guid, &info_size, info_buf) != EFI_SUCCESS) {
    f->Close(f);
    return -1;
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

static void say_line(const CHAR16 *s) {
  boot_puts((CHAR16 *)s);
  boot_puts(L"\r\n");
}

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
    if (gpt_find_novusfs(&gpt, &part) != 0) {
      continue;
    }
    {
      static stage3_part_ctx_t pctx;
      static blkdev_t          pdev;
      static novusfs_fs_t      fs;
      UINT8  root[NOVUSFS_MERKLE_ROOT_LEN];
      UINT32 status;

      pctx.parent = disk;
      pctx.base = part.first_lba;
      pctx.sectors = part.sector_count;
      pdev.name = "syspart";
      pdev.sector_count = part.sector_count;
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
/* Modules live on the ESP as NOVUS\*.NKM. They are read and verified
 * here rather than in the kernel for the same reason everything else
 * disk-related is: the firmware FAT driver stops working at
 * ExitBootServices. */

static void say_ascii(const char *s) {
  CHAR16 buf[64];
  UINTN  i;

  if (s == NULL) {
    return;
  }
  for (i = 0; i < 63 && s[i] != '\0'; i++) {
    buf[i] = (CHAR16)(UINT8)s[i];
  }
  buf[i] = 0;
  boot_puts(buf);
}

static void modules_load(void) {
  shared_mem_t *shm = shared_mem_get();

  shm->nkm_status = SHM_NKM_UNKNOWN;
  shm->nkm_loaded = 0;

  if (g_esp_root == NULL) {
    return;
  }
  {
    UINT8 *img = NULL;
    UINT64 len = 0;
    int    rc;

    if (esp_read(L"NOVUS\\TEST.NKM", &img, &len) != 0) {
      say_line(L"[NKM ] no NOVUS\\TEST.NKM on the ESP");
      return;
    }
    rc = nkm_load(img, len);
    if (rc >= 0) {
      const nkm_module_t *m = nkm_get((UINTN)rc);

      shm->nkm_status = SHM_NKM_OK;
      shm->nkm_loaded = 1;
      say(L"[NKM ] TEST.NKM accepted, digest ");
      if (m != NULL) {
        hex8(m->digest, 8);
      }
      say_line(L"...");
    } else {
      shm->nkm_status = SHM_NKM_REFUSED;
      say(L"[NKM ] TEST.NKM refused: ");
      say_ascii(nkm_reject_text(rc));
      say_line(L"");
    }
    efi_free(img);
  }
}

/* ------------------------------------------------------------------ */
/* Installer                                                           */
/* ------------------------------------------------------------------ */
/* Writes a whole disk: GPT, an ESP holding the boot files, and a Novus
 * system partition whose Merkle tree is built on the spot. Every write
 * goes through the firmware block I/O protocol, so this only works
 * before ExitBootServices - which is exactly where it runs. */

#if CONFIG_NOVUS_INSTALLER

/* Pick a plausible target: a whole disk (not a partition), writable and
 * not removable. Returns NULL when there is none. */
static blkdev_t *install_pick_target(void) {
  UINTN i;

  for (i = 0; i < blkdev_count(); i++) {
    blkdev_t *d = blkdev_get_at(i);

    if (d == NULL) {
      continue;
    }
    if ((d->flags & (BLKDEV_FLAG_READONLY | BLKDEV_FLAG_REMOVABLE |
                     BLKDEV_FLAG_PARTITION)) != 0) {
      continue;
    }
    if (d->sector_count < 4096u) {
      continue; /* too small to hold anything useful */
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
    UINT64 remaining_sectors = (bytes - done) / BLKDEV_SECTOR_SIZE;
    UINTN  chunk = 8u; /* 4 KiB per call keeps the transfer bounded */

    if ((UINT64)chunk > remaining_sectors) {
      chunk = (UINTN)remaining_sectors;
    }
    if (chunk == 0) {
      /* Tail that does not fill a sector: pad one and write it. */
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
