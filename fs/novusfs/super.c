/**
 * Novus OS - NovusFS superblock, layout and raw block IO, M2.
 *
 * Layout (README 4.2), in 4096-byte blocks:
 *
 *   0                 superblock (primary)
 *   1                 superblock (backup, dual copy per 4.2.2)
 *   wal_start         WAL circular log          [wal_blocks]
 *   merkle_start      Merkle tree, RESERVED     [merkle_blocks]  (M3)
 *   free_bmap_start   free block bitmap         [free_bmap_blocks]
 *   inode_tbl_start   inode table               [inode_tbl_blocks]
 *   data_start        data blocks               [data_blocks]
 *
 * The Merkle region is carved out now even though M2 never writes a
 * node: sizing it from the data area means M3 can populate the tree
 * without moving a single block, which is what keeps the disk ABI
 * stable across milestones.
 *
 * The two superblock copies are written together on every flush and the
 * version counter is bumped first, so the older copy always remains
 * readable while the newer one is being written - the basis for the
 * "roll back to the previous superblock copy" rule of README 4.2.2.
 *
 * Nothing here panics: a superblock that fails magic or CRC falls back
 * to the other copy, and if both are damaged mount simply reports and
 * refuses (README 4.2.3).
 */

#include "novusfs.h"

/* Smallest image M2 will format: enough room for the metadata plus a
 * handful of data blocks. */
#define NOVUSFS_MIN_TOTAL_BLOCKS 160u

/* ------------------------------------------------------------------ */
/* Raw block IO (sector translation)                                   */
/* ------------------------------------------------------------------ */

int novusfs_read_block(novusfs_fs_t *fs, UINT64 blk, void *buf) {
  if (fs == NULL || fs->dev == NULL || buf == NULL) {
    return -1;
  }
  if (fs->sb.total_blocks != 0 && blk >= fs->sb.total_blocks) {
    return -1; /* defensive: never issue an out-of-range transfer */
  }
  return blkdev_read(fs->dev, blk * (UINT64)NOVUSFS_SECTORS_PER_BLOCK,
                     NOVUSFS_SECTORS_PER_BLOCK, buf);
}

int novusfs_write_block(novusfs_fs_t *fs, UINT64 blk, const void *buf) {
  if (fs == NULL || fs->dev == NULL || buf == NULL) {
    return -1;
  }
  if (fs->sb.total_blocks != 0 && blk >= fs->sb.total_blocks) {
    return -1;
  }
  return blkdev_write(fs->dev, blk * (UINT64)NOVUSFS_SECTORS_PER_BLOCK,
                      NOVUSFS_SECTORS_PER_BLOCK, buf);
}

/* ------------------------------------------------------------------ */
/* Scratch buffers                                                     */
/* ------------------------------------------------------------------ */

/* M3: allocator indirection. The default is the kernel heap; the boot
 * stage swaps in EFI AllocatePool before it touches the filesystem,
 * because kmalloc needs the kernel heap, which does not exist yet at
 * that point. */
static void *novusfs_default_alloc(UINT64 bytes) {
  return kmalloc(bytes);
}

static void novusfs_default_free(void *ptr) {
  kfree(ptr);
}

static novusfs_alloc_fn fs_alloc = novusfs_default_alloc;
static novusfs_free_fn  fs_free = novusfs_default_free;

void novusfs_set_allocator(novusfs_alloc_fn alloc_fn,
                           novusfs_free_fn free_fn) {
  fs_alloc = (alloc_fn != NULL) ? alloc_fn : novusfs_default_alloc;
  fs_free = (free_fn != NULL) ? free_fn : novusfs_default_free;
}

void *novusfs_alloc(UINT64 bytes) {
  return fs_alloc(bytes);
}

void novusfs_free(void *ptr) {
  fs_free(ptr);
}

