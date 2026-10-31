/**
 * Novus OS - NovusFS inode table, M2.
 *
 * The inode table is a flat array of 128-byte inodes starting at
 * inode_tbl_start. Inode numbers are 1-based (0 means "no inode") and
 * NOVUSFS_ROOT_INO is the root directory.
 *
 * Block mapping is 11 direct pointers plus a single indirect block,
 * which holds 512 further pointers: enough for the small images M2
 * creates while keeping the inode inside one 128-byte slot.
 *
 * novusfs_inode_bmap() updates the inode in place (new indirect block,
 * new data block, block counter) but does NOT write it back: the caller
 * owns the copy and persists it once the whole update is done, so a
 * single transaction never leaves a half-updated inode on disk.
 */

#include "novusfs.h"

/* Resolve an inode number to (block, byte offset) inside the table. */
static int ino_locate(novusfs_fs_t *fs, UINT32 ino, UINT64 *blk,
                      UINT64 *off) {
  UINT64 idx;
  UINT64 b;

  if (fs == NULL || !fs->mounted || ino == 0) {
    return -1;
  }
  if (ino >= fs->sb.inode_count) {
    return -1;
  }
  idx = (UINT64)ino - 1u;
  b = idx / NOVUSFS_INODES_PER_BLOCK;
  if (b >= fs->sb.inode_tbl_blocks) {
    return -1;
  }
  *blk = fs->sb.inode_tbl_start + b;
  *off = (idx % NOVUSFS_INODES_PER_BLOCK) * NOVUSFS_INODE_SIZE;
  return 0;
}

int novusfs_inode_read(novusfs_fs_t *fs, UINT32 ino, novusfs_inode_t *out) {
  UINT64 blk, off;

  if (out == NULL || ino_locate(fs, ino, &blk, &off) != 0) {
    return -1;
  }
  if (novusfs_read_block(fs, blk, fs->scratch) != 0) {
    return -1;
  }
  memcpy(out, fs->scratch + off, sizeof(*out));
  return 0;
}

int novusfs_inode_write_tx(novusfs_fs_t *fs, const novusfs_inode_t *in,
                           UINT64 txid) {
  UINT64 blk, off;

  if (in == NULL || ino_locate(fs, in->ino, &blk, &off) != 0) {
    return -1;
  }
  if (novusfs_read_block(fs, blk, fs->scratch) != 0) {
    return -1;
  }
  memcpy(fs->scratch + off, in, sizeof(*in));

  if (txid != 0) {
    /* Redo record: the whole inode-table block is replayed onto the
     * table when the transaction commits. */
    return wal_log_block(fs, txid, blk, fs->scratch, NOVUSFS_BLOCK_SIZE);
  }
  return novusfs_write_block(fs, blk, fs->scratch);
}

int novusfs_inode_write(novusfs_fs_t *fs, const novusfs_inode_t *in) {
  return novusfs_inode_write_tx(fs, in, 0);
}

/* Which table block an inode lives in (needed to group a batch). */
static int ino_block_of(novusfs_fs_t *fs, UINT32 ino, UINT64 *blk) {
  UINT64 off;
  return ino_locate(fs, ino, blk, &off);
}

int novusfs_inode_write_batch(novusfs_fs_t *fs, const novusfs_inode_t *ins,
                              UINTN count, UINT64 txid) {
  UINTN i;

  if (fs == NULL || ins == NULL || count == 0) {
    return -1;
  }
  for (i = 0; i < count; i++) {
    UINT64 blk;
    UINTN j;
    UINTN k;
    int already = 0;

    /* Skip inodes whose block was already flushed by an earlier
     * iteration: they were patched together with that group. */
    for (k = 0; k < i; k++) {
      UINT64 prev;
      if (ino_block_of(fs, ins[k].ino, &prev) == 0 &&
          ino_block_of(fs, ins[i].ino, &blk) == 0 && prev == blk) {
        already = 1;
        break;
      }
    }
    if (already) {
      continue;
    }
    if (ino_block_of(fs, ins[i].ino, &blk) != 0) {
      return -1;
    }
    /* Reload the on-disk table block once per group, then emit one full
     * block image so distinct inodes sharing a block cannot overwrite
     * one another's staged updates during redo. */
    if (novusfs_read_block(fs, blk, fs->scratch) != 0) {
      return -1;
    }
    /* Patch this inode and every inode sharing its block. */
    for (j = i; j < count; j++) {
      UINT64 jblk;
      UINT64 joff;
      if (ino_locate(fs, ins[j].ino, &jblk, &joff) == 0 && jblk == blk) {
        memcpy(fs->scratch + joff, &ins[j], sizeof(ins[j]));
      }
    }
    if (txid != 0) {
      if (wal_log_block(fs, txid, blk, fs->scratch,
                        NOVUSFS_BLOCK_SIZE) != 0) {
        return -1;
      }
    } else if (novusfs_write_block(fs, blk, fs->scratch) != 0) {
      return -1;
    }
  }
  return 0;
}

