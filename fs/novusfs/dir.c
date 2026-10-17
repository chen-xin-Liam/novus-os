/**
 * Novus OS - NovusFS directories, M2.
 *
 * A directory is an inode of mode NOVUSFS_IDIR whose data blocks hold a
 * flat array of fixed 64-byte novusfs_dirent_t records (64 per block).
 * An entry with ino == 0 is a free slot: removal clears the field in
 * place instead of compacting the array, which keeps removal a single
 * block update and keeps slot indices stable.
 *
 * Directory size therefore always stays a multiple of the record size,
 * and the slot count is derived from it - there is no separate entry
 * count to keep in sync.
 *
 * Contract with the caller: the mutating entry points take the
 * directory inode by pointer, update it in place and only touch the
 * directory's data blocks on disk. Persisting the inode itself is the
 * caller's job (see novusfs_inode_write_batch), so a transaction that
 * also updates the parent directory can stage both inodes and emit a
 * single redo record per inode-table block.
 *
 * Transaction rule (M2): a given block is logged at most once per
 * transaction. Data blocks reach the disk only when the transaction
 * commits, so a second record for a block would have to be staged from
 * the pre-transaction image on disk and would undo the first. Staging
 * every change of one block into a single image (as novusfs_dir_init
 * does) is both correct and cheaper.
 */

#include "novusfs.h"

/* strlen for the freestanding kernel (kernel.h has no string.h). */
static UINT64 dir_strlen(const char *s) {
  UINT64 n = 0;
  if (s == NULL) {
    return 0;
  }
  while (s[n] != '\0') {
    n++;
  }
  return n;
}

/* Compare a stored (length-counted) name against a C string. */
static int dir_name_eq(const novusfs_dirent_t *de, const char *name) {
  UINT64 i;

  for (i = 0;; i++) {
    if (name[i] == '\0') {
      return i == (UINT64)de->name_len;
    }
    if (i >= (UINT64)de->name_len) {
      return 0;
    }
    if ((UINT8)name[i] != de->name[i]) {
      return 0;
    }
  }
}

static UINT64 dir_slots(const novusfs_inode_t *din) {
  return (UINT64)din->size / NOVUSFS_DIRENT_SIZE;
}

/* Persist a directory data block: through the log inside a transaction,
 * straight to disk when txid == 0 (formatting). */
static int dir_write_block(novusfs_fs_t *fs, UINT64 txid, UINT64 blk,
                           const UINT8 *buf) {
  if (txid != 0) {
    return wal_log_block(fs, txid, blk, buf, NOVUSFS_BLOCK_SIZE);
  }
  return novusfs_write_block(fs, blk, buf);
}

/* Resolve slot `i` of directory `din` to (block, offset). `alloc` lets
 * the caller grow the directory. Returns 0 when the slot is absent. */
static UINT64 dir_slot_block(novusfs_fs_t *fs, novusfs_inode_t *din,
                             UINT64 i, UINT64 *off, int alloc) {
  UINT64 byte_off = i * NOVUSFS_DIRENT_SIZE;
  UINT64 lblk = byte_off / NOVUSFS_BLOCK_SIZE;

  *off = byte_off % NOVUSFS_BLOCK_SIZE;
  return novusfs_inode_bmap(fs, din, lblk, alloc);
}

UINT32 novusfs_dir_lookup(novusfs_fs_t *fs, UINT32 dir_ino,
                          const char *name) {
  novusfs_inode_t din;
  UINT64 slots;
  UINT64 i;
  UINT64 cached = 0;
  int have = 0;

  if (name == NULL || name[0] == '\0') {
    return 0;
  }
  if (novusfs_inode_read(fs, dir_ino, &din) != 0) {
    return 0;
  }
  if (din.mode != NOVUSFS_IDIR) {
    return 0;
  }
  slots = dir_slots(&din);
  for (i = 0; i < slots; i++) {
    UINT64 off;
    UINT64 blk = dir_slot_block(fs, &din, i, &off, 0);
    novusfs_dirent_t de;

    if (blk == 0) {
      break;
    }
    if (!have || cached != blk) {
      if (novusfs_read_block(fs, blk, fs->blkbuf) != 0) {
        return 0;
      }
      cached = blk;
      have = 1;
    }
    memcpy(&de, fs->blkbuf + off, sizeof(de));
    if (de.ino != 0 && dir_name_eq(&de, name)) {
      return de.ino;
    }
  }
  return 0;
}

int novusfs_dir_add(novusfs_fs_t *fs, novusfs_inode_t *din,
                    const char *name, UINT32 ino, UINT64 txid) {
  novusfs_dirent_t de;
  UINT64 slots;
  UINT64 nlen;
  UINT64 i;
  UINT64 off;
  UINT64 blk;

  if (fs == NULL || din == NULL || name == NULL || ino == 0) {
    return -1;
  }
  if (din->mode != NOVUSFS_IDIR) {
    return -1;
  }
  nlen = dir_strlen(name);
  if (nlen == 0 || nlen > NOVUSFS_NAME_MAX) {
    return -1;
  }

  memset(&de, 0, sizeof(de));
  de.ino = ino;
  de.rec_len = (UINT16)NOVUSFS_DIRENT_SIZE;
  de.name_len = (UINT16)nlen;
  memcpy(de.name, name, nlen);

  slots = dir_slots(din);

  /* First pass: reuse a slot freed by an earlier removal. */
  for (i = 0; i < slots; i++) {
    UINT64 cur = dir_slot_block(fs, din, i, &off, 0);
    novusfs_dirent_t cur_de;

    if (cur == 0) {
      break;
    }
    if (novusfs_read_block(fs, cur, fs->blkbuf) != 0) {
      return -1;
    }
    memcpy(&cur_de, fs->blkbuf + off, sizeof(cur_de));
    if (cur_de.ino == 0) {
      memcpy(fs->blkbuf + off, &de, sizeof(de));
      din->mtime = ++fs->mtime_counter;
      return dir_write_block(fs, txid, cur, fs->blkbuf);
    }
  }

  /* Second pass: append one slot, growing the directory as needed. */
  i = slots;
  blk = dir_slot_block(fs, din, i, &off, 1);
  if (blk == 0) {
    return -1;
  }
  if (off == 0) {
    /* The slot opens a brand new block: start from zeroes so the
     * remaining slots read as free. */
    memset(fs->blkbuf, 0, NOVUSFS_BLOCK_SIZE);
  } else if (novusfs_read_block(fs, blk, fs->blkbuf) != 0) {
    return -1;
  }
  memcpy(fs->blkbuf + off, &de, sizeof(de));
  if (dir_write_block(fs, txid, blk, fs->blkbuf) != 0) {
    return -1;
  }
  din->size = (UINT32)((i + 1u) * NOVUSFS_DIRENT_SIZE);
  din->mtime = ++fs->mtime_counter;
  return 0;
}

