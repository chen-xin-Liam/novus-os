/**
 * Novus OS - ext4 mount and sync (M12).
 */

#include "kernel.h"
#include "blkdev.h"
#include "ext4.h"
#include "ext4_int.h"
#include "ext4_le.h"

int ext4_sb_load(ext4_fs_t *fs);
int ext4_sb_store(ext4_fs_t *fs);
int ext4_gd_read(ext4_fs_t *fs, UINT32 g, ext4_group_t *out);

int ext4_probe(blkdev_t *dev) {
  UINT8 sb[EXT4_SUPER_SIZE];

  if (dev == NULL) {
    return 0;
  }
  if (blkdev_read(dev, EXT4_SUPER_OFFSET / BLKDEV_SECTOR_SIZE,
                  EXT4_SUPER_SIZE / BLKDEV_SECTOR_SIZE, sb) != 0) {
    return 0;
  }
  return (ext4_rd16(sb + 0x38) == EXT4_SUPER_MAGIC) ? 1 : 0;
}

int ext4_mount(ext4_fs_t *fs, blkdev_t *dev) {
  UINT32 i;

  if (fs == NULL || dev == NULL) {
    return -1;
  }
  memset(fs, 0, sizeof(*fs));
  fs->dev = dev;
  if (ext4_sb_load(fs) != 0) {
    return -1;
  }
  if (fs->blocks_count <= (UINT64)fs->first_data_block) {
    return -1;
  }
  fs->group_count = (UINT32)((fs->blocks_count - fs->first_data_block +
                               fs->blocks_per_group - 1u) /
                              fs->blocks_per_group);
  if (fs->group_count == 0u || fs->group_count > EXT4_MAX_GROUPS) {
    kputs("[EXT4] too many block groups\n");
    return -1;
  }
  for (i = 0; i < fs->group_count; i++) {
    if (ext4_gd_read(fs, i, &fs->groups[i]) != 0) {
      kputs("[EXT4] group descriptor read failed\n");
      return -1;
    }
  }
  fs->mounted = 1;
  kputs("[EXT4] mounted: ");
  kputs_hex(fs->blocks_count);
  kputs(" blocks, ");
  kputs_hex(fs->inodes_count);
  kputs(" inodes, ");
  kputs_hex(fs->block_size);
  kputs(" byte blocks, ");
  kputs_hex(fs->group_count);
  kputs(" groups\n");
  if ((fs->feature_ro_compat & EXT4_RO_COMPAT_METADATA_CSUM) != 0u) {
    kputs("[EXT4] metadata checksums present, not verified yet\n");
  }
  if ((fs->feature_compat & EXT4_COMPAT_HAS_JOURNAL) != 0u) {
    kputs("[EXT4] volume has a journal, not replayed yet\n");
  }
  return 0;
}

void ext4_unmount(ext4_fs_t *fs) {
  if (fs != NULL) {
    fs->mounted = 0;
  }
}

int ext4_sync(ext4_fs_t *fs) {
  UINT32 i;

  if (fs == NULL || !fs->mounted) {
    return -1;
  }
  for (i = 0; i < fs->group_count; i++) {
    if (ext4_group_update(fs, i) != 0) {
      return -1;
    }
  }
  return ext4_sb_store(fs);
}

UINT64 ext4_inode_block(const ext4_fs_t *fs, UINT32 ino) {
  UINT32 g = (ino - 1u) / fs->inodes_per_group;
  UINT32 index = (ino - 1u) % fs->inodes_per_group;
  UINT64 off = (UINT64)index * fs->inode_size;

  if (g >= EXT4_MAX_GROUPS) {
    return 0;
  }
  return fs->groups[g].inode_table + off / fs->block_size;
}

/* ---- built-in regression test ------------------------------------------ */

/* One megabyte of ramdisk is enough to exercise every read/write path: it
 * holds one block group with room for the metadata, a handful of files and
 * a directory. */
#define EXT4_TEST_BYTES (1024u * 1024u)

typedef struct {
  const char *want;
  int         seen;
} ext4_ls_ctx_t;

static void ext4_selftest_pick(void *arg, UINT32 ino, const char *name) {
  ext4_ls_ctx_t *ctx = (ext4_ls_ctx_t *)arg;
  UINTN          i;

  (void)ino;
  if (ctx->want == NULL || name == NULL) {
    return;
  }
  for (i = 0; ctx->want[i] != '\0' && name[i] != '\0'; i++) {
    if (ctx->want[i] != name[i]) {
      return;
    }
  }
  if (ctx->want[i] == '\0' && name[i] == '\0') {
    ctx->seen = 1;
  }
}

