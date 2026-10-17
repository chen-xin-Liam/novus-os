/**
 * Novus OS - NovusFS Merkle integrity tree, M3 (README 4.2.2).
 *
 * What is protected: every block from free_bmap_start to the end of the
 * image - the free-block bitmap, the inode table and the data area. The
 * WAL is excluded because it is replayed before verification runs, and
 * the Merkle region is excluded because a tree cannot cover the blocks
 * that store it. The superblock is protected by its own CRC-32 plus the
 * dual-copy rule (4.2.2), not by this tree.
 *
 * Tree shape (frozen):
 *   - one 64-byte SHA-256 node per protected block, in ascending block
 *     order, level 0;
 *   - parent = SHA-256(child_left || child_right), 128 bytes in;
 *   - a level with an odd node count is padded by duplicating its last
 *     node, and the duplicate is STORED, so the shape is a pure
 *     function of the leaf count and build and verify cannot drift;
 *   - levels = ceil(log2(leaf_count)) + 1, level 0 being the leaves and
 *     the last level holding the single root node.
 *
 * Node placement is the frozen rule of 4.2.2: node idx lives in block
 * merkle_start + idx / 64 at byte offset (idx % 64) * 64.
 *
 * Verification is implemented as a rebuild plus a comparison rather
 * than as a separate traversal. That is deliberate: a second traversal
 * would have to agree with the builder on the padding rule forever, and
 * any disagreement would show up only as a false alarm on a good disk.
 * The rebuild writes only the Merkle region, which is derived data, so
 * verifying never modifies anything that is protected.
 *
 * Nothing here panics. A tree that does not fit, a device that fails or
 * a root that mismatches is reported and left to the caller (README 17
 * and 18: the A core stops the boot, the filesystem itself does not).
 */

#include "novusfs.h"

/* A level count is halved on every step, so 32 levels covers 2^31
 * leaves - far more than any image this filesystem can address - while
 * keeping the level tables off the stack budget. */
#define MERKLE_MAX_LEVELS 32u

/* ------------------------------------------------------------------ */
/* Shape                                                               */
/* ------------------------------------------------------------------ */

UINT32 novusfs_merkle_height(UINT64 leaf_count) {
  UINT64 n = (leaf_count == 0) ? 1u : leaf_count;
  UINT32 levels = 1u;

  /* ceil(log2(n)) halvings to reach 1, plus the leaf level itself. */
  while (n > 1u) {
    n = (n + 1u) / 2u;
    levels++;
  }
  return levels;
}

/* Fill `counts` with the number of STORED nodes per level (already
 * padded to even where the level above needs a partner) and return the
 * number of levels. `counts` must hold MERKLE_MAX_LEVELS entries. */
static UINT32 merkle_levels(UINT64 leaf_count, UINT64 *counts) {
  UINT32 levels = 1u;
  UINT64 c = leaf_count;

  counts[0] = c;
  while (counts[levels - 1u] > 1u) {
    c = counts[levels - 1u];
    if ((c & 1u) != 0u) {
      c++; /* duplicate the last node so the level can be paired up */
    }
    counts[levels - 1u] = c;
    counts[levels] = c / 2u;
    levels++;
    if (levels >= MERKLE_MAX_LEVELS) {
      break; /* unreachable for any image this FS can address */
    }
  }
  return levels;
}

