/**
 * Novus OS - ext4 directories (M12).
 *
 * A directory is a file whose data blocks hold a chain of variable length
 * records (ext4_dir_entry_2): inode, record length, name length, file
 * type and the name itself. Two shapes exist:
 *
 *   linear   every block is a plain record chain, scanned front to back.
 *   htree    block 0 carries a hashed index (the "dx root"); its entries
 *            point at leaf blocks that hold the plain record chains. The
 *            index hides itself behind the ".." entry's record length, so
 *            a naive linear scan of the leaves alone would miss nothing
 *            important - but the index blocks themselves are not chains, so
 *            we walk the tree instead.
 *
 * The inode flag EXT4_INDEX_FL (0x1000) marks an htree directory; a linear
 * one never has it. Writing (creat/mkdir/unlink) is implemented for linear
 * directories only; htree directories are read correctly but converting a
 * growing directory to htree is the next milestone.
 */

#include "kernel.h"
#include "blkdev.h"
#include "ext4.h"
#include "ext4_int.h"
#include "ext4_le.h"

/* Separate bounce buffer from inode.c's g_blk: the extent mapper there uses
 * g_blk while we hold a directory block in g_dirblk, and the two must not
 * clobber each other. */
static UINT8 g_dirblk[EXT4_BLOCK_MAX];

/* The inode flag that says a directory is indexed (htree). */
#define EXT4_INDEX_FL 0x1000u

/* Per-entry callback. Returns 1 to stop the walk early. */
typedef int (*ext4_dirent_fn)(void *arg, UINT32 ino, const char *name,
                              UINT8 type);

/* ---- name helpers ------------------------------------------------------ */

static UINT32 dir_name_len(const char *s) {
  UINT32 n = 0;

  if (s == NULL) {
    return 0;
  }
  while (s[n] != '\0' && n < EXT4_NAME_MAX) {
    n++;
  }
  return n;
}

static int dir_name_eq(const char *a, const char *b) {
  UINT32 i = 0;

  if (a == NULL || b == NULL) {
    return 0;
  }
  while (a[i] != '\0' && b[i] != '\0') {
    if (a[i] != b[i]) {
      return 0;
    }
    i++;
  }
  return (a[i] == '\0' && b[i] == '\0') ? 1 : 0;
}

/* ---- reading one directory block -------------------------------------- */

static void dir_block_entries(ext4_fs_t *fs, const UINT8 *buf,
                              ext4_dirent_fn cb, void *arg, int *stop) {
  UINT32 off = 0;
  UINT32 bs = fs->block_size;

  while (off + 8u <= bs && !*stop) {
    UINT32 inode = ext4_rd32(buf + off + 0);
    UINT32 rec_len = ext4_rd16(buf + off + 4);
    UINT8  name_len = buf[off + 6];
    UINT8  type = buf[off + 7];
    char   name[EXT4_NAME_MAX + 1u];
    UINT32 i;

    if (rec_len < 8u) {
      break; /* corrupt: stop rather than loop forever */
    }
    if (inode != 0u) {
      for (i = 0; i < name_len && i < EXT4_NAME_MAX; i++) {
        name[i] = (char)buf[off + 8u + i];
      }
      name[name_len] = '\0';
      if (cb(arg, inode, name, type) != 0) {
        *stop = 1;
      }
    }
    off += rec_len;
  }
}

/* ---- the htree index --------------------------------------------------- */

/* Walk the dx index whose root is `lblk`, descending `levels` more index
 * levels. Leaf blocks are handed to dir_block_entries. */
static void dir_dx_walk(ext4_fs_t *fs, const ext4_inode_t *dir, UINT32 lblk,
                        UINT8 levels, ext4_dirent_fn cb, void *arg,
                        int *stop) {
  UINT64 phys = 0;
  const UINT8 *base;
  UINT32 h;
  UINT32 count;
  UINT32 i;

  if (*stop) {
    return;
  }
  if (ext4_map_block(fs, dir, lblk, &phys) != 1) {
    return; /* hole or error: skip */
  }
  if (ext4_blk_read(fs, phys, g_dirblk) != 0) {
    return;
  }
  if (levels == 0u) {
    dir_block_entries(fs, g_dirblk, cb, arg, stop);
    return;
  }
  /* An index node: the countlimit overlays the first dx_entry. The root
   * sits at offset 32 (after "." / ".." and the dx_root_info); every other
   * node is a single 8-byte fake dirent followed by the entries. */
  base = (lblk == 0u) ? g_dirblk + 32u : g_dirblk + 8u;
  h = ext4_rd32(base);
  count = (h >> 16) & 0xFFFFu;
  if (count == 0u || count > fs->block_size / 8u) {
    return; /* implausible index: bail */
  }
  for (i = 1; i < count && !*stop; i++) {
    const UINT8 *e = base + (UINT32)(8u * i);
    UINT32 child = ext4_rd32(e + 4);

    dir_dx_walk(fs, dir, child, (UINT8)(levels - 1u), cb, arg, stop);
  }
}

