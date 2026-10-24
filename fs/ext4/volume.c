/**
 * Novus OS - ext4 volume: superblock and group descriptors (M12).
 *
 * The volume layout a Linux ext4 file system uses:
 *
 *   block 0            the superblock, 1024 bytes into the volume
 *   block 1            the group descriptor table
 *   block 2..          bitmaps and inode tables, per block group
 */

#include "kernel.h"
#include "blkdev.h"
#include "ext4.h"
#include "ext4_int.h"
#include "ext4_le.h"

/* One bounce buffer for block transfers and one for bitmaps. The driver
 * is single threaded and never needs two blocks at the same time. */
static UINT8 g_blk[EXT4_BLOCK_MAX];
static UINT8 g_bitmap[EXT4_BLOCK_MAX];

int ext4_blk_read(ext4_fs_t *fs, UINT64 blk, void *buf) {
  UINT64 sector = blk * (UINT64)(fs->block_size / BLKDEV_SECTOR_SIZE);

  return blkdev_read(fs->dev, sector, fs->block_size / BLKDEV_SECTOR_SIZE, buf);
}

int ext4_blk_write(ext4_fs_t *fs, UINT64 blk, const void *buf) {
  UINT64 sector = blk * (UINT64)(fs->block_size / BLKDEV_SECTOR_SIZE);

  return blkdev_write(fs->dev, sector, fs->block_size / BLKDEV_SECTOR_SIZE,
                      buf);
}

/* The bitmap buffer, shared with the allocation code. */
UINT8 *ext4_bitmap_buffer(void) { return g_bitmap; }

/* ---- superblock -------------------------------------------------------- */

int ext4_sb_load(ext4_fs_t *fs) {
  UINT8 sb[EXT4_SUPER_SIZE];

  if (blkdev_read(fs->dev, EXT4_SUPER_OFFSET / BLKDEV_SECTOR_SIZE,
                  EXT4_SUPER_SIZE / BLKDEV_SECTOR_SIZE, sb) != 0) {
    return -1;
  }
  if (ext4_rd16(sb + 0x38) != EXT4_SUPER_MAGIC) {
    return -1;
  }
  fs->feature_compat = ext4_rd32(sb + 0x5C);
  fs->feature_incompat = ext4_rd32(sb + 0x60);
  fs->feature_ro_compat = ext4_rd32(sb + 0x64);
  fs->inodes_count = ext4_rd32(sb + 0x00);
  fs->blocks_count = ext4_rd32(sb + 0x04);
  if ((fs->feature_incompat & EXT4_INCOMPAT_64BIT) != 0u) {
    fs->blocks_count |= (UINT64)ext4_rd32(sb + 0x164) << 32;
  }
  fs->free_blocks = ext4_rd32(sb + 0x0C);
  fs->free_inodes = ext4_rd32(sb + 0x10);
  fs->first_data_block = ext4_rd32(sb + 0x14);
  fs->block_shift = ext4_rd32(sb + 0x18);
  fs->block_size = 1024u << fs->block_shift;
  if (fs->block_size < EXT4_BLOCK_MIN || fs->block_size > EXT4_BLOCK_MAX) {
    return -1;
  }
  fs->blocks_per_group = ext4_rd32(sb + 0x20);
  fs->inodes_per_group = ext4_rd32(sb + 0x28);
  if (fs->blocks_per_group == 0u || fs->inodes_per_group == 0u) {
    return -1;
  }
  fs->inode_size = ext4_rd16(sb + 0x58);
  if (fs->inode_size == 0u) {
    fs->inode_size = EXT4_INODE_SIZE;
  }
  if (fs->inode_size > EXT4_BLOCK_MAX ||
      (fs->inode_size & (fs->inode_size - 1u)) != 0u) {
    return -1;
  }
  /* The descriptor is 64 bytes wide when the volume uses the 64-bit
   * feature and 32 otherwise; older volumes leave the field at zero. */
  fs->desc_size = ext4_rd16(sb + 0xFE);
  if (fs->desc_size == 0u) {
    fs->desc_size = 32u;
  }
  if (fs->desc_size != 32u && fs->desc_size != 64u) {
    return -1;
  }
  fs->journal_ino = ext4_rd32(sb + 0xF8);
  fs->root_ino = EXT4_ROOT_INO;
  fs->lost_found_ino = EXT4_LOST_FOUND_INO;
  return 0;
}