void novusfs_inode_free(novusfs_fs_t *fs, UINT32 ino) {
  UINT64 blk, off;

  if (ino_locate(fs, ino, &blk, &off) != 0) {
    return;
  }
  if (novusfs_read_block(fs, blk, fs->scratch) != 0) {
    return;
  }
  memset(fs->scratch + off, 0, NOVUSFS_INODE_SIZE);
  (VOID)novusfs_write_block(fs, blk, fs->scratch);
}

UINT32 novusfs_inode_alloc(novusfs_fs_t *fs, UINT16 mode) {
  UINT64 b;

  if (fs == NULL || !fs->mounted) {
    return 0;
  }
  /* Block-at-a-time scan: 32 inodes per read. */
  for (b = 0; b < fs->sb.inode_tbl_blocks; b++) {
    UINTN i;

    if (novusfs_read_block(fs, fs->sb.inode_tbl_start + b, fs->scratch) != 0) {
      return 0;
    }
    for (i = 0; i < (UINTN)NOVUSFS_INODES_PER_BLOCK; i++) {
      UINT64 off = (UINT64)i * NOVUSFS_INODE_SIZE;
      UINT32 ino = (UINT32)(b * NOVUSFS_INODES_PER_BLOCK + (UINT64)i) + 1u;
      novusfs_inode_t in;

      if (ino >= fs->sb.inode_count) {
        return 0;
      }
      memcpy(&in, fs->scratch + off, sizeof(in));
      if (in.mode == NOVUSFS_IFREE) {
        memset(&in, 0, sizeof(in));
        in.mode = mode;
        in.ino = ino;
        in.size = 0;
        in.nlinks = (mode == NOVUSFS_IDIR) ? 2u : 1u;
        in.blocks = 0;
        in.mtime = ++fs->mtime_counter;
        memcpy(fs->scratch + off, &in, sizeof(in));
        if (novusfs_write_block(fs, fs->sb.inode_tbl_start + b,
                                fs->scratch) != 0) {
          return 0;
        }
        return ino;
      }
    }
  }
  return 0; /* inode table full */
}

/* Zero a freshly allocated indirect block. */
static int zero_block(novusfs_fs_t *fs, UINT64 blk) {
  memset(fs->scratch, 0, NOVUSFS_BLOCK_SIZE);
  return novusfs_write_block(fs, blk, fs->scratch);
}

