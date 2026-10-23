/**
 * Novus OS - in-system installer (M10).
 *
 * Three inputs: the two payloads the boot stage staged (the ESP template
 * and the NovusFS system image), one whole disk, and a caller that has
 * been authorised by apps_launch(). Everything destructive is validated
 * before the first sector is written, because a half-formatted disk is a
 * worse outcome than a refusal.
 *
 * The copy runs in 128 KiB steps so the wizard's progress bar keeps
 * repainting; the GPT and the verification are single steps because each
 * is short enough not to be felt.
 */

#include "kernel.h"
#include "install.h"
#include "blkdev.h"
#include "gpt.h"
#include "mm.h"
#include "shared_mem.h"
#include "novusfs.h"

#if CONFIG_NOVUS_INSTALLER

/* 256 x 512: one DMA-friendly burst that is still shorter than a frame. */
#define INSTALL_CHUNK_SECTORS 256u
#define INSTALL_MIN_SECTORS   (64u * 1024u * 2u) /* 64 MiB */

static const UINT8 *g_esp_img;
static const UINT8 *g_sys_img;
static UINT64       g_esp_bytes;
static UINT64       g_sys_bytes;

static blkdev_t *g_target;
static UINT64    g_esp_lba;
static UINT64    g_esp_sectors;
static UINT64    g_sys_lba;
static UINT64    g_sys_sectors;