/* Iterate every real entry of `dir_ino`, linear or htree. */
static int dir_each_entry(ext4_fs_t *fs, UINT32 dir_ino, ext4_dirent_fn cb,
                          void *arg) {
  ext4_inode_t dir;
  int          stop = 0;
  UINT64       size;
  UINT32       nblocks;
  UINT32       lblk;

  if (ext4_inode_read(fs, dir_ino, &dir) != 0) {
    return -1;
  }
  size = dir.size;
  nblocks = (UINT32)((size + fs->block_size - 1u) / fs->block_size);
  if (nblocks == 0u) {
    return 0;
  }
  if ((dir.flags & EXT4_INDEX_FL) != 0u) {
    UINT64 phys = 0;
    UINT8  levels;

    if (ext4_map_block(fs, &dir, 0, &phys) != 1) {
      return -1;
    }
    if (ext4_blk_read(fs, phys, g_dirblk) != 0) {
      return -1;
    }
    levels = g_dirblk[30]; /* dx_root_info.indirect_levels at offset 30 */
    dir_dx_walk(fs, &dir, 0, (UINT8)(levels + 1u), cb, arg, &stop);
    return 0;
  }
  for (lblk = 0; lblk < nblocks && !stop; lblk++) {
    UINT64 phys = 0;

    if (ext4_map_block(fs, &dir, lblk, &phys) != 1) {
      continue;
    }
    if (ext4_blk_read(fs, phys, g_dirblk) != 0) {
      return -1;
    }
    dir_block_entries(fs, g_dirblk, cb, arg, &stop);
  }
  return 0;
}

/* ---- lookup ------------------------------------------------------------ */

typedef struct {
  const char *name;
  UINT32      found;
} lookup_ctx_t;

static int dir_lookup_cb(void *arg, UINT32 ino, const char *name, UINT8 type) {
  lookup_ctx_t *c = (lookup_ctx_t *)arg;

  (void)type;
  if (dir_name_eq(name, c->name)) {
    c->found = ino;
    return 1;
  }
  return 0;
}

UINT32 ext4_lookup(ext4_fs_t *fs, UINT32 dir_ino, const char *name) {
  lookup_ctx_t ctx;

  ctx.name = name;
  ctx.found = 0;
  if (fs == NULL || name == NULL) {
    return 0;
  }
  if (dir_each_entry(fs, dir_ino, dir_lookup_cb, &ctx) != 0) {
    return 0;
  }
  return ctx.found;
}

/* ---- readdir ----------------------------------------------------------- */

typedef struct {
  void (*cb)(void *arg, UINT32 ino, const char *name);
  void *arg;
} readdir_ctx_t;

static int dir_readdir_cb(void *arg, UINT32 ino, const char *name,
                          UINT8 type) {
  readdir_ctx_t *c = (readdir_ctx_t *)arg;

  (void)type;
  c->cb(c->arg, ino, name);
  return 0;
}

int ext4_readdir(ext4_fs_t *fs, UINT32 dir_ino,
                 void (*cb)(void *arg, UINT32 ino, const char *name),
                 void *arg) {
  readdir_ctx_t ctx;

  if (fs == NULL || cb == NULL) {
    return -1;
  }
  ctx.cb = cb;
  ctx.arg = arg;
  return dir_each_entry(fs, dir_ino, dir_readdir_cb, &ctx);
}

/* ---- writing: add / remove a directory entry --------------------------- */

/* Round a record length up to the 4-byte boundary ext4 requires. */
static UINT32 dir_rec_len(UINT32 name_len) {
  return (8u + name_len + 3u) & ~(UINT32)3u;
}

/* Add name->ino (file type `type`) to directory `dir_ino`. Allocates a new
 * directory block when the existing ones are full. */