static int alloc_buffers(novusfs_fs_t *fs) {
  fs->scratch = (UINT8 *)fs_alloc(NOVUSFS_BLOCK_SIZE);
  fs->blkbuf = (UINT8 *)fs_alloc(NOVUSFS_BLOCK_SIZE);
  fs->walbuf = (UINT8 *)fs_alloc(WAL_HDR_SIZE + WAL_MAX_PAYLOAD);
  fs->crcbuf = (UINT8 *)fs_alloc(WAL_CRC_PREFIX + WAL_MAX_PAYLOAD);

  if (fs->scratch == NULL || fs->blkbuf == NULL || fs->walbuf == NULL ||
      fs->crcbuf == NULL) {
    if (fs->scratch != NULL) { fs_free(fs->scratch); fs->scratch = NULL; }
    if (fs->blkbuf != NULL) { fs_free(fs->blkbuf); fs->blkbuf = NULL; }
    if (fs->walbuf != NULL) { fs_free(fs->walbuf); fs->walbuf = NULL; }
    if (fs->crcbuf != NULL) { fs_free(fs->crcbuf); fs->crcbuf = NULL; }
    return -1;
  }
  return 0;
}

static void free_buffers(novusfs_fs_t *fs) {
  if (fs->scratch != NULL) { fs_free(fs->scratch); fs->scratch = NULL; }
  if (fs->blkbuf != NULL) { fs_free(fs->blkbuf); fs->blkbuf = NULL; }
  if (fs->walbuf != NULL) { fs_free(fs->walbuf); fs->walbuf = NULL; }
  if (fs->crcbuf != NULL) { fs_free(fs->crcbuf); fs->crcbuf = NULL; }
}

/* ------------------------------------------------------------------ */
/* Superblock                                                          */
/* ------------------------------------------------------------------ */

/* CRC-32 over every byte of the structure except the trailing
 * checksum field itself. */
static UINT32 super_crc(const novusfs_super_t *sb) {
  UINT64 len = sizeof(*sb) - sizeof(sb->checksum);
  return novus_rs_crc32(0, (const UINT8 *)sb, len);
}

/* Validate one copy: magic, version, geometry and checksum. */
static int super_valid(const novusfs_super_t *sb) {
  if (sb->magic != NOVUSFS_MAGIC) {
    return 0;
  }
  if (sb->version != NOVUSFS_VERSION) {
    return 0;
  }
  if (sb->block_size != NOVUSFS_BLOCK_SIZE) {
    return 0;
  }
  if (sb->checksum != super_crc(sb)) {
    return 0;
  }
  return 1;
}

/* Derive the values that are pure functions of the geometry. */
static void super_derive(novusfs_fs_t *fs) {
  /* The data area is the suffix after the inode table; the bitmap and
   * inode-table starts are stored geometry, while its end is total_blocks.
   * WAL sizing is independent and is used for ring offsets, not sectors. */
  fs->data_start = fs->sb.inode_tbl_start + fs->sb.inode_tbl_blocks;
  fs->data_blocks = fs->sb.total_blocks - fs->data_start;
  fs->wal_bytes = fs->sb.wal_blocks * (UINT64)NOVUSFS_BLOCK_SIZE;
}

int novusfs_super_flush(novusfs_fs_t *fs) {
  if (fs == NULL || fs->dev == NULL || fs->blkbuf == NULL) {
    return -1;
  }
  fs->sb.magic = NOVUSFS_MAGIC;
  fs->sb.version = NOVUSFS_VERSION;
  fs->sb.block_size = NOVUSFS_BLOCK_SIZE;
  fs->sb.sb_version++;
  fs->sb.checksum = super_crc(&fs->sb);

  /* This is an ordered pair of block writes, not an atomic pair: a failure
   * after the primary write can leave the backup carrying older bytes.
   * Mount currently tries the primary first rather than selecting by version. */
  /* Both copies carry identical bytes; the version counter is what
   * later milestones use to tell them apart after a torn write. */
  memset(fs->blkbuf, 0, NOVUSFS_BLOCK_SIZE);
  memcpy(fs->blkbuf, &fs->sb, sizeof(fs->sb));
  if (novusfs_write_block(fs, NOVUSFS_SB_PRIMARY, fs->blkbuf) != 0) {
    return -1;
  }
  return novusfs_write_block(fs, NOVUSFS_SB_BACKUP, fs->blkbuf);
}