int ext4_sb_store(ext4_fs_t *fs) {
  UINT8 sb[EXT4_SUPER_SIZE];

  if (blkdev_read(fs->dev, EXT4_SUPER_OFFSET / BLKDEV_SECTOR_SIZE,
                  EXT4_SUPER_SIZE / BLKDEV_SECTOR_SIZE, sb) != 0) {
    return -1;
  }
  ext4_wr32(sb + 0x0C, fs->free_blocks & 0xFFFFFFFFu);
  ext4_wr32(sb + 0x10, fs->free_inodes);
  if (blkdev_write(fs->dev, EXT4_SUPER_OFFSET / BLKDEV_SECTOR_SIZE,
                   EXT4_SUPER_SIZE / BLKDEV_SECTOR_SIZE, sb) != 0) {
    return -1;
  }
  return 0;
}

/* ---- group descriptors ------------------------------------------------- */

UINT64 ext4_gd_table_block(const ext4_fs_t *fs) {
  return (UINT64)fs->first_data_block + 1u;
}

int ext4_gd_read(ext4_fs_t *fs, UINT32 g, ext4_group_t *out) {
  UINT64       off = (UINT64)g * fs->desc_size;
  UINT64       blk = ext4_gd_table_block(fs) + off / fs->block_size;
  UINT32       within = (UINT32)(off % fs->block_size);
  const UINT8 *p;

  if (g >= EXT4_MAX_GROUPS) {
    return -1;
  }
  if (ext4_blk_read(fs, blk, g_blk) != 0) {
    return -1;
  }
  p = g_blk + within;
  out->block_bitmap = ext4_rd32(p + 0x00);
  out->inode_bitmap = ext4_rd32(p + 0x04);
  out->inode_table = ext4_rd32(p + 0x08);
  out->free_blocks = ext4_rd16(p + 0x0C);
  out->free_inodes = ext4_rd16(p + 0x0E);
  out->used_dirs = ext4_rd16(p + 0x10);
  if (fs->desc_size == 64u) {
    out->block_bitmap |= (UINT64)ext4_rd32(p + 0x20) << 32;
    out->inode_bitmap |= (UINT64)ext4_rd32(p + 0x24) << 32;
    out->inode_table |= (UINT64)ext4_rd32(p + 0x28) << 32;
    out->free_blocks |= (UINT32)ext4_rd16(p + 0x2C) << 16;
    out->free_inodes |= (UINT32)ext4_rd16(p + 0x2E) << 16;
  }
  return 0;
}

int ext4_group_update(ext4_fs_t *fs, UINT32 g) {
  UINT64       off = (UINT64)g * fs->desc_size;
  UINT64       blk = ext4_gd_table_block(fs) + off / fs->block_size;
  UINT32       within = (UINT32)(off % fs->block_size);
  ext4_group_t *grp;
  UINT8       *p;

  if (g >= EXT4_MAX_GROUPS) {
    return -1;
  }
  grp = &fs->groups[g];
  if (ext4_blk_read(fs, blk, g_blk) != 0) {
    return -1;
  }
  p = g_blk + within;
  ext4_wr32(p + 0x00, (UINT32)(grp->block_bitmap & 0xFFFFFFFFu));
  ext4_wr32(p + 0x04, (UINT32)(grp->inode_bitmap & 0xFFFFFFFFu));
  ext4_wr32(p + 0x08, (UINT32)(grp->inode_table & 0xFFFFFFFFu));
  ext4_wr16(p + 0x0C, (UINT16)(grp->free_blocks & 0xFFFFu));
  ext4_wr16(p + 0x0E, (UINT16)(grp->free_inodes & 0xFFFFu));
  ext4_wr16(p + 0x10, (UINT16)(grp->used_dirs & 0xFFFFu));
  if (fs->desc_size == 64u) {
    ext4_wr32(p + 0x20, (UINT32)(grp->block_bitmap >> 32));
    ext4_wr32(p + 0x24, (UINT32)(grp->inode_bitmap >> 32));
    ext4_wr32(p + 0x28, (UINT32)(grp->inode_table >> 32));
    ext4_wr16(p + 0x2C, (UINT16)(grp->free_blocks >> 16));
    ext4_wr16(p + 0x2E, (UINT16)(grp->free_inodes >> 16));
  }
  return ext4_blk_write(fs, blk, g_blk);
}
