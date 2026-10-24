/**
 * Novus OS - ext4 inodes and the extent tree (M12).
 *
 * An inode stores either classic block pointers or, when the EXTENTS
 * feature is on, a tree of extents: runs of blocks that belong together.
 * The tree starts in the inode itself (i_block) and grows into index
 * blocks when a file needs more than four runs.
 *
 * The layout of a tree node:
 *
 *   header   magic 0xF30A, entries, max, depth, generation
 *   leaf     depth 0: one extent per entry
 *            extent: logical block, length, physical block (64 bit)
 *   index    depth > 0: one child pointer per entry
 *            index: logical block, child block (64 bit)
 */

#include "kernel.h"
#include "blkdev.h"
#include "ext4.h"
#include "ext4_int.h"
#include "ext4_le.h"

static UINT8 g_blk[EXT4_BLOCK_MAX];

/* ---- inode ------------------------------------------------------------- */

int ext4_inode_read(ext4_fs_t *fs, UINT32 ino, ext4_inode_t *out) {
  UINT64 blk = ext4_inode_block(fs, ino);
  UINT32 index = (ino - 1u) % fs->inodes_per_group;
  UINT64 off = (UINT64)index * fs->inode_size;
  UINT32 within = (UINT32)(off % fs->block_size);
  const UINT8 *p;

  if (ino == 0u || blk == 0u) {
    return -1;
  }
  if (ext4_blk_read(fs, blk, g_blk) != 0) {
    return -1;
  }
  p = g_blk + within;
  memset(out, 0, sizeof(*out));
  out->mode = ext4_rd16(p + 0x00);
  out->links = ext4_rd16(p + 0x1A);
  out->blocks = ext4_rd32(p + 0x1C);
  out->flags = ext4_rd32(p + 0x20);
  out->size = (UINT64)ext4_rd32(p + 0x04);
  if ((fs->feature_ro_compat & EXT4_RO_COMPAT_LARGE_FILE) != 0u) {
    out->size |= (UINT64)ext4_rd32(p + 0x6C) << 32;
  }
  memcpy(out->i_block, p + 0x28, sizeof(out->i_block));
  if (fs->inode_size > 128u &&
      (fs->feature_ro_compat & EXT4_RO_COMPAT_EXTRA_ISIZE) != 0u) {
    out->extra_isize = ext4_rd16(p + 0x80);
  }
  return 0;
}

int ext4_inode_write(ext4_fs_t *fs, UINT32 ino, const ext4_inode_t *in) {
  UINT64 blk = ext4_inode_block(fs, ino);
  UINT32 index = (ino - 1u) % fs->inodes_per_group;
  UINT64 off = (UINT64)index * fs->inode_size;
  UINT32 within = (UINT32)(off % fs->block_size);
  UINT8 *p;

  if (ino == 0u || blk == 0u) {
    return -1;
  }
  if (ext4_blk_read(fs, blk, g_blk) != 0) {
    return -1;
  }
  p = g_blk + within;
  ext4_wr16(p + 0x00, in->mode);
  ext4_wr16(p + 0x02, 0);
  ext4_wr32(p + 0x04, (UINT32)(in->size & 0xFFFFFFFFu));
  ext4_wr16(p + 0x18, 0);
  ext4_wr16(p + 0x1A, in->links);
  ext4_wr32(p + 0x1C, in->blocks);
  ext4_wr32(p + 0x20, in->flags);
  memcpy(p + 0x28, in->i_block, sizeof(in->i_block));
  if (fs->inode_size > 128u) {
    ext4_wr16(p + 0x80, in->extra_isize);
    ext4_wr32(p + 0x6C, (UINT32)(in->size >> 32));
  }
  return ext4_blk_write(fs, blk, g_blk);
}

int ext4_stat(ext4_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode) {
  ext4_inode_t in;

  if (ext4_inode_read(fs, ino, &in) != 0) {
    return -1;
  }
  if (size != NULL) {
    *size = in.size;
  }
  if (mode != NULL) {
    *mode = in.mode;
  }
  return 0;
}

/* ---- the extent tree --------------------------------------------------- */

/* Map one logical block to a physical one.
 * Returns 1 when the block is mapped, 0 when it is a hole and -1 on
 * error. */