UINT64 novusfs_merkle_reserve(UINT64 total_blocks) {
  UINT64 fixed;
  UINT64 leaf;
  UINT64 nodes;

  /* Everything that precedes the Merkle region: both superblock copies
   * and the WAL. */
  fixed = (UINT64)NOVUSFS_SB_BACKUP + 1u + (UINT64)NOVUSFS_WAL_BLOCKS;
  if (total_blocks <= fixed) {
    return 0;
  }
  /* Size as if the region were absent. That over-estimates the leaf
   * count - the real one is smaller by merkle_blocks - and the estimate
   * only grows with the leaf count, so the reservation is sufficient.
   * The MERKLE_MAX_LEVELS slack covers the per-level odd padding. */
  leaf = total_blocks - fixed;
  nodes = 2u * leaf + (UINT64)MERKLE_MAX_LEVELS + 8u;
  return (nodes + MERKLE_NODES_PER_BLOCK - 1u) / MERKLE_NODES_PER_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Node access, one cached block at a time                             */
/* ------------------------------------------------------------------ */
/* Reading a node always pulls a whole 4096-byte block, so sequential
 * access over a level costs one block read per 64 nodes instead of one
 * per node. */

typedef struct {
  novusfs_fs_t *fs;
  UINT8        *buf;
  UINT64        block;  /* block currently held in buf */
  UINTN         valid;  /* 1 == buf holds `block` */
  UINTN         dirty;  /* 1 == buf must be written back */
} merkle_cache_t;

static int cache_sync(merkle_cache_t *c) {
  if (c->valid && c->dirty) {
    if (novusfs_write_block(c->fs, c->block, c->buf) != 0) {
      return -1;
    }
    c->dirty = 0;
  }
  return 0;
}

static int cache_seek(merkle_cache_t *c, UINT64 node_idx) {
  UINT64 blk = c->fs->sb.merkle_start +
               node_idx / (UINT64)MERKLE_NODES_PER_BLOCK;

  if (c->valid && c->block == blk) {
    return 0;
  }
  if (cache_sync(c) != 0) {
    return -1;
  }
  if (novusfs_read_block(c->fs, blk, c->buf) != 0) {
    c->valid = 0;
    return -1;
  }
  c->block = blk;
  c->valid = 1;
  c->dirty = 0;
  return 0;
}

static UINTN node_off(UINT64 node_idx) {
  return (UINTN)((node_idx % (UINT64)MERKLE_NODES_PER_BLOCK) *
                 (UINT64)MERKLE_NODE_SIZE);
}

static int node_read(merkle_cache_t *c, UINT64 idx, UINT8 *out) {
  if (cache_seek(c, idx) != 0) {
    return -1;
  }
  memcpy(out, c->buf + node_off(idx), MERKLE_NODE_SIZE);
  return 0;
}

static int node_write(merkle_cache_t *c, UINT64 idx, const UINT8 *in) {
  if (cache_seek(c, idx) != 0) {
    return -1;
  }
  memcpy(c->buf + node_off(idx), in, MERKLE_NODE_SIZE);
  c->dirty = 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Build / rebuild                                                     */
/* ------------------------------------------------------------------ */

/* Walk the protected region, write the tree into the Merkle region and
 * return the root in `root`. When `persist` is 0 the superblock is left
 * untouched, which is what verification needs. */
static int merkle_rebuild(novusfs_fs_t *fs, UINT8 *root, int persist) {
  UINT64 counts[MERKLE_MAX_LEVELS];
  UINT64 base[MERKLE_MAX_LEVELS];
  UINT64 leaf_count;
  UINT64 total_nodes = 0;
  UINT64 capacity;
  UINT32 levels;
  UINT32 lvl;
  UINT64 i;
  merkle_cache_t src;
  merkle_cache_t dst;
  UINT8 *databuf = NULL;
  UINT8  pair[2u * MERKLE_NODE_SIZE];
  UINT8  node[MERKLE_NODE_SIZE];
  int    rc = -1;

  if (fs == NULL || !fs->mounted || fs->sb.merkle_blocks == 0) {
    return -1;
  }
  if (fs->sb.total_blocks <= fs->sb.free_bmap_start) {
    return -1;
  }
  leaf_count = fs->sb.total_blocks - fs->sb.free_bmap_start;
  if (leaf_count == 0) {
    return -1;
  }

  levels = merkle_levels(leaf_count, counts);
  base[0] = 0;
  total_nodes = counts[0];
  for (lvl = 1; lvl < levels; lvl++) {
    base[lvl] = base[lvl - 1u] + counts[lvl - 1u];
    total_nodes += counts[lvl];
  }
  capacity = fs->sb.merkle_blocks * (UINT64)MERKLE_NODES_PER_BLOCK;
  if (total_nodes > capacity) {
    kputs("[MRKL] WARN region too small: need ");
    kputs_hex(total_nodes);
    kputs(" nodes, have ");
    kputs_hex(capacity);
    kputs("\n");
    return -1;
  }

  databuf = (UINT8 *)novusfs_alloc(NOVUSFS_BLOCK_SIZE);
  src.buf = (UINT8 *)novusfs_alloc(NOVUSFS_BLOCK_SIZE);
  dst.buf = (UINT8 *)novusfs_alloc(NOVUSFS_BLOCK_SIZE);
  if (databuf == NULL || src.buf == NULL || dst.buf == NULL) {
    kputs("[MRKL] WARN out of memory for tree buffers\n");
    goto done;
  }
  src.fs = fs; src.block = 0; src.valid = 0; src.dirty = 0;
  dst.fs = fs; dst.block = 0; dst.valid = 0; dst.dirty = 0;

  /* Level 0: one hash per protected block. */
  for (i = 0; i < leaf_count; i++) {
    if (novusfs_read_block(fs, fs->sb.free_bmap_start + i, databuf) != 0) {
      kputs("[MRKL] WARN read failed at block ");
      kputs_hex(fs->sb.free_bmap_start + i);
      kputs("\n");
      goto done;
    }
    novus_rs_sha256(databuf, NOVUSFS_BLOCK_SIZE, node);
    if (node_write(&dst, base[0] + i, node) != 0) {
      goto done;
    }
  }
  /* Store the padding duplicate, if the leaf level was odd. */
  if (counts[0] > leaf_count) {
    if (node_read(&dst, base[0] + leaf_count - 1u, node) != 0) {
      goto done;
    }
    if (node_write(&dst, base[0] + leaf_count, node) != 0) {
      goto done;
    }
  }

  /* Higher levels: hash each pair of children. */
  for (lvl = 1; lvl < levels; lvl++) {
    UINT64 n = counts[lvl];
    UINT64 j;

    for (j = 0; j < n; j++) {
      UINT64 left = base[lvl - 1u] + 2u * j;

      if (node_read(&src, left, pair) != 0) {
        goto done;
      }
      if (node_read(&src, left + 1u, pair + MERKLE_NODE_SIZE) != 0) {
        goto done;
      }
      novus_rs_sha256(pair, 2u * MERKLE_NODE_SIZE, node);
      if (node_write(&dst, base[lvl] + j, node) != 0) {
        goto done;
      }
    }
  }

  if (cache_sync(&dst) != 0) {
    goto done;
  }
  /* The last level holds exactly one node: the root.
   *
   * A node slot is 64 bytes and the root the caller wants is the
   * 32-byte digest, so the root is read into a node-sized buffer and
   * then narrowed. Reading straight into the caller's 32-byte array
   * would write past its end by 32 bytes. */
  if (node_read(&dst, base[levels - 1u], node) != 0) {
    goto done;
  }
  memcpy(root, node, NOVUSFS_MERKLE_ROOT_LEN);

  if (persist) {
    memcpy(fs->sb.merkle_root, root, NOVUSFS_MERKLE_ROOT_LEN);
    fs->sb.merkle_height = levels;
    fs->sb.merkle_leaf_count = (UINT32)leaf_count;
    fs->sb.merkle_valid = 1u;
    if (novusfs_super_flush(fs) != 0) {
      kputs("[MRKL] WARN superblock flush failed\n");
      goto done;
    }
  }
  rc = 0;

done:
  if (databuf != NULL) { novusfs_free(databuf); }
  if (src.buf != NULL) { novusfs_free(src.buf); }
  if (dst.buf != NULL) { novusfs_free(dst.buf); }
  return rc;
}

int novusfs_merkle_build(novusfs_fs_t *fs, UINT8 *out_root) {
  UINT8 root[NOVUSFS_MERKLE_ROOT_LEN];

  if (merkle_rebuild(fs, root, 1) != 0) {
    return -1;
  }
  if (out_root != NULL) {
    memcpy(out_root, root, NOVUSFS_MERKLE_ROOT_LEN);
  }
  return 0;
}

UINT32 novusfs_merkle_verify(novusfs_fs_t *fs, UINT8 *out_root) {
  UINT8 root[NOVUSFS_MERKLE_ROOT_LEN];

  if (fs == NULL || !fs->mounted) {
    return NOVUSFS_MERKLE_UNREADABLE;
  }
  if (fs->sb.merkle_blocks == 0 || fs->sb.merkle_valid == 0) {
    return NOVUSFS_MERKLE_NOT_BUILT;
  }
  /* persist == 0: the rebuild only rewrites the Merkle region, so the
   * superblock - and with it the authoritative root and merkle_valid -
   * is untouched and can be compared against afterwards. */
  if (merkle_rebuild(fs, root, 0) != 0) {
    return NOVUSFS_MERKLE_UNREADABLE;
  }
  if (out_root != NULL) {
    memcpy(out_root, root, NOVUSFS_MERKLE_ROOT_LEN);
  }
  if (memcmp(root, fs->sb.merkle_root, NOVUSFS_MERKLE_ROOT_LEN) != 0) {
    return NOVUSFS_MERKLE_MISMATCH;
  }
  return NOVUSFS_MERKLE_OK;
}

void novusfs_merkle_invalidate(novusfs_fs_t *fs) {
  if (fs == NULL || !fs->mounted) {
    return;
  }
  /* Persisting is the caller's job - wal_commit already flushes the
   * superblock right after this, and flushing twice per transaction
   * would double the metadata writes. */
  fs->sb.merkle_valid = 0;
}
