/**
 * Novus OS - NovusFS high level API and self test, M2.
 *
 * This is the layer that owns transaction boundaries. Lower layers only
 * stage changes: directory calls update an in-memory inode and log
 * their data blocks, and this layer decides when a set of changes
 * becomes one transaction and gets committed (README 4.2.1).
 *
 * Write path of a file update, in order:
 *   1. wal_begin()                     open a transaction
 *   2. wal_log_block() per data block  redo record, before the block
 *                                      is touched on disk
 *   3. novusfs_inode_write_tx()        log the inode-table block that
 *                                      carries the new size
 *   4. wal_commit()                    commit marker, then apply every
 *                                      record of the transaction
 *
 * Nothing writes to the data area before step 4, so an interrupted
 * update either is fully recorded (and replayed on the next mount) or
 * leaves no trace at all.
 */

#include "novusfs.h"

/* Ramdisk the selftest formats: 4 MiB, comfortably above the metadata
 * floor and cheap to allocate from the PMM. */
#define NOVUSFS_TEST_DISK_BYTES (4ULL * 1024ULL * 1024ULL)

/* The single mounted instance. */
static novusfs_fs_t g_novusfs;
static novusfs_fs_t *g_root_fs;

novusfs_fs_t *novusfs_get(void) {
  if (!g_novusfs.mounted) {
    return NULL;
  }
  return &g_novusfs;
}

novusfs_fs_t *novusfs_root(void) {
  if (g_root_fs != NULL && g_root_fs->mounted) {
    return g_root_fs;
  }
  return novusfs_get();
}

int novusfs_root_persistent(void) { return g_root_fs != NULL && g_root_fs->mounted; }

void novusfs_set_root(novusfs_fs_t *fs) {
  if (g_root_fs == NULL && fs != NULL && fs->mounted) {
    g_root_fs = fs;
  }
}

/* ------------------------------------------------------------------ */
/* Small console helpers (no libc)                                     */
/* ------------------------------------------------------------------ */