int ext4_map_block(ext4_fs_t *fs, const ext4_inode_t *in, UINT32 lblk,
                    UINT64 *phys) {
  UINT8  node[60];
  UINT32 depth;
  UINT32 entries;
  UINT32 i;

  *phys = 0;
  memcpy(node, in->i_block, sizeof(node));
  depth = 0;
  for (;;) {
    if (ext4_rd16(node) != EXT4_EXTENT_MAGIC) {
      return -1;
    }
    entries = ext4_rd16(node + 2);
    depth = ext4_rd16(node + 6);
    if (depth == 0u) {
      for (i = 0; i < entries; i++) {
        const UINT8 *e = node + 12u + i * 12u;
        UINT32 first = ext4_rd32(e);
        UINT32 len = ext4_rd16(e + 4);
        UINT64 start;

        if (len > 32768u) {
          len = len & 0x7FFFu; /* the high bit marks an unwritten extent */
        }
        if (lblk < first || lblk >= first + len) {
          continue;
        }
        start = ((UINT64)ext4_rd16(e + 6) << 32) | ext4_rd32(e + 8);
        *phys = start + (lblk - first);
        return 1;
      }
      return 0; /* a hole */
    }
    /* An index node: pick the child whose range covers the block. */
    {
      UINT32 chosen = 0;
      int    found = 0;

      for (i = 0; i < entries; i++) {
        const UINT8 *e = node + 12u + i * 12u;

        if (ext4_rd32(e) <= lblk) {
          chosen = i;
          found = 1;
        }
      }
      if (!found) {
        return 0;
      }
      {
        const UINT8 *e = node + 12u + chosen * 12u;
        UINT64 child = ((UINT64)ext4_rd16(e + 8) << 32) | ext4_rd32(e + 4);

        if (ext4_blk_read(fs, child, g_blk) != 0) {
          return -1;
        }
        memcpy(node, g_blk, sizeof(node));
      }
    }
  }
}

/* Insert one extent into a leaf node, keeping the order by logical
 * block. Returns 0 when the node had no room. */
static int ext4_leaf_insert(UINT8 *leaf, UINT32 lblk, UINT32 len,
                            UINT64 phys, UINT32 max_extents) {
  UINT32 entries = ext4_rd16(leaf + 2);
  UINT32 i;
  UINT32 pos;

  if (entries >= max_extents) {
    return -1;
  }
  /* Find the first extent that starts after the new one. */
  pos = entries;
  for (i = 0; i < entries; i++) {
    if (ext4_rd32(leaf + 12u + i * 12u) > lblk) {
      pos = i;
      break;
    }
  }
  /* Move the tail one slot to the right. */
  for (i = entries; i > pos; i--) {
    memcpy(leaf + 12u + i * 12u, leaf + 12u + (i - 1u) * 12u, 12u);
  }
  {
    UINT8 *e = leaf + 12u + pos * 12u;

    ext4_wr32(e + 0, lblk);
    ext4_wr16(e + 4, (UINT16)len);
    ext4_wr16(e + 6, (UINT16)(phys >> 32));
    ext4_wr32(e + 8, (UINT32)(phys & 0xFFFFFFFFu));
  }
  ext4_wr16(leaf + 2, (UINT16)(entries + 1u));
  return 0;
}

/* Add one extent to an inode, growing the tree when the inode's own
 * node runs out of room. */