int novusfs_dir_remove(novusfs_fs_t *fs, novusfs_inode_t *din,
                       const char *name, UINT64 txid) {
  UINT64 slots;
  UINT64 i;

  if (fs == NULL || din == NULL || name == NULL || name[0] == '\0') {
    return -1;
  }
  if (din->mode != NOVUSFS_IDIR) {
    return -1;
  }
  slots = dir_slots(din);
  for (i = 0; i < slots; i++) {
    UINT64 off;
    UINT64 blk = dir_slot_block(fs, din, i, &off, 0);
    novusfs_dirent_t de;

    if (blk == 0) {
      break;
    }
    if (novusfs_read_block(fs, blk, fs->blkbuf) != 0) {
      return -1;
    }
    memcpy(&de, fs->blkbuf + off, sizeof(de));
    if (de.ino != 0 && dir_name_eq(&de, name)) {
      de.ino = 0;
      memcpy(fs->blkbuf + off, &de, sizeof(de));
      if (dir_write_block(fs, txid, blk, fs->blkbuf) != 0) {
        return -1;
      }
      din->mtime = ++fs->mtime_counter;
      return 0;
    }
  }
  return -1; /* not found */
}

/* "." and ".." always land in the first block of a freshly allocated
 * directory, so both are staged into one block image and emitted as ONE
 * redo record.
 *
 * Doing it as two novusfs_dir_add() calls would be wrong: the second
 * call stages its image from the copy on disk, which until the
 * transaction commits still lacks the first entry - the two records
 * would describe the same block and the later one would silently drop
 * ".". The general rule for M2 is therefore: a given block is logged at
 * most once per transaction.
 */
int novusfs_dir_init(novusfs_fs_t *fs, novusfs_inode_t *din,
                     UINT32 parent_ino, UINT64 txid) {
  novusfs_dirent_t de;
  UINT64 off;
  UINT64 blk;

  if (fs == NULL || din == NULL || din->ino == 0) {
    return -1;
  }
  if (din->mode != NOVUSFS_IDIR || din->size != 0) {
    return -1; /* only meaningful on a freshly allocated directory */
  }
  blk = dir_slot_block(fs, din, 0, &off, 1);
  if (blk == 0 || off != 0) {
    return -1;
  }
  memset(fs->blkbuf, 0, NOVUSFS_BLOCK_SIZE);

  memset(&de, 0, sizeof(de));
  de.ino = din->ino;
  de.rec_len = (UINT16)NOVUSFS_DIRENT_SIZE;
  de.name_len = 1u;
  de.name[0] = '.';
  memcpy(fs->blkbuf, &de, sizeof(de));

  memset(&de, 0, sizeof(de));
  de.ino = parent_ino;
  de.rec_len = (UINT16)NOVUSFS_DIRENT_SIZE;
  de.name_len = 2u;
  de.name[0] = '.';
  de.name[1] = '.';
  memcpy(fs->blkbuf + NOVUSFS_DIRENT_SIZE, &de, sizeof(de));

  if (dir_write_block(fs, txid, blk, fs->blkbuf) != 0) {
    return -1;
  }
  din->size = 2u * NOVUSFS_DIRENT_SIZE;
  din->mtime = ++fs->mtime_counter;
  return 0;
}

int novusfs_dir_readdir(novusfs_fs_t *fs, UINT32 dir_ino,
                        novusfs_readdir_cb cb, void *ctx) {
  novusfs_inode_t din;
  UINT64 slots;
  UINT64 i;
  UINT64 cached = 0;
  int have = 0;

  if (cb == NULL) {
    return -1;
  }
  if (novusfs_inode_read(fs, dir_ino, &din) != 0) {
    return -1;
  }
  if (din.mode != NOVUSFS_IDIR) {
    return -1;
  }
  slots = dir_slots(&din);
  for (i = 0; i < slots; i++) {
    UINT64 off;
    UINT64 blk = dir_slot_block(fs, &din, i, &off, 0);
    novusfs_dirent_t de;
    char nbuf[NOVUSFS_NAME_MAX + 1u];

    if (blk == 0) {
      break;
    }
    if (!have || cached != blk) {
      if (novusfs_read_block(fs, blk, fs->blkbuf) != 0) {
        return -1;
      }
      cached = blk;
      have = 1;
    }
    memcpy(&de, fs->blkbuf + off, sizeof(de));
    if (de.ino == 0 || de.name_len > NOVUSFS_NAME_MAX) {
      continue;
    }
    memcpy(nbuf, de.name, de.name_len);
    nbuf[de.name_len] = '\0';
    cb(ctx, de.ino, nbuf);
  }
  return 0;
}