static void kputs_dec(UINT64 v) {
  char buf[24];
  UINTN pos = 23;

  buf[23] = '\0';
  if (v == 0) {
    kputs("0");
    return;
  }
  while (v > 0 && pos > 0) {
    buf[--pos] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  kputs(buf + pos);
}

static int streq(const char *a, const char *b) {
  UINTN i = 0;

  if (a == NULL || b == NULL) {
    return 0;
  }
  for (;; i++) {
    if (a[i] != b[i]) {
      return 0;
    }
    if (a[i] == '\0') {
      return 1;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Format and mount                                                    */
/* ------------------------------------------------------------------ */

int novusfs_format(novusfs_fs_t *fs, blkdev_t *dev) {
  novusfs_inode_t root;
  UINT32 ino;

  if (fs == NULL || dev == NULL) {
    return -1;
  }
  if (novusfs_format_layout(fs, dev) != 0) {
    return -1;
  }

  /* Root directory. Written straight through (txid == 0): the ring is
   * empty at this point and there is nothing to protect yet. */
  ino = novusfs_inode_alloc(fs, NOVUSFS_IDIR);
  if (ino == 0) {
    kputs("[NOVS] ERROR root inode allocation failed\n");
    return -1;
  }
  if (novusfs_inode_read(fs, ino, &root) != 0) {
    return -1;
  }
  if (novusfs_dir_init(fs, &root, ino, 0) != 0) {
    kputs("[NOVS] ERROR root directory init failed\n");
    return -1;
  }
  if (novusfs_inode_write(fs, &root) != 0) {
    return -1;
  }
  fs->sb.root_inode = ino;
  return novusfs_super_flush(fs);
}

int novusfs_mount(novusfs_fs_t *fs, blkdev_t *dev) {
  UINT64 replayed;

  if (novusfs_super_read(fs, dev) != 0) {
    return -1;
  }
  /* Recovery uses the geometry and ring pointers selected from the
   * superblock; Merkle verification is a separate API, not done here.
   * This keeps log recovery independent of the caller's integrity policy. */
  replayed = wal_recover(fs);
  if (replayed != 0) {
    kputs("[NOVS] wal: replayed ");
    kputs_dec(replayed);
    kputs(" entries\n");
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Creation                                                            */
/* ------------------------------------------------------------------ */

UINT32 novusfs_creat(novusfs_fs_t *fs, UINT32 dir_ino, const char *name) {
  novusfs_inode_t pdin;
  UINT32 ino;
  UINT64 txid;

  if (fs == NULL || !fs->mounted || name == NULL || name[0] == '\0') {
    return 0;
  }
  ino = novusfs_inode_alloc(fs, NOVUSFS_IFILE);
  if (ino == 0) {
    return 0;
  }
  if (novusfs_inode_read(fs, dir_ino, &pdin) != 0) {
    return 0;
  }
  txid = wal_begin(fs);
  if (txid == 0) {
    return 0;
  }
  if (novusfs_dir_add(fs, &pdin, name, ino, txid) != 0 ||
      novusfs_inode_write_tx(fs, &pdin, txid) != 0) {
    wal_abort(fs, txid);
    return 0;
  }
  if (wal_commit(fs, txid) != 0) {
    return 0;
  }
  return ino;
}

/* Remove one name from a directory. The change goes through the write
 * ahead log like every other metadata change, so a power cut leaves the
 * directory as it was. */
int novusfs_unlink(novusfs_fs_t *fs, UINT32 dir_ino, const char *name) {
  novusfs_inode_t din;
  UINT64           txid;

  if (fs == NULL || !fs->mounted || name == NULL) {
    return -1;
  }
  if (novusfs_inode_read(fs, dir_ino, &din) != 0) {
    return -1;
  }
  txid = wal_begin(fs);
  if (txid == 0u) {
    return -1;
  }
  if (novusfs_dir_remove(fs, &din, name, txid) != 0) {
    wal_abort(fs, txid);
    return -1;
  }
  if (novusfs_inode_write_tx(fs, &din, txid) != 0) {
    wal_abort(fs, txid);
    return -1;
  }
  if (wal_commit(fs, txid) != 0) {
    return -1;
  }
  return 0;
}

UINT32 novusfs_mkdir(novusfs_fs_t *fs, UINT32 dir_ino, const char *name) {
  novusfs_inode_t pdin;
  novusfs_inode_t ndin;
  novusfs_inode_t batch[2];
  UINT32 ino;
  UINT64 txid;

  if (fs == NULL || !fs->mounted || name == NULL || name[0] == '\0') {
    return 0;
  }
  ino = novusfs_inode_alloc(fs, NOVUSFS_IDIR);
  if (ino == 0) {
    return 0;
  }
  if (novusfs_inode_read(fs, ino, &ndin) != 0) {
    return 0;
  }
  if (novusfs_inode_read(fs, dir_ino, &pdin) != 0) {
    return 0;
  }
  txid = wal_begin(fs);
  if (txid == 0) {
    return 0;
  }
  /* "." and ".." of the new directory, then the entry in the parent. */
  if (novusfs_dir_init(fs, &ndin, dir_ino, txid) != 0 ||
      novusfs_dir_add(fs, &pdin, name, ino, txid) != 0) {
    wal_abort(fs, txid);
    return 0;
  }
  pdin.nlinks++;

  /* The two inodes usually share an inode-table block, so they must be
   * persisted together - two separate records for one block would make
   * the second overwrite the first at replay time. */
  batch[0] = ndin;
  batch[1] = pdin;
  if (novusfs_inode_write_batch(fs, batch, 2, txid) != 0) {
    wal_abort(fs, txid);
    return 0;
  }
  if (wal_commit(fs, txid) != 0) {
    return 0;
  }
  return ino;
}

/* ------------------------------------------------------------------ */
/* Data IO                                                             */
/* ------------------------------------------------------------------ */

INT64 novusfs_write(novusfs_fs_t *fs, UINT32 ino, UINT64 off,
                    const void *data, UINT64 len) {
  novusfs_inode_t in;
  const UINT8 *src = (const UINT8 *)data;
  UINT64 old_size;
  UINT64 pos = off;
  UINT64 done = 0;
  UINT64 txid;

  if (fs == NULL || !fs->mounted || data == NULL) {
    return -1;
  }
  if (len == 0) {
    return 0;
  }
  if (novusfs_inode_read(fs, ino, &in) != 0) {
    return -1;
  }
  if (in.mode != NOVUSFS_IFILE) {
    return -1;
  }
  txid = wal_begin(fs);
  if (txid == 0) {
    return -1;
  }
  old_size = in.size;

  /* Split the request at block boundaries: each iteration logs a complete
   * post-write block image, including untouched bytes from partial writes.
   * Partial blocks are read first so bytes outside the span are preserved. */
  while (done < len) {
    UINT64 lblk = pos / NOVUSFS_BLOCK_SIZE;
    UINT64 boff = pos % NOVUSFS_BLOCK_SIZE;
    UINT64 chunk = NOVUSFS_BLOCK_SIZE - boff;
    UINT64 blk;

    if (chunk > len - done) {
      chunk = len - done;
    }
    blk = novusfs_inode_bmap(fs, &in, lblk, 1);
    if (blk == 0) {
      wal_abort(fs, txid);
      return -1;
    }
    if (chunk == NOVUSFS_BLOCK_SIZE) {
      /* Whole block: no need to preserve previous contents. */
      memcpy(fs->blkbuf, src + done, chunk);
    } else {
      UINT64 end;
      UINT64 tail;

      if (novusfs_read_block(fs, blk, fs->blkbuf) != 0) {
        wal_abort(fs, txid);
        return -1;
      }
      memcpy(fs->blkbuf + boff, src + done, chunk);

      /* Anything past EOF must read back as zero, otherwise a freshly
       * allocated block would expose whatever the ramdisk held. */
      end = boff + chunk;
      if (pos + chunk > old_size && end < NOVUSFS_BLOCK_SIZE) {
        tail = NOVUSFS_BLOCK_SIZE - end;
        memset(fs->blkbuf + end, 0, tail);
      }
    }
    /* Redo record first: the block itself is only written at commit. */
    if (wal_log_block(fs, txid, blk, fs->blkbuf, NOVUSFS_BLOCK_SIZE) != 0) {
      wal_abort(fs, txid);
      return -1;
    }
    pos += chunk;
    done += chunk;
  }

  if (pos > in.size) {
    in.size = (UINT32)pos;
  }
  in.mtime = ++fs->mtime_counter;
  if (novusfs_inode_write_tx(fs, &in, txid) != 0) {
    wal_abort(fs, txid);
    return -1;
  }
  if (wal_commit(fs, txid) != 0) {
    return -1;
  }
  return (INT64)done;
}

INT64 novusfs_read(novusfs_fs_t *fs, UINT32 ino, UINT64 off, void *dst,
                   UINT64 len) {
  novusfs_inode_t in;
  UINT8 *out = (UINT8 *)dst;
  UINT64 pos = off;
  UINT64 done = 0;

  if (fs == NULL || !fs->mounted || dst == NULL) {
    return -1;
  }
  if (len == 0) {
    return 0;
  }
  if (novusfs_inode_read(fs, ino, &in) != 0) {
    return -1;
  }
  if (in.mode != NOVUSFS_IFILE) {
    return -1;
  }
  if (off >= (UINT64)in.size) {
    return 0;
  }
  if (off + len > (UINT64)in.size) {
    len = (UINT64)in.size - off;
  }
  while (done < len) {
    UINT64 lblk = pos / NOVUSFS_BLOCK_SIZE;
    UINT64 boff = pos % NOVUSFS_BLOCK_SIZE;
    UINT64 chunk = NOVUSFS_BLOCK_SIZE - boff;
    UINT64 blk;

    if (chunk > len - done) {
      chunk = len - done;
    }
    blk = novusfs_inode_bmap(fs, &in, lblk, 0);
    if (blk == 0) {
      /* Sparse hole: reads as zeros. */
      memset(out + done, 0, chunk);
    } else {
      if (novusfs_read_block(fs, blk, fs->blkbuf) != 0) {
        return -1;
      }
      memcpy(out + done, fs->blkbuf + boff, chunk);
    }
    pos += chunk;
    done += chunk;
  }
  return (INT64)done;
}

int novusfs_stat(novusfs_fs_t *fs, UINT32 ino, UINT64 *out_size,
                 UINT16 *out_mode) {
  novusfs_inode_t in;

  if (fs == NULL || !fs->mounted) {
    return -1;
  }
  if (novusfs_inode_read(fs, ino, &in) != 0) {
    return -1;
  }
  if (out_size != NULL) {
    *out_size = in.size;
  }
  if (out_mode != NULL) {
    *out_mode = in.mode;
  }
  return 0;
}

UINT32 novusfs_lookup_path(novusfs_fs_t *fs, UINT32 dir_ino,
                           const char *path) {
  UINT32 cur = dir_ino;
  const char *p = path;

  if (fs == NULL || !fs->mounted || p == NULL) {
    return 0;
  }
  if (*p == '/') {
    cur = fs->sb.root_inode;
    p++;
  }
  while (*p != '\0') {
    char comp[NOVUSFS_NAME_MAX + 1u];
    UINTN n = 0;

    while (*p != '\0' && *p != '/') {
      if (n < NOVUSFS_NAME_MAX) {
        comp[n++] = *p;
      }
      p++;
    }
    comp[n] = '\0';
    if (*p == '/') {
      p++;
    }
    if (n == 0) {
      continue;
    }
    /* "." and ".." are real entries, so plain lookup handles them. */
    cur = novusfs_dir_lookup(fs, cur, comp);
    if (cur == 0) {
      return 0;
    }
  }
  return cur;
}

/* ------------------------------------------------------------------ */
/* Self test                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
  UINTN   count;
  UINT32  found;
  const char *want;
} ls_ctx_t;

static void ls_cb(void *ctx, UINT32 ino, const char *name) {
  ls_ctx_t *c = (ls_ctx_t *)ctx;

  c->count++;
  if (c->want != NULL && streq(name, c->want)) {
    c->found = ino;
  }
}

/* Report one check. `ok` non-zero means PASS. */
static int check(const char *what, int ok) {
  kputs("[NOVS] ");
  kputs(ok ? "PASS " : "FAIL ");
  kputs(what);
  kputs("\n");
  return ok ? 0 : -1;
}

int novusfs_selftest(void) {
  blkdev_t *dev;
  novusfs_fs_t *fs = &g_novusfs;
  int fails = 0;
  UINT32 root;
  UINT32 sysdir;
  UINT32 file;
  UINT32 tmp;
  ls_ctx_t ls;
  UINT64 size = 0;
  UINT16 mode = 0;
  UINT64 used;
  UINT64 valid;
  INT64 n;
  char buf[64];
  const char *msg = "Hello NovusFS!";
  UINT64 msg_len = 14u; /* strlen("Hello NovusFS!") */

  kputs("[NOVS] === M3 selftest: NovusFS + WAL + Merkle ===\n");

  /* 1. Backing device. */
  dev = ramdisk_init(NOVUSFS_TEST_DISK_BYTES);
  if (dev == NULL) {
    kputs("[NOVS] FAIL ramdisk_init (no PMM pages?)\n");
    return -1;
  }
  kputs("[NOVS] ramdisk: ");
  kputs_dec(dev->sector_count);
  kputs(" sectors\n");

  /* 2. Format. */
  if (novusfs_format(fs, dev) != 0) {
    kputs("[NOVS] FAIL format\n");
    return -1;
  }
  novusfs_dump_layout(fs);
  root = fs->sb.root_inode;
  fails += check("format + root inode", root != 0);

  /* 3. Root directory contains "." and "..". */
  ls.count = 0;
  ls.found = 0;
  ls.want = ".";
  if (novusfs_dir_readdir(fs, root, ls_cb, &ls) != 0) {
    fails += check("readdir root", 0);
  } else {
    fails += check("readdir root: 2 entries + self ref",
                   ls.count == 2u && ls.found == root);
  }

  /* 4. mkdir. */
  sysdir = novusfs_mkdir(fs, root, "system");
  fails += check("mkdir /system", sysdir != 0);
  tmp = novusfs_dir_lookup(fs, root, "system");
  fails += check("lookup /system matches mkdir", tmp != 0 && tmp == sysdir);
  tmp = novusfs_lookup_path(fs, root, "/system");
  fails += check("path lookup /system", tmp == sysdir);

  /* 5. creat + write + read. */
  file = novusfs_creat(fs, sysdir, "hello.txt");
  fails += check("creat /system/hello.txt", file != 0);

  n = novusfs_write(fs, file, 0, msg, msg_len);
  fails += check("write returns full length", n == (INT64)msg_len);

  memset(buf, 0, sizeof(buf));
  n = novusfs_read(fs, file, 0, buf, sizeof(buf));
  fails += check("read returns full length", n == (INT64)msg_len);
  fails += check("read back matches write", streq(buf, msg));

  /* 6. stat. */
  if (novusfs_stat(fs, file, &size, &mode) != 0) {
    fails += check("stat", 0);
  } else {
    fails += check("stat size and mode",
                   size == msg_len && mode == NOVUSFS_IFILE);
  }

  /* 7. Directory listing shows the new file. */
  ls.count = 0;
  ls.found = 0;
  ls.want = "hello.txt";
  if (novusfs_dir_readdir(fs, sysdir, ls_cb, &ls) != 0) {
    fails += check("readdir /system", 0);
  } else {
    /* "." and ".." from mkdir, plus the file created below. */
    fails += check("readdir /system finds hello.txt",
                   ls.found == file && ls.count == 3u);
  }

  /* 8. WAL state: records present, all checksums good. */
  used = wal_used_bytes(fs);
  valid = wal_verify(fs);
  fails += check("wal used > 0", used > 0);
  fails += check("wal entries verify (CRC-32)", valid > 0 && !fs->wal_corrupt);
  kputs("[NOVS] wal: used=");
  kputs_dec(used);
  kputs(" bytes, entries=");
  kputs_dec(valid);
  kputs("\n");

  /* 9. Remount: everything must survive, and recovery replays cleanly. */
  novusfs_unmount(fs);
  if (novusfs_mount(fs, dev) != 0) {
    fails += check("remount", 0);
    return (fails == 0) ? 0 : -1;
  }
  fails += check("remount", 1);

  tmp = novusfs_lookup_path(fs, fs->sb.root_inode, "/system/hello.txt");
  fails += check("remount: file still reachable", tmp == file);

  memset(buf, 0, sizeof(buf));
  n = novusfs_read(fs, tmp, 0, buf, sizeof(buf));
  fails += check("remount: contents intact",
                 n == (INT64)msg_len && streq(buf, msg));

  /* 10. Larger write spanning several blocks, read back and verified. */
  {
    UINT64 big_len = NOVUSFS_BLOCK_SIZE * 2u + 100u;
    static UINT8 wbuf[NOVUSFS_BLOCK_SIZE * 3u];
    static UINT8 rbuf[NOVUSFS_BLOCK_SIZE * 3u];
    UINT64 i;
    int same;

    for (i = 0; i < big_len; i++) {
      wbuf[i] = (UINT8)(i * 7u + 3u);
    }
    n = novusfs_write(fs, file, 0, wbuf, big_len);
    fails += check("multi-block write", n == (INT64)big_len);

    memset(rbuf, 0, sizeof(rbuf));
    n = novusfs_read(fs, file, 0, rbuf, big_len);
    same = (n == (INT64)big_len);
    if (same) {
      for (i = 0; i < big_len; i++) {
        if (rbuf[i] != wbuf[i]) {
          same = 0;
          break;
        }
      }
    }
    fails += check("multi-block read matches", same);
  }

  /* 11. Merkle tree over the protected region (README 4.2.2). */
  fails += check("merkle build", novusfs_merkle_build(fs, NULL) == 0);
  fails += check("merkle verify after build",
                 novusfs_merkle_verify(fs, NULL) == NOVUSFS_MERKLE_OK);
  kputs("[NOVS] merkle: leaves=");
  kputs_dec(fs->sb.merkle_leaf_count);
  kputs(" height=");
  kputs_dec(fs->sb.merkle_height);
  kputs("\n");

  /* 12. A committed transaction must drop the stored root's validity:
   * the tree no longer describes the image. */
  n = novusfs_write(fs, file, 0, msg, msg_len);
  fails += check("merkle invalidated by a committed write",
                 n == (INT64)msg_len && fs->sb.merkle_valid == 0u);
  fails += check("merkle reports not-built while invalid",
                 novusfs_merkle_verify(fs, NULL) ==
                     NOVUSFS_MERKLE_NOT_BUILT);
  fails += check("merkle rebuild after write",
                 novusfs_merkle_build(fs, NULL) == 0 &&
                 novusfs_merkle_verify(fs, NULL) == NOVUSFS_MERKLE_OK);

  /* 13. Flip one byte straight on the device, bypassing the filesystem:
   * the tree must detect it (README 18). */
  {
    /* static: a 512-byte frame in the middle of an already deep call
     * chain is not worth spending kernel stack on. */
    static UINT8 sector[BLKDEV_SECTOR_SIZE];
    UINT64 sec = fs->data_start * (UINT64)NOVUSFS_SECTORS_PER_BLOCK;

    if (blkdev_read(dev, sec, 1, sector) != 0) {
      fails += check("merkle tamper setup", 0);
    } else {
      sector[0] ^= 0xffu;
      fails += check("merkle tamper setup",
                     blkdev_write(dev, sec, 1, sector) == 0);
      fails += check("merkle detects tampering",
                     novusfs_merkle_verify(fs, NULL) ==
                         NOVUSFS_MERKLE_MISMATCH);

      /* Undo the damage and rebuild: the image is sound again. */
      sector[0] ^= 0xffu;
      (VOID)blkdev_write(dev, sec, 1, sector);
      fails += check("merkle rebuild after restore",
                     novusfs_merkle_build(fs, NULL) == 0 &&
                     novusfs_merkle_verify(fs, NULL) == NOVUSFS_MERKLE_OK);
    }
  }

  kputs("[NOVS] === selftest ");
  kputs(fails == 0 ? "PASSED" : "FAILED");
  kputs(" ===\n");

  novusfs_unmount(fs);
  return (fails == 0) ? 0 : -1;
}