int novusfs_super_read(novusfs_fs_t *fs, blkdev_t *dev) {
  novusfs_super_t sb;
  UINT64 total_blocks;

  if (fs == NULL || dev == NULL) {
    return -1;
  }
  memset(fs, 0, sizeof(*fs));
  fs->dev = dev;
  total_blocks = (dev->sector_count * (UINT64)BLKDEV_SECTOR_SIZE) /
                 NOVUSFS_BLOCK_SIZE;
  if (total_blocks < NOVUSFS_MIN_TOTAL_BLOCKS) {
    kputs("[NOVS] WARN image too small for NovusFS\n");
    return -1;
  }
  if (alloc_buffers(fs) != 0) {
    kputs("[NOVS] WARN out of memory for FS buffers\n");
    return -1;
  }
  fs->sb.total_blocks = total_blocks;

  /* Primary copy. */
  if (novusfs_read_block(fs, NOVUSFS_SB_PRIMARY, fs->blkbuf) == 0) {
    memcpy(&sb, fs->blkbuf, sizeof(sb));
    if (super_valid(&sb)) {
      memcpy(&fs->sb, &sb, sizeof(sb));
      super_derive(fs);
      fs->mounted = 1;
      fs->next_txid = 1;
      return 0;
    }
    kputs("[NOVS] WARN primary superblock bad, trying backup\n");
  }

  /* Backup copy (README 4.2.3 step 1). */
  if (novusfs_read_block(fs, NOVUSFS_SB_BACKUP, fs->blkbuf) == 0) {
    memcpy(&sb, fs->blkbuf, sizeof(sb));
    if (super_valid(&sb)) {
      memcpy(&fs->sb, &sb, sizeof(sb));
      super_derive(fs);
      fs->mounted = 1;
      fs->next_txid = 1;
      kputs("[NOVS] mounted from backup superblock\n");
      return 0;
    }
  }

  kputs("[NOVS] ERROR both superblock copies unusable, mount refused\n");
  free_buffers(fs);
  memset(fs, 0, sizeof(*fs));
  return -1;
}