static int dir_add_entry(ext4_fs_t *fs, UINT32 dir_ino, const char *name,
                         UINT32 ino, UINT8 type) {
  ext4_inode_t dir;
  UINT64       size;
  UINT32       nblocks;
  UINT32       lblk;
  UINT32       namelen;
  UINT32       needed;

  if (ext4_inode_read(fs, dir_ino, &dir) != 0) {
    return -1;
  }
  if ((dir.flags & EXT4_INDEX_FL) != 0u) {
    return -1; /* htree write not implemented yet */
  }
  namelen = dir_name_len(name);
  if (namelen == 0u || namelen > EXT4_NAME_MAX) {
    return -1;
  }
  needed = dir_rec_len(namelen);
  size = dir.size;
  nblocks = (UINT32)((size + fs->block_size - 1u) / fs->block_size);
  if (nblocks == 0u) {
    nblocks = 1u;
  }

  /* Look for slack inside an existing block. */
  for (lblk = 0; lblk < nblocks; lblk++) {
    UINT64 phys = 0;
    UINT32 off = 0;

    if (ext4_map_block(fs, &dir, lblk, &phys) != 1) {
      continue;
    }
    if (ext4_blk_read(fs, phys, g_dirblk) != 0) {
      return -1;
    }
    while (off + 8u <= fs->block_size) {
      UINT32 rec = ext4_rd16(g_dirblk + off + 4);
      UINT8  nlen = g_dirblk[off + 6];
      UINT32 used = dir_rec_len((UINT32)nlen);
      UINT32 slack = rec - used;

      if (rec < 8u) {
        break;
      }
      if (slack >= needed) {
        UINT32 noff = off + used;

        ext4_wr16(g_dirblk + off + 4, (UINT16)used);
        ext4_wr32(g_dirblk + noff + 0, ino);
        ext4_wr16(g_dirblk + noff + 4, (UINT16)(rec - used));
        g_dirblk[noff + 6] = (UINT8)namelen;
        g_dirblk[noff + 7] = type;
        memcpy(g_dirblk + noff + 8u, name, namelen);
        if (ext4_blk_write(fs, phys, g_dirblk) != 0) {
          return -1;
        }
        return 0;
      }
      off += rec;
    }
  }

  /* No room: append a fresh block holding one empty record, then the new
   * entry. */
  {
    UINT64 newblk = 0;
    UINT32 rec = fs->block_size;
    UINT32 used = needed;
    UINT32 noff = 0;

    if (ext4_alloc_block(fs, &newblk) != 0) {
      return -1;
    }
    memset(g_dirblk, 0, fs->block_size);
    ext4_wr32(g_dirblk + used + 0, 0);                 /* empty tail */
    ext4_wr16(g_dirblk + used + 4, (UINT16)(rec - used));
    ext4_wr32(g_dirblk + noff + 0, ino);              /* new entry */
    ext4_wr16(g_dirblk + noff + 4, (UINT16)used);
    g_dirblk[noff + 6] = (UINT8)namelen;
    g_dirblk[noff + 7] = type;
    memcpy(g_dirblk + noff + 8u, name, namelen);
    if (ext4_blk_write(fs, newblk, g_dirblk) != 0) {
      (void)ext4_free_block(fs, newblk);
      return -1;
    }
    if (ext4_add_extent(fs, dir_ino, &dir, nblocks, 1, newblk) != 0) {
      (void)ext4_free_block(fs, newblk);
      return -1;
    }
    dir.size = (UINT64)(nblocks + 1u) * fs->block_size;
    if (ext4_inode_write(fs, dir_ino, &dir) != 0) {
      return -1;
    }
    return 0;
  }
}

UINT32 ext4_creat(ext4_fs_t *fs, UINT32 dir_ino, const char *name) {
  UINT32       ino = 0;
  ext4_inode_t in;

  if (fs == NULL || name == NULL) {
    return 0;
  }
  if (ext4_alloc_inode(fs, &ino) != 0) {
    return 0;
  }
  memset(&in, 0, sizeof(in));
  in.mode = 0x81A4u; /* S_IFREG | 0644 */
  in.links = 1;
  in.flags = EXT4_INODE_FLAG_EXTENTS;
  in.size = 0;
  in.extra_isize = 32;
  /* Start with an empty extent tree so a hole maps cleanly on the first
   * write instead of being treated as a corrupt inode. */
  ext4_wr16(in.i_block + 0, EXT4_EXTENT_MAGIC);
  ext4_wr16(in.i_block + 2, 0); /* zero extents */
  ext4_wr16(in.i_block + 4, 4); /* room for four */
  ext4_wr16(in.i_block + 6, 0); /* depth: a leaf */
  if (ext4_inode_write(fs, ino, &in) != 0) {
    (void)ext4_free_inode(fs, ino);
    return 0;
  }
  if (dir_add_entry(fs, dir_ino, name, ino, EXT4_FT_REG_FILE) != 0) {
    (void)ext4_free_inode(fs, ino);
    return 0;
  }
  return ino;
}

