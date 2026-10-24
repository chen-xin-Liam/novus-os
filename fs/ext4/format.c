/**
 * Novus OS - ext4 formatting (M12).
 *
 * Creates a file system that Linux can mount and Novus can read: the
 * classic layout of one superblock, one group descriptor table, and per
 * group a block bitmap, an inode bitmap and the inode table.
 */

#include "kernel.h"
#include "blkdev.h"
#include "ext4.h"
#include "ext4_int.h"
#include "ext4_le.h"

UINT64 ext4_gd_table_block(const ext4_fs_t *fs);
UINT8 *ext4_bitmap_buffer(void);

/* One block of scratch space while formatting. */
static UINT8 g_fmt[EXT4_BLOCK_MAX];

/* Write one directory block with its "." and ".." entries. */
static int write_dir_block(ext4_fs_t *fs, UINT64 blk, UINT32 self,
                           UINT32 parent) {
  UINT8 *p = g_fmt;

  memset(p, 0, fs->block_size);
  ext4_wr32(p + 0, self);
  ext4_wr16(p + 4, 12);
  p[6] = 1;
  p[7] = EXT4_FT_DIR;
  p[8] = '.';
  ext4_wr32(p + 12, parent);
  ext4_wr16(p + 16, (UINT16)(fs->block_size - 12u));
  p[18] = 2;
  p[19] = EXT4_FT_DIR;
  p[20] = '.';
  p[21] = '.';
  return ext4_blk_write(fs, blk, p);
}

/* Build one inode image. */
static void build_inode(UINT8 *out, UINT16 mode, UINT32 links, UINT64 size,
                        UINT32 blocks, UINT64 first_block, UINT32 extent_len) {
  UINT8 *eb;

  memset(out, 0, EXT4_INODE_SIZE);
  ext4_wr16(out + 0x00, mode);
  ext4_wr16(out + 0x02, 0);  /* uid */
  ext4_wr32(out + 0x04, (UINT32)(size & 0xFFFFFFFFu));
  ext4_wr16(out + 0x18, 0);  /* gid */
  ext4_wr16(out + 0x1A, (UINT16)links);
  ext4_wr32(out + 0x1C, blocks);
  ext4_wr32(out + 0x20, EXT4_INODE_FLAG_EXTENTS);
  eb = out + 0x28;
  ext4_wr16(eb + 0, EXT4_EXTENT_MAGIC);
  ext4_wr16(eb + 2, 1);          /* one extent */
  ext4_wr16(eb + 4, 4);          /* room for four */
  ext4_wr16(eb + 6, 0);          /* depth: a leaf */
  ext4_wr32(eb + 8, 0);          /* generation */
  ext4_wr32(eb + 12, 0);         /* the first logical block */
  ext4_wr16(eb + 16, (UINT16)extent_len);
  ext4_wr16(eb + 18, 0);
  ext4_wr32(eb + 20, (UINT32)first_block);
  ext4_wr16(out + 0x80, 32);     /* i_extra_isize */
  ext4_wr32(out + 0x6C, (UINT32)(size >> 32));
}

/* Put one inode into the inode table. */
static int write_inode(ext4_fs_t *fs, UINT32 ino, const UINT8 *image) {
  UINT32 index = (ino - 1u) % fs->inodes_per_group;
  UINT64 off = (UINT64)index * fs->inode_size;
  UINT64 blk = ext4_inode_block(fs, ino);
  UINT32 within = (UINT32)(off % fs->block_size);

  if (ext4_blk_read(fs, blk, g_fmt) != 0) {
    return -1;
  }
  memcpy(g_fmt + within, image, fs->inode_size);
  return ext4_blk_write(fs, blk, g_fmt);
}

