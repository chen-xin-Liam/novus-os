/**
 * Novus OS - ext4 allocation (M12).
 *
 * The free space of a block group is described by two bitmaps: one bit
 * per block and one bit per inode. The driver keeps the free counters in
 * the group descriptors and the superblock in step with them.
 */

#include "kernel.h"
#include "blkdev.h"
#include "ext4.h"
#include "ext4_int.h"
#include "ext4_le.h"

int ext4_sb_store(ext4_fs_t *fs);
UINT8 *ext4_bitmap_buffer(void);

/* A flex group shares one pair of bitmaps between eight groups, and the
 * shared bitmaps live in the first group of the flex group. */
static UINT32 flex_first_group(const ext4_fs_t *fs, UINT32 g) {
  if ((fs->feature_incompat & EXT4_INCOMPAT_FLEX_BG) != 0u) {
    return (g / 8u) * 8u;
  }
  return g;
}

static UINT64 flex_first_block(const ext4_fs_t *fs, UINT32 g) {
  UINT32 flex = flex_first_group(fs, g);

  return (UINT64)flex * fs->blocks_per_group + fs->first_data_block;
}

int ext4_alloc_block(ext4_fs_t *fs, UINT64 *out) {
  UINT8 *bitmap = ext4_bitmap_buffer();
  UINT32 g;

  for (g = 0; g < fs->group_count; g++) {
    UINT32 flex = flex_first_group(fs, g);
    UINT64 base = flex_first_block(fs, g);
    UINT64 bitmap_blk = fs->groups[flex].block_bitmap;
    UINT32 i;

    if (fs->groups[g].free_blocks == 0u) {
      continue;
    }
    if (ext4_blk_read(fs, bitmap_blk, bitmap) != 0) {
      continue;
    }
    for (i = 0; i < fs->block_size * 8u; i++) {
      UINT64 blk;

      if ((bitmap[i / 8u] & (UINT8)(1u << (i % 8u))) != 0u) {
        continue; /* already used */
      }
      blk = base + i;
      if (blk >= fs->blocks_count) {
        break;
      }
      bitmap[i / 8u] |= (UINT8)(1u << (i % 8u));
      if (ext4_blk_write(fs, bitmap_blk, bitmap) != 0) {
        return -1;
      }
      if (fs->groups[g].free_blocks > 0u) {
        fs->groups[g].free_blocks--;
      }
      if (fs->free_blocks > 0u) {
        fs->free_blocks--;
      }
      (void)ext4_group_update(fs, g);
      (void)ext4_sb_store(fs);
      *out = blk;
      return 0;
    }
  }
  return -1;
}

int ext4_free_block(ext4_fs_t *fs, UINT64 blk) {
  UINT8 *bitmap = ext4_bitmap_buffer();
  UINT32 g;
  UINT32 flex;
  UINT64 base;
  UINT32 i;

  if (blk < fs->first_data_block) {
    return -1;
  }
  g = (UINT32)((blk - fs->first_data_block) / fs->blocks_per_group);
  if (g >= fs->group_count) {
    return -1;
  }
  flex = flex_first_group(fs, g);
  base = flex_first_block(fs, g);
  if (blk < base) {
    return -1;
  }
  i = (UINT32)(blk - base);
  if (i >= fs->block_size * 8u) {
    return -1;
  }
  if (ext4_blk_read(fs, fs->groups[flex].block_bitmap, bitmap) != 0) {
    return -1;
  }
  bitmap[i / 8u] &= (UINT8)~(1u << (i % 8u));
  if (ext4_blk_write(fs, fs->groups[flex].block_bitmap, bitmap) != 0) {
    return -1;
  }
  fs->groups[g].free_blocks++;
  fs->free_blocks++;
  (void)ext4_group_update(fs, g);
  (void)ext4_sb_store(fs);
  return 0;
}

int ext4_alloc_inode(ext4_fs_t *fs, UINT32 *out) {
  UINT8 *bitmap = ext4_bitmap_buffer();
  UINT32 g;

  for (g = 0; g < fs->group_count; g++) {
    UINT32 flex = flex_first_group(fs, g);
    UINT64 bitmap_blk = fs->groups[flex].inode_bitmap;
    UINT64 first_ino = (UINT64)flex * fs->inodes_per_group + 1u;
    UINT32 i;

    if (fs->groups[g].free_inodes == 0u) {
      continue;
    }
    if (ext4_blk_read(fs, bitmap_blk, bitmap) != 0) {
      continue;
    }
    for (i = 0; i < fs->block_size * 8u; i++) {
      UINT32 ino;

      if ((bitmap[i / 8u] & (UINT8)(1u << (i % 8u))) != 0u) {
        continue;
      }
      ino = (UINT32)(first_ino + i);
      if (ino == 0u || ino > fs->inodes_count) {
        break;
      }
      bitmap[i / 8u] |= (UINT8)(1u << (i % 8u));
      if (ext4_blk_write(fs, bitmap_blk, bitmap) != 0) {
        return -1;
      }
      if (fs->groups[g].free_inodes > 0u) {
        fs->groups[g].free_inodes--;
      }
      if (fs->free_inodes > 0u) {
        fs->free_inodes--;
      }
      (void)ext4_group_update(fs, g);
      (void)ext4_sb_store(fs);
      *out = ino;
      return 0;
    }
  }
  return -1;
}

int ext4_free_inode(ext4_fs_t *fs, UINT32 ino) {
  UINT8 *bitmap = ext4_bitmap_buffer();
  UINT32 g;
  UINT32 flex;
  UINT64 first_ino;
  UINT32 i;

  if (ino == 0u) {
    return -1;
  }
  g = (ino - 1u) / fs->inodes_per_group;
  if (g >= fs->group_count) {
    return -1;
  }
  flex = flex_first_group(fs, g);
  first_ino = (UINT64)flex * fs->inodes_per_group + 1u;
  if (ino < first_ino) {
    return -1;
  }
  i = ino - (UINT32)first_ino;
  if (i >= fs->block_size * 8u) {
    return -1;
  }
  if (ext4_blk_read(fs, fs->groups[flex].inode_bitmap, bitmap) != 0) {
    return -1;
  }
  bitmap[i / 8u] &= (UINT8)~(1u << (i % 8u));
  if (ext4_blk_write(fs, fs->groups[flex].inode_bitmap, bitmap) != 0) {
    return -1;
  }
  fs->groups[g].free_inodes++;
  fs->free_inodes++;
  (void)ext4_group_update(fs, g);
  (void)ext4_sb_store(fs);
  return 0;
}