UINT32 ext4_mkdir(ext4_fs_t *fs, UINT32 dir_ino, const char *name) {
  UINT32       ino = 0;
  UINT64       blk = 0;
  ext4_inode_t in;
  UINT8        eb[60];

  if (fs == NULL || name == NULL) {
    return 0;
  }
  if (ext4_alloc_inode(fs, &ino) != 0) {
    return 0;
  }
  if (ext4_alloc_block(fs, &blk) != 0) {
    (void)ext4_free_inode(fs, ino);
    return 0;
  }
  memset(g_dirblk, 0, fs->block_size);
  ext4_wr32(g_dirblk + 0, ino);                       /* "." -> self */
  ext4_wr16(g_dirblk + 4, 12);
  g_dirblk[6] = 1;
  g_dirblk[7] = EXT4_FT_DIR;
  g_dirblk[8] = '.';
  ext4_wr32(g_dirblk + 12, dir_ino);                 /* ".." -> parent */
  ext4_wr16(g_dirblk + 16, (UINT16)(fs->block_size - 12u));
  g_dirblk[18] = 2;
  g_dirblk[19] = EXT4_FT_DIR;
  g_dirblk[20] = '.';
  g_dirblk[21] = '.';
  if (ext4_blk_write(fs, blk, g_dirblk) != 0) {
    (void)ext4_free_block(fs, blk);
    (void)ext4_free_inode(fs, ino);
    return 0;
  }
  memset(&in, 0, sizeof(in));
  in.mode = 0x41EDu; /* S_IFDIR | 0755 */
  in.links = 2;
  in.flags = EXT4_INODE_FLAG_EXTENTS;
  in.size = fs->block_size;
  in.blocks = fs->block_size / 512u;
  memset(eb, 0, sizeof(eb));
  ext4_wr16(eb + 0, EXT4_EXTENT_MAGIC);
  ext4_wr16(eb + 2, 1);   /* one extent */
  ext4_wr16(eb + 4, 4);   /* room for four */
  ext4_wr16(eb + 6, 0);   /* depth: a leaf */
  ext4_wr32(eb + 12, 0);  /* first logical block */
  ext4_wr16(eb + 16, 1);  /* length 1 */
  ext4_wr16(eb + 18, (UINT16)(blk >> 32));
  ext4_wr32(eb + 20, (UINT32)(blk & 0xFFFFFFFFu));
  memcpy(in.i_block, eb, sizeof(in.i_block));
  if (ext4_inode_write(fs, ino, &in) != 0) {
    (void)ext4_free_block(fs, blk);
    (void)ext4_free_inode(fs, ino);
    return 0;
  }
  if (dir_add_entry(fs, dir_ino, name, ino, EXT4_FT_DIR) != 0) {
    (void)ext4_free_inode(fs, ino);
    return 0;
  }
  return ino;
}

int ext4_unlink(ext4_fs_t *fs, UINT32 dir_ino, const char *name) {
  ext4_inode_t dir;
  UINT64       size;
  UINT32       nblocks;
  UINT32       lblk;
  UINT32       found_ino = 0;

  if (fs == NULL || name == NULL) {
    return -1;
  }
  if (ext4_inode_read(fs, dir_ino, &dir) != 0) {
    return -1;
  }
  if ((dir.flags & EXT4_INDEX_FL) != 0u) {
    return -1; /* htree write not implemented yet */
  }
  size = dir.size;
  nblocks = (UINT32)((size + fs->block_size - 1u) / fs->block_size);
  for (lblk = 0; lblk < nblocks && found_ino == 0u; lblk++) {
    UINT64 phys = 0;
    UINT32 off = 0;

    if (ext4_map_block(fs, &dir, lblk, &phys) != 1) {
      continue;
    }
    if (ext4_blk_read(fs, phys, g_dirblk) != 0) {
      return -1;
    }
    while (off + 8u <= fs->block_size) {
      UINT32 inode = ext4_rd32(g_dirblk + off);
      UINT32 rec = ext4_rd16(g_dirblk + off + 4);
      UINT8  nlen = g_dirblk[off + 6];
      char   ename[EXT4_NAME_MAX + 1u];
      UINT32 i;

      if (rec < 8u) {
        break;
      }
      if (inode != 0u) {
        for (i = 0; i < nlen && i < EXT4_NAME_MAX; i++) {
          ename[i] = (char)g_dirblk[off + 8u + i];
        }
        ename[nlen] = '\0';
        if (dir_name_eq(ename, name)) {
          found_ino = inode;
          ext4_wr32(g_dirblk + off, 0); /* mark deleted */
          if (ext4_blk_write(fs, phys, g_dirblk) != 0) {
            return -1;
          }
          break;
        }
      }
      off += rec;
    }
  }
  if (found_ino == 0u) {
    return -1;
  }
  (void)ext4_inode_truncate(fs, found_ino);
  (void)ext4_free_inode(fs, found_ino);
  return 0;
}
