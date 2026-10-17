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
/* Modules live on the ESP under NOVUS, with an 8.3 name ending in
 * .NKM. They are read and verified here rather than in the kernel for
 * the same reason everything else disk-related is: the firmware FAT
 * driver stops working at ExitBootServices. */

static void modules_load(void) {
  shared_mem_t *shm = shared_mem_get();
  UINT8        *img = NULL;
  UINT64        len = 0;

  shm->nkm_status = SHM_NKM_UNKNOWN;
  shm->nkm_loaded = 0;

  if (g_esp_root == NULL) {
    return;
  }
  if (esp_read(L"NOVUS\\TEST.NKM", &img, &len) != 0) {
    say_line(L"[NKM ] no NOVUS\\TEST.NKM on the ESP");
    return;
  }

  {
    int rc = nkm_load(img, len);

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
      boot_put_ascii((const CHAR8 *)nkm_reject_text(rc), 64);
      say_line(L"");
    }
  }
  efi_free(img);
}

/* ------------------------------------------------------------------ */
/* Installer                                                           */
/* ------------------------------------------------------------------ */
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
    /* A device too small to hold a GPT and both partitions is not a
     * usable target however willing it looks. */
    if (d->sector_count < 4096u) {
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

/* Write a whole disk: GPT, an ESP holding the boot files, and a Novus
 * system partition whose integrity tree is built on the spot.
 *
 * Every write goes through the firmware block I/O protocol, so this can
 * only run before ExitBootServices - which is exactly where it runs. */
static int install_run(void) {
  blkdev_t       *disk;
  UINT8          *esp_blob = NULL;
  UINT8          *sys_blob = NULL;
  UINT64          esp_bytes = 0;
  UINT64          sys_bytes = 0;
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

  if (esp_read(L"NOVUS\\ESP.IMG", &esp_blob, &esp_bytes) != 0) {
    say_line(L"[INST] NOVUS\\ESP.IMG missing: cannot build an ESP");
    return -1;
  }
  if (esp_read(L"NOVUS\\SYSTEM.IMG", &sys_blob, &sys_bytes) != 0) {
    say_line(L"[INST] NOVUS\\SYSTEM.IMG missing: cannot build a system");
    efi_free(esp_blob);
    return -1;
  }

  /* Both payloads start on a 1 MiB boundary, which every alignment
   * convention in the firmware and in NovusFS is happy with. */
  esp_lba = 2048u;
  esp_sectors = (esp_bytes + BLKDEV_SECTOR_SIZE - 1u) / BLKDEV_SECTOR_SIZE;
  sys_lba = esp_lba + esp_sectors;
  sys_sectors = (sys_bytes + BLKDEV_SECTOR_SIZE - 1u) / BLKDEV_SECTOR_SIZE;
  /* Round the system partition up to a whole MiB so the filesystem's
   * block count is a clean multiple. */
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
  if (esp_blob != NULL) {
    efi_free(esp_blob);
  }
  if (sys_blob != NULL) {
    efi_free(sys_blob);
  }
  return rc;
}

#endif /* CONFIG_NOVUS_INSTALLER */

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
  say_line(L"   Novus OS M3 - live system");
  say_line(L"  ================================================");
  say_line(L"   1  Verify the system partition (Merkle)");
  say_line(L"   2  Load and verify signed modules (NKM)");
  say_line(L"   3  Install Novus to a disk");
  say_line(L"   4  Continue into the kernel");
  say_line(L"");
  say(L"  choice [4]: ");
}

static void menu_run(void) {
  EFI_INPUT_KEY key;

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
    (VOID)install_run();
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

/* Probe devices, open the ESP, verify a system partition if one exists,
 * and load the live root. Safe to call when any of those are missing. */
void stage3_prepare(void) {
  shared_mem_t *shm = shared_mem_get();
  UINTN         ndev;

  ndev = (UINTN)blkdev_uefi_probe(g_st);
  say(L"[BOOT] block devices: ");
  boot_put_dec(ndev);
  say_line(L"");

  if (esp_open() == 0) {
    say_line(L"[BOOT] EFI system partition opened");
  } else {
    say_line(L"[BOOT] WARN no EFI system partition: live root unavailable");
  }

  shm->boot_mode = SHM_BOOT_MODE_LIVE;

#if CONFIG_NOVUS_MERKLE
  (VOID)syspart_verify();
#endif

#if CONFIG_NOVUS_NKM
  modules_load();
#endif

  /* Live root: adopt the NovusFS image off the ESP into reserved pages.
   * Without it the kernel has no filesystem at all, so a failure here is
   * worth saying loudly - but it is still not fatal. */
  if (esp_read_reserved(L"NOVUS\\SYSTEM.IMG", &g_live_base, &g_live_size) ==
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