int novusfs_format_layout(novusfs_fs_t *fs, blkdev_t *dev) {
  UINT64 total_blocks;
  UINT64 meta_fixed;
  UINT64 merkle;
  UINT64 b;

  if (fs == NULL || dev == NULL) {
    return -1;
  }
  memset(fs, 0, sizeof(*fs));
  fs->dev = dev;

  total_blocks = (dev->sector_count * (UINT64)BLKDEV_SECTOR_SIZE) /
                 NOVUSFS_BLOCK_SIZE;
  if (total_blocks < NOVUSFS_MIN_TOTAL_BLOCKS) {
    kputs("[NOVS] WARN cannot format: image too small\n");
    return -1;
  }
  if (alloc_buffers(fs) != 0) {
    kputs("[NOVS] WARN out of memory for FS buffers\n");
    return -1;
  }
  meta_fixed = (UINT64)NOVUSFS_SB_BACKUP + 1u +
               (UINT64)NOVUSFS_WAL_BLOCKS +
               (UINT64)NOVUSFS_BMAP_BLOCKS +
               (UINT64)NOVUSFS_INODE_TBL_BLOCKS;
  /* Reserve from total_blocks, not from the data area: the tree covers
   * the bitmap and the inode table as well (see novusfs.h). */
  merkle = novusfs_merkle_reserve(total_blocks);
  if (meta_fixed + merkle >= total_blocks) {
    merkle = 0; /* no room to reserve anything: M3 will notice */
  }

  /* Regions are contiguous and each start is the prior region's end:
   * the reserved tree sits between WAL and bitmap, while the data suffix
   * begins only after the fixed-size bitmap and inode table. */
  fs->sb.magic = NOVUSFS_MAGIC;
  fs->sb.version = NOVUSFS_VERSION;
  fs->sb.block_size = NOVUSFS_BLOCK_SIZE;
  fs->sb.total_blocks = total_blocks;
  fs->sb.inode_count = NOVUSFS_MAX_INODES;
  fs->sb.inode_tbl_blocks = NOVUSFS_INODE_TBL_BLOCKS;
  fs->sb.free_bmap_blocks = NOVUSFS_BMAP_BLOCKS;
  fs->sb.wal_start = (UINT64)NOVUSFS_SB_BACKUP + 1u;
  fs->sb.wal_blocks = NOVUSFS_WAL_BLOCKS;
  fs->sb.merkle_start = fs->sb.wal_start + fs->sb.wal_blocks;
  fs->sb.merkle_blocks = merkle;
  fs->sb.free_bmap_start = fs->sb.merkle_start + merkle;
  fs->sb.inode_tbl_start = fs->sb.free_bmap_start + fs->sb.free_bmap_blocks;
  fs->sb.wal_write_ptr = 0;
  fs->sb.wal_tail_ptr = 0;
  fs->sb.root_inode = 0;   /* set by the caller once the root exists */
  fs->sb.sb_version = 0;

  super_derive(fs);
  if (fs->data_blocks < 16u) {
    kputs("[NOVS] WARN cannot format: no usable data area\n");
    free_buffers(fs);
    memset(fs, 0, sizeof(*fs));
    return -1;
  }

  /* Zero the WAL region: an empty ring must not contain stale bytes
   * that a later scan could mistake for records. */
  memset(fs->blkbuf, 0, NOVUSFS_BLOCK_SIZE);
  for (b = 0; b < fs->sb.wal_blocks; b++) {
    if (novusfs_write_block(fs, fs->sb.wal_start + b, fs->blkbuf) != 0) {
      kputs("[NOVS] WARN WAL zeroing failed\n");
      free_buffers(fs);
      memset(fs, 0, sizeof(*fs));
      return -1;
    }
  }

  /* Empty bitmap: every data block starts free. */
  for (b = 0; b < fs->sb.free_bmap_blocks; b++) {
    if (novusfs_write_block(fs, fs->sb.free_bmap_start + b, fs->blkbuf) != 0) {
      kputs("[NOVS] WARN bitmap init failed\n");
      free_buffers(fs);
      memset(fs, 0, sizeof(*fs));
      return -1;
    }
  }

  /* Empty inode table: mode == NOVUSFS_IFREE everywhere. */
  for (b = 0; b < fs->sb.inode_tbl_blocks; b++) {
    if (novusfs_write_block(fs, fs->sb.inode_tbl_start + b, fs->blkbuf) != 0) {
      kputs("[NOVS] WARN inode table init failed\n");
      free_buffers(fs);
      memset(fs, 0, sizeof(*fs));
      return -1;
    }
  }

  fs->mounted = 1;
  fs->next_txid = 1;
  return novusfs_super_flush(fs);
}

/* Detach from the device and hand the scratch buffers back to the heap.
 * The image itself is left untouched, so a following novusfs_mount()
 * sees exactly what was on disk (this is what the selftest uses to
 * prove that data survives a remount). */
void novusfs_unmount(novusfs_fs_t *fs) {
  if (fs == NULL) {
    return;
  }
  free_buffers(fs);
  memset(fs, 0, sizeof(*fs));
}

void novusfs_dump_layout(novusfs_fs_t *fs) {
  if (fs == NULL || !fs->mounted) {
    kputs("[NOVS] layout: not mounted\n");
    return;
  }
  kputs("[NOVS] layout total=");
  kputs_hex(fs->sb.total_blocks);
  kputs(" wal@");
  kputs_hex(fs->sb.wal_start);
  kputs(" merkle@");
  kputs_hex(fs->sb.merkle_start);
  kputs(" bmap@");
  kputs_hex(fs->sb.free_bmap_start);
  kputs(" inode@");
  kputs_hex(fs->sb.inode_tbl_start);
  kputs(" data@");
  kputs_hex(fs->data_start);
  kputs("\n");
}