int ext4_add_extent(ext4_fs_t *fs, UINT32 ino, ext4_inode_t *in,
                    UINT32 lblk, UINT32 len, UINT64 phys) {
  UINT8 root[60];
  UINT32 depth;
  UINT32 entries;

  (void)ino;

  memcpy(root, in->i_block, sizeof(root));
  if (ext4_rd16(root) != EXT4_EXTENT_MAGIC) {
    /* No tree yet: start one. */
    memset(root, 0, sizeof(root));
    ext4_wr16(root + 0, EXT4_EXTENT_MAGIC);
    ext4_wr16(root + 2, 0);
    ext4_wr16(root + 4, EXT4_EXTENTS_PER_LEAF);
    ext4_wr16(root + 6, 0);
  }
  depth = ext4_rd16(root + 6);
  entries = ext4_rd16(root + 2);

  if (depth == 0u) {
    if (entries < EXT4_EXTENTS_PER_LEAF) {
      if (ext4_leaf_insert(root, lblk, len, phys, EXT4_EXTENTS_PER_LEAF) != 0) {
        return -1;
      }
      memcpy(in->i_block, root, sizeof(root));
      return 0;
    }
    /* The inode's own node is full: move what is there into a leaf and
     * turn the inode into an index. */
    {
      UINT64 leaf_block = 0;
      UINT32 i;

      if (ext4_alloc_block(fs, &leaf_block) != 0) {
        return -1;
      }
      memset(g_blk, 0, fs->block_size);
      ext4_wr16(g_blk + 0, EXT4_EXTENT_MAGIC);
      ext4_wr16(g_blk + 2, entries);
      ext4_wr16(g_blk + 4, (UINT16)((fs->block_size - 12u) / 12u));
      ext4_wr16(g_blk + 6, 0);
      for (i = 0; i < entries; i++) {
        memcpy(g_blk + 12u + i * 12u, root + 12u + i * 12u, 12u);
      }
      if (ext4_blk_write(fs, leaf_block, g_blk) != 0) {
        (void)ext4_free_block(fs, leaf_block);
        return -1;
      }
      memset(root, 0, sizeof(root));
      ext4_wr16(root + 0, EXT4_EXTENT_MAGIC);
      ext4_wr16(root + 2, 1);
      ext4_wr16(root + 4, EXT4_EXTENTS_PER_LEAF);
      ext4_wr16(root + 6, 1);
      ext4_wr32(root + 12, 0); /* the first logical block of the child */
      ext4_wr32(root + 16, (UINT32)(leaf_block & 0xFFFFFFFFu));
      ext4_wr16(root + 20, (UINT16)(leaf_block >> 32));
      memcpy(in->i_block, root, sizeof(root));
      return 0;
    }
  }

  /* Depth 1: the inode holds index entries, each pointing at a leaf. */
  {
    UINT32 chosen = 0;
    UINT32 i;
    int    found = 0;
    UINT64 leaf_block;

    for (i = 0; i < entries; i++) {
      const UINT8 *e = root + 12u + i * 12u;

      if (ext4_rd32(e) <= lblk) {
        chosen = i;
        found = 1;
      }
    }
    if (!found) {
      return -1;
    }
    {
      const UINT8 *e = root + 12u + chosen * 12u;

      leaf_block = ((UINT64)ext4_rd16(e + 8) << 32) | ext4_rd32(e + 4);
    }
    if (ext4_blk_read(fs, leaf_block, g_blk) != 0) {
      return -1;
    }
    if (ext4_leaf_insert(g_blk, lblk, len, phys,
                         (fs->block_size - 12u) / 12u) == 0) {
      if (ext4_blk_write(fs, leaf_block, g_blk) != 0) {
        return -1;
      }
      memcpy(in->i_block, root, sizeof(root));
      return 0;
    }
    /* The leaf is full: a new one needs a free index entry. */
    if (entries >= EXT4_EXTENTS_PER_LEAF) {
      return -1;
    }
    {
      UINT64 fresh = 0;
      UINT8 *slot;

      if (ext4_alloc_block(fs, &fresh) != 0) {
        return -1;
      }
      memset(g_blk, 0, fs->block_size);
      ext4_wr16(g_blk + 0, EXT4_EXTENT_MAGIC);
      ext4_wr16(g_blk + 2, 1);
      ext4_wr16(g_blk + 4, (UINT16)((fs->block_size - 12u) / 12u));
      ext4_wr16(g_blk + 6, 0);
      {
        UINT8 *e = g_blk + 12u;

        ext4_wr32(e + 0, lblk);
        ext4_wr16(e + 4, (UINT16)len);
        ext4_wr16(e + 6, (UINT16)(phys >> 32));
        ext4_wr32(e + 8, (UINT32)(phys & 0xFFFFFFFFu));
      }
      if (ext4_blk_write(fs, fresh, g_blk) != 0) {
        (void)ext4_free_block(fs, fresh);
        return -1;
      }
      slot = root + 12u + entries * 12u;
      ext4_wr32(slot + 0, lblk);
      ext4_wr32(slot + 4, (UINT32)(fresh & 0xFFFFFFFFu));
      ext4_wr16(slot + 8, (UINT16)(fresh >> 32));
      ext4_wr16(root + 2, (UINT16)(entries + 1u));
      memcpy(in->i_block, root, sizeof(root));
      return 0;
    }
  }
}

/* ---- file data --------------------------------------------------------- */

INT64 ext4_read(ext4_fs_t *fs, UINT32 ino, UINT64 off, void *buf, UINT32 len) {
  ext4_inode_t in;
  UINT8       *dst = (UINT8 *)buf;
  UINT32       done = 0;

  if (fs == NULL || buf == NULL) {
    return -1;
  }
  if (ext4_inode_read(fs, ino, &in) != 0) {
    return -1;
  }
  if (off >= in.size) {
    return 0;
  }
  if (off + len > in.size) {
    len = (UINT32)(in.size - off);
  }
  while (done < len) {
    UINT64 pos = off + done;
    UINT32 lblk = (UINT32)(pos / fs->block_size);
    UINT32 within = (UINT32)(pos % fs->block_size);
    UINT32 n = fs->block_size - within;
    UINT64 phys = 0;
    int    rc;

    if (n > len - done) {
      n = len - done;
    }
    rc = ext4_map_block(fs, &in, lblk, &phys);
    if (rc < 0) {
      return -1;
    }
    if (rc == 0) {
      memset(dst + done, 0, n); /* a hole reads as zeros */
    } else {
      if (ext4_blk_read(fs, phys, g_blk) != 0) {
        return -1;
      }
      memcpy(dst + done, g_blk + within, n);
    }
    done += n;
  }
  return (INT64)done;
}