static UINT8  g_active;
static UINT8  g_table_written;
static UINT64 g_owner_pid;
static UINT64 g_off_esp;
static UINT64 g_off_sys;
static install_progress_t g_prog;

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static void copy_name(char *dst, UINT32 cap, const char *src) {
  UINT32 i;

  for (i = 0; i + 1u < cap && src[i] != '\0'; i++) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

void install_init(void) {
  shared_mem_t *shm = shared_mem_get();

  g_esp_img = NULL;
  g_sys_img = NULL;
  g_esp_bytes = 0;
  g_sys_bytes = 0;

  if (shm == NULL) {
    return;
  }
  if ((shm->install_flags & SHM_INSTALL_FLAG_PAYLOADS) == 0u) {
    kputs("[INST] no install payload staged by the boot stage\n");
    return;
  }
  g_esp_img = (const UINT8 *)phys_to_virt(shm->esp_img_base);
  g_esp_bytes = shm->esp_img_size;
  g_sys_img = (const UINT8 *)phys_to_virt(shm->live_root_base);
  g_sys_bytes = shm->live_root_size;
  if (g_esp_img == NULL || g_sys_img == NULL || g_esp_bytes == 0u ||
      g_sys_bytes == 0u) {
    g_esp_img = NULL;
    g_sys_img = NULL;
    return;
  }
  kputs("[INST] payloads ready: esp ");
  kputs_hex(g_esp_bytes / 1024u);
  kputs(" KiB, system ");
  kputs_hex(g_sys_bytes / 1024u);
  kputs(" KiB\n");
}

int install_available(void) {
  return g_esp_img != NULL && g_sys_img != NULL && g_esp_bytes != 0u &&
         g_sys_bytes != 0u;
}

void install_authorize(UINT64 pid) { g_owner_pid = pid; }

int install_owner(UINT64 pid) { return pid != 0u && pid == g_owner_pid; }

/* ------------------------------------------------------------------ */
/* Candidate disks                                                     */
/* ------------------------------------------------------------------ */

int install_count(void) { return (int)blkdev_count(); }

static UINT8 disk_reason(blkdev_t *dev) {
  gpt_table_t table;

  if (dev == NULL) {
    return INSTALL_E_ARG;
  }
  if ((dev->flags & BLKDEV_FLAG_PARTITION) != 0u) {
    return INSTALL_E_PARTITION;
  }
  if ((dev->flags & BLKDEV_FLAG_READONLY) != 0u) {
    return INSTALL_E_READONLY;
  }
  if ((dev->flags & BLKDEV_FLAG_REMOVABLE) != 0u) {
    return INSTALL_E_REMOVABLE;
  }
  if (dev->sector_count < INSTALL_MIN_SECTORS) {
    return INSTALL_E_SMALL;
  }
  /* A raw disk is a target. A table with any other layout belongs to
   * someone else and is refused. The one exception is our own layout -
   * exactly an ESP plus a NovusFS partition - so that a failed or
   * aborted install can be retried without a host-side wipe. */
  if (gpt_read(dev, &table) == 0) {
    gpt_part_t esp;
    gpt_part_t novus;

    if (table.count == 2u &&
        gpt_find_esp(&table, &esp) == 0 &&
        gpt_find_novusfs(&table, &novus) == 0) {
      return INSTALL_OK;
    }
    if (table.count > 0u) {
      return INSTALL_E_FOREIGN;
    }
  }
  return INSTALL_OK;
}

int install_disk(UINT8 index, install_disk_t *out) {
  blkdev_t *dev;

  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  dev = blkdev_get_at((UINTN)index);
  if (dev == NULL) {
    return -1;
  }
  copy_name(out->name, sizeof(out->name), dev->name);
  out->sectors = dev->sector_count;
  out->block_size = blkdev_block_size(dev);
  out->flags = (UINT8)(dev->flags & 0xFFu);
  out->reason = disk_reason(dev);
  out->eligible = (UINT8)(out->reason == INSTALL_OK ? 1u : 0u);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static UINT64 round_up_mib(UINT64 sectors) {
  return (sectors + 2047u) & ~(UINT64)2047u;
}

static void build_specs(gpt_part_spec_t specs[2]) {
  static const UINT8 esp_guid[16] = GPT_TYPE_ESP_BYTES;
  static const UINT8 novusfs_guid[16] = GPT_TYPE_NOVUSFS_BYTES;
  UINTN i;

  memset(specs, 0, sizeof(gpt_part_spec_t) * 2u);
  memcpy(specs[0].type_guid, esp_guid, 16);
  specs[0].first_lba = g_esp_lba;
  specs[0].last_lba = g_esp_lba + g_esp_sectors - 1u;
  for (i = 0; i < 4u; i++) {
    specs[0].name[i] = L"EFI "[i];
  }
  memcpy(specs[1].type_guid, novusfs_guid, 16);
  specs[1].first_lba = g_sys_lba;
  specs[1].last_lba = g_sys_lba + g_sys_sectors - 1u;
  for (i = 0; i < 6u; i++) {
    specs[1].name[i] = L"NOVUS "[i];
  }
}

int install_begin(UINT8 index) {
  blkdev_t *dev;
  UINT64    need;

  if (g_active) {
    return -1;
  }
  if (!install_available()) {
    return -1;
  }
  dev = blkdev_get_at((UINTN)index);
  if (dev == NULL || disk_reason(dev) != INSTALL_OK) {
    return -1;
  }
  /* A payload whose length is not sector aligned would make the last
   * transfer read past its reserved pages. The build writes whole-MiB
   * images, so this is a corrupt-stage check, not a normal path. */
  if ((g_esp_bytes % BLKDEV_SECTOR_SIZE) != 0u ||
      (g_sys_bytes % BLKDEV_SECTOR_SIZE) != 0u) {
    return -1;
  }

  g_esp_lba = 2048u; /* 1 MiB: firmware and GPT alignment convention */
  g_esp_sectors = round_up_mib((g_esp_bytes + BLKDEV_SECTOR_SIZE - 1u) /
                               BLKDEV_SECTOR_SIZE);
  g_sys_lba = g_esp_lba + g_esp_sectors;
  g_sys_sectors = round_up_mib((g_sys_bytes + BLKDEV_SECTOR_SIZE - 1u) /
                               BLKDEV_SECTOR_SIZE);
  need = g_sys_lba + g_sys_sectors + 64u; /* room for the backup GPT */
  if (need > dev->sector_count) {
    return -1;
  }

  g_target = dev;
  g_active = 1u;
  g_table_written = 0u;
  g_off_esp = 0u;
  g_off_sys = 0u;
  memset(&g_prog, 0, sizeof(g_prog));
  g_prog.phase = INSTALL_PHASE_TABLE;
  g_prog.total_kb = (UINT32)((g_esp_bytes + g_sys_bytes) / 1024u);
  kputs("[INST] begin on ");
  kputs(dev->name);
  kputs("\n");
  return 0;
}

/* ------------------------------------------------------------------ */
/* Steps                                                               */
/* ------------------------------------------------------------------ */

static int copy_step(const UINT8 *src, UINT64 bytes, UINT64 lba, UINT64 *off) {
  UINT64 remain = bytes - *off;
  UINT64 sectors = (remain + BLKDEV_SECTOR_SIZE - 1u) / BLKDEV_SECTOR_SIZE;
  UINT64 done;

  if (sectors > INSTALL_CHUNK_SECTORS) {
    sectors = INSTALL_CHUNK_SECTORS;
  }
  done = sectors * BLKDEV_SECTOR_SIZE;
  if (done > remain) {
    done = remain;
  }
  if (blkdev_write(g_target, lba + (*off / BLKDEV_SECTOR_SIZE),
                   (UINTN)sectors, src + *off) != 0) {
    return -1;
  }
  *off += done;
  return 0;
}

static int verify_system(void) {
  static novusfs_fs_t fs;
  blkdev_t *part;
  UINT8     root[NOVUSFS_MERKLE_ROOT_LEN];
  UINT32    rc;

  part = blkdev_partition_init("novusinst", g_target, g_sys_lba, g_sys_sectors);
  if (part == NULL) {
    return -1;
  }
  if (novusfs_mount(&fs, part) != 0) {
    (void)blkdev_release(part);
    return -1;
  }
  rc = novusfs_merkle_verify(&fs, root);
  if (rc == NOVUSFS_MERKLE_NOT_BUILT) {
    /* An image built without a root is still installable; build it now so
     * the first boot has something to verify against. */
    rc = (novusfs_merkle_build(&fs, root) == 0) ? NOVUSFS_MERKLE_OK
                                                : NOVUSFS_MERKLE_UNREADABLE;
  }
  novusfs_unmount(&fs);
  (void)blkdev_release(part);
  kputs("[INST] verify root ");
  if (rc == NOVUSFS_MERKLE_OK) {
    kputs("ok\n");
    return 0;
  }
  kputs(rc == NOVUSFS_MERKLE_MISMATCH ? "mismatch\n" : "unreadable\n");
  return -1;
}

static void fail(UINT8 err) {
  g_prog.phase = INSTALL_PHASE_FAIL;
  g_prog.err = err;
  g_active = 0u;
  kputs("[INST] failed, code ");
  kputs_hex(err);
  kputs("\n");
}

static void progress_bytes(void) {
  UINT64 done = g_off_esp + g_off_sys;
  UINT64 total = g_esp_bytes + g_sys_bytes;

  g_prog.done_kb = (UINT32)(done / 1024u);
  if (total != 0u) {
    g_prog.permille = (UINT16)(300u + (done * 660u) / total);
  }
}

int install_step(install_progress_t *out) {
  if (!g_active) {
    if (out != NULL) {
      *out = g_prog;
    }
    return (int)g_prog.phase;
  }

  switch (g_prog.phase) {
  case INSTALL_PHASE_TABLE: {
    gpt_part_spec_t specs[2];

    build_specs(specs);
    if (gpt_write(g_target, specs, 2) != 0) {
      fail(INSTALL_ERR_TABLE);
      break;
    }
    g_table_written = 1u;
    g_prog.phase = INSTALL_PHASE_ESP;
    g_prog.permille = 300u;
    kputs("[INST] partition table written\n");
    break;
  }
  case INSTALL_PHASE_ESP:
    if (copy_step(g_esp_img, g_esp_bytes, g_esp_lba, &g_off_esp) != 0) {
      fail(INSTALL_ERR_WRITE);
      break;
    }
    if (g_off_esp >= g_esp_bytes) {
      g_prog.phase = INSTALL_PHASE_SYSTEM;
      kputs("[INST] ESP written\n");
    }
    progress_bytes();
    break;

  case INSTALL_PHASE_SYSTEM:
    if (copy_step(g_sys_img, g_sys_bytes, g_sys_lba, &g_off_sys) != 0) {
      fail(INSTALL_ERR_WRITE);
      break;
    }
    if (g_off_sys >= g_sys_bytes) {
      g_prog.phase = INSTALL_PHASE_VERIFY;
      g_prog.permille = 970u;
      kputs("[INST] system image written\n");
    }
    progress_bytes();
    break;

  case INSTALL_PHASE_VERIFY:
    if (verify_system() != 0) {
      fail(INSTALL_ERR_VERIFY);
      break;
    }
    g_prog.phase = INSTALL_PHASE_DONE;
    g_prog.permille = 1000u;
    g_active = 0u;
    kputs("[INST] install complete\n");
    break;

  default:
    break;
  }

  if (out != NULL) {
    *out = g_prog;
  }
  return (int)g_prog.phase;
}

int install_progress(install_progress_t *out) {
  if (out == NULL) {
    return -1;
  }
  *out = g_prog;
  return 0;
}

int install_abort(void) {
  if (!g_active) {
    return 0;
  }
  if (g_table_written) {
    return -1; /* the disk is already being modified */
  }
  g_active = 0u;
  memset(&g_prog, 0, sizeof(g_prog));
  return 0;
}

int install_reset(void) {
  if (g_active) {
    return -1;
  }
  g_target = NULL;
  g_table_written = 0u;
  g_off_esp = 0u;
  g_off_sys = 0u;
  memset(&g_prog, 0, sizeof(g_prog));
  return 0;
}

#else /* !CONFIG_NOVUS_INSTALLER */

void install_init(void) {}
int  install_available(void) { return 0; }
int  install_count(void) { return 0; }
int  install_disk(UINT8 index, install_disk_t *out) {
  (void)index;
  (void)out;
  return -1;
}
int install_begin(UINT8 index) { (void)index; return -1; }
int install_step(install_progress_t *out) { (void)out; return -1; }
int install_progress(install_progress_t *out) { (void)out; return -1; }
int install_abort(void) { return -1; }
int install_reset(void) { return -1; }
void install_authorize(UINT64 pid) { (void)pid; }
int  install_owner(UINT64 pid) { (void)pid; return 0; }

#endif /* CONFIG_NOVUS_INSTALLER */