UINT64 novusfs_inode_bmap(novusfs_fs_t *fs, novusfs_inode_t *in,
                          UINT64 lblk, int alloc) {
  UINT64 res = 0;

  if (fs == NULL || in == NULL || in->ino == 0) {
    return 0;
  }
  if (lblk < (UINT64)NOVUSFS_NDIR) {
    if (in->dblock[lblk] != 0) {
      return in->dblock[lblk];
    }
    if (!alloc) {
      return 0;
    }
    res = novusfs_balloc_alloc(fs);
    if (res == 0) {
      return 0;
    }
    /* The inode pointer is only changed in this caller-owned copy; the
     * allocator has already written its bitmap bit directly to disk.
     * A separate inode write is still needed to persist the new pointer. */
    in->dblock[lblk] = res;
    in->blocks++;
    return res;
  }

  /* Indirect and double-indirect levels. */
  {
    UINT64 dbl_base = (UINT64)NOVUSFS_NDIR + (UINT64)NOVUSFS_INDIRECT_CNT;

    /* Single indirect: lblk in [NDIR, NDIR + INDIRECT_CNT). */
    if (lblk < dbl_base) {
      UINT64 idx = lblk - (UINT64)NOVUSFS_NDIR;
      UINT64 *ent;

      if (in->iblk == 0) {
        if (!alloc) {
          return 0;
        }
        in->iblk = novusfs_balloc_alloc(fs);
        if (in->iblk == 0) {
          return 0;
        }
        if (zero_block(fs, in->iblk) != 0) {
          return 0;
        }
        /* The indirect block is initialized eagerly, before its eventual
         * inode-table update; later pointer edits also write this block directly.
         * Zeroing makes absent entries read as holes, not stale block numbers. */
        in->blocks++;
      }
      if (novusfs_read_block(fs, in->iblk, fs->scratch) != 0) {
        return 0;
      }
      ent = (UINT64 *)(VOID *)(fs->scratch + idx * 8u);
      if (*ent != 0) {
        return *ent;
      }
      if (!alloc) {
        return 0;
      }
      res = novusfs_balloc_alloc(fs);
      if (res == 0) {
        return 0;
      }
      /* novusfs_balloc_alloc() clobbers fs->scratch, so re-read the
       * indirect block before patching its entry. */
      if (novusfs_read_block(fs, in->iblk, fs->scratch) != 0) {
        return 0;
      }
      ent = (UINT64 *)(VOID *)(fs->scratch + idx * 8u);
      *ent = res;
      if (novusfs_write_block(fs, in->iblk, fs->scratch) != 0) {
        return 0;
      }
      in->blocks++;
      return res;
    }

    /* Double indirect: each L1 block points to INDIRECT_CNT L2 blocks, each
     * L2 to INDIRECT_CNT data blocks. Supports files far larger than the
     * single-indirect ceiling so the system partition can hold multi-MB
     * assets (fonts, ...). */
    {
      UINT64 rel = lblk - dbl_base;
      UINT64 l1 = rel / (UINT64)NOVUSFS_INDIRECT_CNT;
      UINT64 l2 = rel % (UINT64)NOVUSFS_INDIRECT_CNT;
      UINT64 l1blk;

      if (l1 >= (UINT64)NOVUSFS_INDIRECT_CNT) {
        return 0; /* file too large */
      }
      if (in->diblk == 0) {
        if (!alloc) {
          return 0;
        }
        in->diblk = novusfs_balloc_alloc(fs);
        if (in->diblk == 0) {
          return 0;
        }
        if (zero_block(fs, in->diblk) != 0) {
          return 0;
        }
        in->blocks++;
      }
      if (novusfs_read_block(fs, in->diblk, fs->scratch) != 0) {
        return 0;
      }
      {
        UINT64 *e1 = (UINT64 *)(VOID *)(fs->scratch + l1 * 8u);
        if (*e1 == 0) {
          UINT64 new_l1;

          if (!alloc) {
            return 0;
          }
          new_l1 = novusfs_balloc_alloc(fs);
          if (new_l1 == 0) {
            return 0;
          }
          /* balloc() clobbered fs->scratch, so re-read the diblk before
           * patching its L1 pointer and persisting it. */
          if (novusfs_read_block(fs, in->diblk, fs->scratch) != 0) {
            return 0;
          }
          e1 = (UINT64 *)(VOID *)(fs->scratch + l1 * 8u);
          *e1 = new_l1;
          if (novusfs_write_block(fs, in->diblk, fs->scratch) != 0) {
            return 0;
          }
          if (zero_block(fs, new_l1) != 0) {
            return 0;
          }
          in->blocks++;
          l1blk = new_l1;
        } else {
          l1blk = *e1;
        }
      }
      if (novusfs_read_block(fs, l1blk, fs->scratch) != 0) {
        return 0;
      }
      {
        UINT64 *e2 = (UINT64 *)(VOID *)(fs->scratch + l2 * 8u);
        if (*e2 != 0) {
          return *e2;
        }
        if (!alloc) {
          return 0;
        }
        res = novusfs_balloc_alloc(fs);
        if (res == 0) {
          return 0;
        }
        /* balloc() clobbered fs->scratch, so re-read the L2 block before
         * patching its data pointer and persisting it. */
        if (novusfs_read_block(fs, l1blk, fs->scratch) != 0) {
          return 0;
        }
        e2 = (UINT64 *)(VOID *)(fs->scratch + l2 * 8u);
        *e2 = res;
        if (novusfs_write_block(fs, l1blk, fs->scratch) != 0) {
          return 0;
        }
        in->blocks++;
        return res;
      }
    }
  }
}