INT64 ext4_write(ext4_fs_t *fs, UINT32 ino, UINT64 off, const void *buf,
                 UINT32 len) {
  ext4_inode_t    in;
  const UINT8    *src = (const UINT8 *)buf;
  UINT32          done = 0;
  UINT32          fresh_blocks = 0;
  int             changed = 0;

  if (fs == NULL || buf == NULL) {
    return -1;
  }
  if (ext4_inode_read(fs, ino, &in) != 0) {
    return -1;
  }
  while (done < len) {
    UINT64 pos = off + done;
    UINT32 lblk = (UINT32)(pos / fs->block_size);
    UINT32 within = (UINT32)(pos % fs->block_size);
    UINT32 n = fs->block_size - within;
    UINT64 phys = 0;
    int    rc;

    if (n > len - done) {
      n = len - done;
    }
    rc = ext4_map_block(fs, &in, lblk, &phys);
    if (rc < 0) {
      return -1;
    }
    if (rc == 0) {
      /* The block has no space yet: give it one and remember it. */
      if (ext4_alloc_block(fs, &phys) != 0) {
        return -1;
      }
      if (ext4_add_extent(fs, ino, &in, lblk, 1, phys) != 0) {
        (void)ext4_free_block(fs, phys);
        return -1;
      }
      fresh_blocks++;
      memset(g_blk, 0, fs->block_size);
    } else if (ext4_blk_read(fs, phys, g_blk) != 0) {
      return -1;
    }
    memcpy(g_blk + within, src + done, n);
    if (ext4_blk_write(fs, phys, g_blk) != 0) {
      return -1;
    }
    done += n;
    changed = 1;
  }
  if (off + len > in.size) {
    in.size = off + len;
    changed = 1;
  }
  if (fresh_blocks != 0u) {
    in.blocks += fresh_blocks * (fs->block_size / 512u);
  }
  if (changed) {
    if (ext4_inode_write(fs, ino, &in) != 0) {
      return -1;
    }
  }
  return (INT64)done;
}

/* Free every data block of an inode and reset its extent tree. Handles the
 * depth-0 and depth-1 trees the writer creates. */
int ext4_inode_truncate(ext4_fs_t *fs, UINT32 ino) {
  ext4_inode_t in;
  UINT8       node[60];
  UINT32      depth;
  UINT32      entries;
  UINT32      i;

  if (ext4_inode_read(fs, ino, &in) != 0) {
    return -1;
  }
  memcpy(node, in.i_block, sizeof(node));
  if (ext4_rd16(node) != EXT4_EXTENT_MAGIC) {
    in.size = 0;
    memset(in.i_block, 0, sizeof(in.i_block));
    return ext4_inode_write(fs, ino, &in);
  }
  depth = ext4_rd16(node + 6);
  if (depth == 0u) {
    entries = ext4_rd16(node + 2);
    for (i = 0; i < entries; i++) {
      const UINT8 *e = node + 12u + i * 12u;
      UINT32 len = ext4_rd16(e + 4);
      UINT64 start = ((UINT64)ext4_rd16(e + 6) << 32) | ext4_rd32(e + 8);
      UINT32 j;

      if (len > 32768u) {
        len = len & 0x7FFFu;
      }
      for (j = 0; j < len; j++) {
        (void)ext4_free_block(fs, start + j);
      }
    }
  } else {
    entries = ext4_rd16(node + 2);
    for (i = 0; i < entries; i++) {
      const UINT8 *e = node + 12u + i * 12u;
      UINT64       leaf = ((UINT64)ext4_rd16(e + 8) << 32) | ext4_rd32(e + 4);
      UINT32       le;
      UINT32       lentries;

      if (ext4_blk_read(fs, leaf, g_blk) != 0) {
        continue;
      }
      if (ext4_rd16(g_blk) != EXT4_EXTENT_MAGIC) {
        (void)ext4_free_block(fs, leaf);
        continue;
      }
      lentries = ext4_rd16(g_blk + 2);
      for (le = 0; le < lentries; le++) {
        const UINT8 *le_e = g_blk + 12u + le * 12u;
        UINT32 len = ext4_rd16(le_e + 4);
        UINT64 start = ((UINT64)ext4_rd16(le_e + 6) << 32) | ext4_rd32(le_e + 8);
        UINT32 j;

        if (len > 32768u) {
          len = len & 0x7FFFu;
        }
        for (j = 0; j < len; j++) {
          (void)ext4_free_block(fs, start + j);
        }
      }
      (void)ext4_free_block(fs, leaf);
    }
  }
  in.size = 0;
  in.blocks = 0;
  memset(in.i_block, 0, sizeof(in.i_block));
  return ext4_inode_write(fs, ino, &in);
}