int ext4_selftest(void) {
  blkdev_t     *dev;
  ext4_fs_t     fs;
  ext4_fs_t     mfs;
  UINT8         pattern[64];
  UINT8         back[64];
  UINT32        ino_p, ino_d, ino_t;
  UINT32        i;
  int           ok = 1;

  dev = ramdisk_init(EXT4_TEST_BYTES);
  if (dev == NULL) {
    kputs("[EXT4] selftest FAIL (ramdisk)\n");
    return -1;
  }
  if (ext4_format(&fs, dev, EXT4_TEST_BYTES) != 0) {
    kputs("[EXT4] selftest FAIL (format)\n");
    return -1;
  }
  if (ext4_mount(&mfs, dev) != 0) {
    kputs("[EXT4] selftest FAIL (mount)\n");
    return -1;
  }
  for (i = 0; i < sizeof(pattern); i++) {
    pattern[i] = (UINT8)(i * 7u + 3u);
  }

  /* Create and write a file that must survive a remount. */
  ino_p = ext4_creat(&mfs, mfs.root_ino, "persist.txt");
  if (ino_p == 0u) {
    kputs("[EXT4] selftest FAIL (creat persist)\n");
    ok = 0;
    goto done;
  }
  if (ext4_write(&mfs, ino_p, 0, pattern, sizeof(pattern)) !=
      (INT64)sizeof(pattern)) {
    kputs("[EXT4] selftest FAIL (write persist)\n");
    ok = 0;
    goto done;
  }

  /* A directory. */
  ino_d = ext4_mkdir(&mfs, mfs.root_ino, "ext4dir");
  if (ino_d == 0u) {
    kputs("[EXT4] selftest FAIL (mkdir)\n");
    ok = 0;
    goto done;
  }

  /* A file we will delete before the remount. */
  ino_t = ext4_creat(&mfs, mfs.root_ino, "temp.txt");
  if (ino_t == 0u) {
    kputs("[EXT4] selftest FAIL (creat temp)\n");
    ok = 0;
    goto done;
  }
  if (ext4_write(&mfs, ino_t, 0, pattern, sizeof(pattern)) !=
      (INT64)sizeof(pattern)) {
    kputs("[EXT4] selftest FAIL (write temp)\n");
    ok = 0;
    goto done;
  }

  /* readdir should see the persistent file. */
  {
    ext4_ls_ctx_t ctx;

    ctx.want = "persist.txt";
    ctx.seen = 0;
    if (ext4_readdir(&mfs, mfs.root_ino, ext4_selftest_pick, &ctx) != 0 ||
        ctx.seen == 0) {
      kputs("[EXT4] selftest FAIL (readdir)\n");
      ok = 0;
      goto done;
    }
  }

  /* lookup resolves each name. */
  if (ext4_lookup(&mfs, mfs.root_ino, "persist.txt") != ino_p ||
      ext4_lookup(&mfs, mfs.root_ino, "ext4dir") != ino_d ||
      ext4_lookup(&mfs, mfs.root_ino, "temp.txt") != ino_t) {
    kputs("[EXT4] selftest FAIL (lookup)\n");
    ok = 0;
    goto done;
  }

  /* Remove the temporary file and flush. */
  if (ext4_unlink(&mfs, mfs.root_ino, "temp.txt") != 0) {
    kputs("[EXT4] selftest FAIL (unlink)\n");
    ok = 0;
    goto done;
  }
  if (ext4_sync(&mfs) != 0) {
    kputs("[EXT4] selftest FAIL (sync)\n");
    ok = 0;
    goto done;
  }
  ext4_unmount(&mfs);

  /* Simulate a reboot: mount the same image again. */
  if (ext4_mount(&mfs, dev) != 0) {
    kputs("[EXT4] selftest FAIL (remount)\n");
    return -1;
  }
  if (ext4_lookup(&mfs, mfs.root_ino, "persist.txt") != ino_p) {
    kputs("[EXT4] selftest FAIL (persist lost)\n");
    ok = 0;
    goto done;
  }
  if (ext4_lookup(&mfs, mfs.root_ino, "ext4dir") != ino_d) {
    kputs("[EXT4] selftest FAIL (dir lost)\n");
    ok = 0;
    goto done;
  }
  if (ext4_lookup(&mfs, mfs.root_ino, "temp.txt") != 0u) {
    kputs("[EXT4] selftest FAIL (unlink not persisted)\n");
    ok = 0;
    goto done;
  }
  memset(back, 0, sizeof(back));
  if (ext4_read(&mfs, ino_p, 0, back, sizeof(back)) !=
      (INT64)sizeof(back) || memcmp(back, pattern, sizeof(pattern)) != 0) {
    kputs("[EXT4] selftest FAIL (data not persisted)\n");
    ok = 0;
    goto done;
  }

done:
  ext4_unmount(&mfs);
  if (ok) {
    kputs("[EXT4] selftest: PASS\n");
    return 0;
  }
  return -1;
}