int ext4_format(ext4_fs_t *fs, blkdev_t *dev, UINT64 bytes) {
  UINT8 sb[EXT4_SUPER_SIZE];
  UINT8 inode[EXT4_INODE_SIZE];
  UINT8 *bitmap;
  UINT64 blocks;
  UINT32 groups;
  UINT32 g;
  UINT32 itb;
  UINT64 root_block;
  UINT64 lost_block;
  UINT64 used_blocks;

  if (fs == NULL || dev == NULL) {
    return -1;
  }
  memset(fs, 0, sizeof(*fs));
  fs->dev = dev;
  fs->block_size = EXT4_BLOCK_MAX;
  fs->block_shift = 2; /* 1024 << 2 = 4096 bytes */
  blocks = bytes / fs->block_size;
  if (blocks < 64u) {
    kputs("[EXT4] volume too small\n");
    return -1;
  }
  fs->blocks_count = blocks;
  fs->blocks_per_group = 32768u;
  fs->inodes_per_group = 1024u;
  groups = (UINT32)((blocks + fs->blocks_per_group - 1u) /
                    fs->blocks_per_group);
  if (groups > EXT4_MAX_GROUPS) {
    groups = EXT4_MAX_GROUPS;
  }
  fs->group_count = groups;
  fs->inodes_count = groups * fs->inodes_per_group;
  fs->first_data_block = 0;
  fs->inode_size = EXT4_INODE_SIZE;
  fs->desc_size = 64u; /* the 64-bit feature uses the wide descriptor */
  fs->feature_compat = 0;
  fs->feature_incompat = EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_EXTENTS |
                         EXT4_INCOMPAT_64BIT;
  fs->feature_ro_compat = EXT4_RO_COMPAT_LARGE_FILE |
                          EXT4_RO_COMPAT_HUGE_FILE |
                          EXT4_RO_COMPAT_DIR_NLINK |
                          EXT4_RO_COMPAT_EXTRA_ISIZE;
  fs->root_ino = EXT4_ROOT_INO;
  fs->lost_found_ino = EXT4_LOST_FOUND_INO;
  itb = (fs->inodes_per_group * fs->inode_size + fs->block_size - 1u) /
        fs->block_size;

  for (g = 0; g < groups; g++) {
    UINT64 base = (UINT64)g * fs->blocks_per_group;

    if (g == 0u) {
      fs->groups[g].block_bitmap = base + 3u;
      fs->groups[g].inode_bitmap = base + 4u;
      fs->groups[g].inode_table = base + 5u;
    } else {
      fs->groups[g].block_bitmap = base;
      fs->groups[g].inode_bitmap = base + 1u;
      fs->groups[g].inode_table = base + 2u;
    }
  }
  /* The root directory and lost+found take the two blocks right after the
   * first group's inode table. */
  root_block = fs->groups[0].inode_table + itb;
  lost_block = root_block + 1u;
  used_blocks = 5u + itb + 2u;
  fs->free_blocks = (UINT32)(blocks - used_blocks);
  fs->free_inodes = fs->inodes_count - EXT4_FIRST_INO;

  memset(sb, 0, sizeof(sb));
  ext4_wr32(sb + 0x00, fs->inodes_count);
  ext4_wr32(sb + 0x04, (UINT32)(fs->blocks_count & 0xFFFFFFFFu));
  ext4_wr32(sb + 0x08, (UINT32)(fs->blocks_count & 0xFFFFFFFFu));
  ext4_wr32(sb + 0x0C, fs->free_blocks);
  ext4_wr32(sb + 0x10, fs->free_inodes);
  ext4_wr32(sb + 0x14, fs->first_data_block);
  ext4_wr32(sb + 0x18, fs->block_shift);
  ext4_wr32(sb + 0x20, fs->blocks_per_group);
  ext4_wr32(sb + 0x28, fs->inodes_per_group);
  ext4_wr16(sb + 0x38, EXT4_SUPER_MAGIC);
  ext4_wr16(sb + 0x3A, 1); /* clean */
  ext4_wr16(sb + 0x3C, 1); /* errors: continue */
  ext4_wr32(sb + 0x4C, 1); /* revision 1 */
  ext4_wr32(sb + 0x54, EXT4_FIRST_INO);
  ext4_wr16(sb + 0x58, (UINT16)fs->inode_size);
  ext4_wr32(sb + 0x5C, fs->feature_compat);
  ext4_wr32(sb + 0x60, fs->feature_incompat);
  ext4_wr32(sb + 0x64, fs->feature_ro_compat);
  memcpy(sb + 0x68, "novus-ext4-0001", 16);
  memcpy(sb + 0x78, "novus", 16);
  ext4_wr32(sb + 0x164, (UINT32)(fs->blocks_count >> 32));
  ext4_wr32(sb + 0x168, (UINT32)(fs->blocks_count >> 32));
  ext4_wr16(sb + 0x170, 32); /* s_min_extra_isize */
  ext4_wr16(sb + 0x172, 32); /* s_want_extra_isize */
  ext4_wr16(sb + 0xFE, (UINT16)fs->desc_size);
  if (blkdev_write(dev, EXT4_SUPER_OFFSET / BLKDEV_SECTOR_SIZE,
                   EXT4_SUPER_SIZE / BLKDEV_SECTOR_SIZE, sb) != 0) {
    return -1;
  }

  /* The group descriptors. */
  memset(g_fmt, 0, fs->block_size);
  for (g = 0; g < groups; g++) {
    UINT8 *p = g_fmt + g * fs->desc_size;

    ext4_wr32(p + 0x00, (UINT32)fs->groups[g].block_bitmap);
    ext4_wr32(p + 0x04, (UINT32)fs->groups[g].inode_bitmap);
    ext4_wr32(p + 0x08, (UINT32)fs->groups[g].inode_table);
  }
  if (ext4_blk_write(fs, ext4_gd_table_block(fs), g_fmt) != 0) {
    return -1;
  }

  /* The bitmaps: metadata and reserved inodes are marked used. */
  bitmap = ext4_bitmap_buffer();
  for (g = 0; g < groups; g++) {
    UINT32 meta = (g == 0u) ? 5u + itb + 2u : 2u + itb;
    UINT32 i;

    memset(bitmap, 0, fs->block_size);
    for (i = 0; i < meta; i++) {
      bitmap[i / 8u] |= (UINT8)(1u << (i % 8u));
    }
    if (ext4_blk_write(fs, fs->groups[g].block_bitmap, bitmap) != 0) {
      return -1;
    }
    memset(bitmap, 0, fs->block_size);
    for (i = 0; i < EXT4_FIRST_INO; i++) {
      bitmap[i / 8u] |= (UINT8)(1u << (i % 8u));
    }
    if (ext4_blk_write(fs, fs->groups[g].inode_bitmap, bitmap) != 0) {
      return -1;
    }
    fs->groups[g].free_blocks = fs->blocks_per_group - meta;
    fs->groups[g].free_inodes = fs->inodes_per_group - EXT4_FIRST_INO;
  }

  /* The inode table of the first group is zeroed, then the three
   * reserved inodes are written into it. */
  memset(g_fmt, 0, fs->block_size);
  if (ext4_blk_write(fs, fs->groups[0].inode_table, g_fmt) != 0) {
    return -1;
  }
  build_inode(inode, 0, 0, 0, 0, 0, 0);
  if (write_inode(fs, EXT4_BAD_BLOCKS_INO, inode) != 0) {
    return -1;
  }
  build_inode(inode, 0x41EDu, 2, fs->block_size, fs->block_size / 512u,
              root_block, 1);
  if (write_inode(fs, EXT4_ROOT_INO, inode) != 0) {
    return -1;
  }
  build_inode(inode, 0x41EDu, 2, fs->block_size, fs->block_size / 512u,
              lost_block, 1);
  if (write_inode(fs, EXT4_LOST_FOUND_INO, inode) != 0) {
    return -1;
  }
  if (write_dir_block(fs, root_block, EXT4_ROOT_INO, EXT4_ROOT_INO) != 0) {
    return -1;
  }
  if (write_dir_block(fs, lost_block, EXT4_LOST_FOUND_INO, EXT4_ROOT_INO) != 0) {
    return -1;
  }

  /* The counters in the descriptors and the superblock. */
  for (g = 0; g < groups; g++) {
    if (ext4_group_update(fs, g) != 0) {
      return -1;
    }
  }
  fs->mounted = 1;
  kputs("[EXT4] formatted: ");
  kputs_hex(fs->blocks_count);
  kputs(" blocks, ");
  kputs_hex(fs->inodes_count);
  kputs(" inodes, ");
  kputs_hex(fs->group_count);
  kputs(" groups\n");
  return 0;
}
