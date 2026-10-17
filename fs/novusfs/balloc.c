/**
 * Novus OS - NovusFS free block bitmap (data block allocator), M2.
 *
 * The bitmap immediately follows the reserved Merkle region and covers
 * exactly the data area: bit `i` describes absolute block
 * `data_start + i`, 1 == in use. It is manipulated through the
 * novuslib-rs bitmap primitives already used by the PMM (README 16.4),
 * so no second bit-manipulation implementation exists in the tree.
 *
 * M2 scans the bitmap linearly; the region is one block (4096 bytes ==
 * 32768 bits) which comfortably covers the 16 MiB ramdisk, and the
 * scan is a single read-modify-write per bitmap block.
 *
 * Every helper here may clobber fs->scratch; callers must keep their
 * own data in fs->blkbuf instead.
 */

#include "novusfs.h"

/* Translate an absolute block number into a bitmap bit index. */
static int bmap_bit_of(novusfs_fs_t *fs, UINT64 blk, UINT64 *bit) {
  if (fs == NULL || !fs->mounted || blk < fs->data_start) {
    return -1;
  }
  if (blk - fs->data_start >= fs->data_blocks) {
    return -1;
  }
  *bit = blk - fs->data_start;
  return 0;
}

/* Which bitmap block holds `bit`, and at which byte offset. */
static int bmap_locate(novusfs_fs_t *fs, UINT64 bit, UINT64 *blk,
                       UINT64 *off) {
  UINT64 byte_off = bit / 8u;
  UINT64 b = byte_off / NOVUSFS_BLOCK_SIZE;

  if (b >= fs->sb.free_bmap_blocks) {
    return -1;
  }
  *blk = fs->sb.free_bmap_start + b;
  *off = byte_off % NOVUSFS_BLOCK_SIZE;
  return 0;
}

int novusfs_balloc_is_used(novusfs_fs_t *fs, UINT64 blk) {
  UINT64 bit, bblk, boff;

  if (bmap_bit_of(fs, blk, &bit) != 0 ||
      bmap_locate(fs, bit, &bblk, &boff) != 0) {
    return -1;
  }
  if (novusfs_read_block(fs, bblk, fs->scratch) != 0) {
    return -1;
  }
  return (fs->scratch[boff] >> (bit & 7u)) & 1u;
}

UINT64 novusfs_balloc_alloc(novusfs_fs_t *fs) {
  UINT64 b;

  if (fs == NULL || !fs->mounted) {
    return 0;
  }
  /* Walk the bitmap block by block: one read, one linear scan, one
   * write when a free bit turns up. */
  for (b = 0; b < fs->sb.free_bmap_blocks; b++) {
    UINT64 base_bit = b * (UINT64)NOVUSFS_BLOCK_SIZE * 8u;
    UINT64 nbits = (UINT64)NOVUSFS_BLOCK_SIZE * 8u;
    INT64 idx;

    if (base_bit >= fs->data_blocks) {
      break;
    }
    if (fs->data_blocks - base_bit < nbits) {
      nbits = fs->data_blocks - base_bit;
    }
    if (novusfs_read_block(fs, fs->sb.free_bmap_start + b, fs->scratch) != 0) {
      return 0;
    }
    idx = novus_rs_bitmap_find_first_clear_run(fs->scratch, nbits, 1);
    if (idx >= 0) {
      UINT64 bit = base_bit + (UINT64)idx;
      novus_rs_bitmap_set(fs->scratch, (UINT64)idx);
      if (novusfs_write_block(fs, fs->sb.free_bmap_start + b, fs->scratch) != 0) {
        return 0;
      }
      return fs->data_start + bit;
    }
  }
  return 0; /* data area exhausted */
}

void novusfs_balloc_free(novusfs_fs_t *fs, UINT64 blk) {
  UINT64 bit, bblk, boff;

  if (bmap_bit_of(fs, blk, &bit) != 0 ||
      bmap_locate(fs, bit, &bblk, &boff) != 0) {
    return;
  }
  if (novusfs_read_block(fs, bblk, fs->scratch) != 0) {
    return;
  }
  /* Clear the bit in place; LSB-first within the byte, matching the
   * novuslib-rs bit ordering. */
  fs->scratch[boff] &= (UINT8)~(1u << (bit & 7u));
  (VOID)novusfs_write_block(fs, bblk, fs->scratch);
}

UINT64 novusfs_balloc_free_count(novusfs_fs_t *fs) {
  UINT64 free_cnt = 0;
  UINT64 bit;

  if (fs == NULL || !fs->mounted) {
    return 0;
  }
  for (bit = 0; bit < fs->data_blocks; bit++) {
    UINT64 bblk, boff;
    if (bmap_locate(fs, bit, &bblk, &boff) != 0) {
      break; /* data area larger than the bitmap: stop counting */
    }
    if (boff == 0) {
      if (novusfs_read_block(fs, bblk, fs->scratch) != 0) {
        break;
      }
    }
    if (((fs->scratch[boff] >> (bit & 7u)) & 1u) == 0u) {
      free_cnt++;
    }
  }
  return free_cnt;
}
