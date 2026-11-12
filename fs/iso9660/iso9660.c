/**
 * Novus OS - read-only ISO 9660 / Joliet volume reader.
 */
#include "iso9660.h"

static UINT32 le32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static int str_eq(const char *a, const char *b) {
  UINTN i = 0;

  for (;; i++) {
    if (a[i] != b[i]) {
      return 0;
    }
    if (a[i] == '\0') {
      return 1;
    }
  }
}

static int dev_read_bytes(iso9660_fs_t *fs, UINT64 byte_off, void *dst,
                          UINTN len) {
  UINT8  *out = (UINT8 *)dst;
  UINT32  block_bytes = fs->spb * 512u;

  while (len > 0u) {
    UINT64 sec = byte_off / 512u;
    UINT64 aligned = sec & ~((UINT64)fs->spb - 1u);
    UINTN  inner = (UINTN)(byte_off - aligned * 512u);
    UINTN  avail = block_bytes - inner;
    UINTN  chunk = (len < avail) ? len : avail;

    if (blkdev_read(fs->dev, aligned, fs->spb, fs->bounce) != 0) {
      return -1;
    }
    memcpy(out, fs->bounce + inner, chunk);
    byte_off += chunk;
    out += chunk;
    len -= chunk;
  }
  return 0;
}

static int read_sector(iso9660_fs_t *fs, UINT32 lba, UINT8 *buf) {
  return dev_read_bytes(fs, (UINT64)lba * ISO9660_SECTOR, buf,
                        ISO9660_SECTOR);
}

int iso9660_mount(iso9660_fs_t *fs, blkdev_t *dev) {
  UINT8  vd[ISO9660_SECTOR];
  UINT32 prim_lba = 0;
  UINT32 prim_size = 0;
  UINT32 jol_lba = 0;
  UINT32 jol_size = 0;
  UINT32 lba;

  if (fs == NULL || dev == NULL) {
    return -1;
  }
  memset(fs, 0, sizeof(*fs));
  fs->dev = dev;
  fs->spb = blkdev_sectors_per_block(dev);
  if (fs->spb == 0u || fs->spb > BLKDEV_BLOCK_MAX / 512u) {
    return -1;
  }
  for (lba = 16u; lba < 48u; lba++) {
    if (read_sector(fs, lba, vd) != 0 ||
        memcmp(vd + 1, "CD001", 5) != 0) {
      break;
    }
    if (vd[0] == 255u) {
      break;
    }
    if (vd[0] == 1u && prim_lba == 0u) {
      prim_lba = le32(vd + 158);
      prim_size = le32(vd + 166);
    }
    if (vd[0] == 2u && memcmp(vd + 88, "%/E", 3) == 0 &&
        jol_lba == 0u) {
      jol_lba = le32(vd + 158);
      jol_size = le32(vd + 166);
    }
  }
  if (jol_lba != 0u) {
    fs->joliet = 1u;
    fs->root_lba = jol_lba;
    fs->root_size = jol_size;
  } else if (prim_lba != 0u) {
    fs->root_lba = prim_lba;
    fs->root_size = prim_size;
  } else {
    return -1;
  }
  if (fs->root_size == 0u) {
    fs->root_size = ISO9660_SECTOR;
  }
  fs->used_nodes = 2u;
  fs->nodes[ISO9660_ROOT_INO].used = 1u;
  fs->nodes[ISO9660_ROOT_INO].is_dir = 1u;
  fs->nodes[ISO9660_ROOT_INO].parent = 0u;
  fs->nodes[ISO9660_ROOT_INO].lba = fs->root_lba;
  fs->nodes[ISO9660_ROOT_INO].size = fs->root_size;
  fs->nodes[ISO9660_ROOT_INO].name[0] = '\0';
  fs->mounted = 1u;
  return 0;
}

void iso9660_unmount(iso9660_fs_t *fs) {
  if (fs != NULL) {
    fs->mounted = 0u;
    fs->dev = NULL;
  }
}

/* Decode a directory record's identifier. Joliet uses UCS-2BE; non-ASCII
 * characters are rendered as '?' because the VFS ABI is byte-oriented. */
static void decode_name(iso9660_fs_t *fs, const UINT8 *id, UINT32 len,
                        char *out) {
  UINT32 i;
  UINT32 n = 0;

  if (fs->joliet != 0u) {
    for (i = 0; i + 1u < len && n + 1u < VFS_NAME_MAX; i += 2u) {
      out[n++] = (id[i] == 0u && id[i + 1u] < 0x80u)
                     ? (char)id[i + 1u]
                     : '?';
    }
  } else {
    for (i = 0; i < len && n + 1u < VFS_NAME_MAX; i++) {
      out[n++] = (id[i] < 0x80u) ? (char)id[i] : '?';
    }
  }
  /* ISO 9660 primary names carry a ";1" version suffix. */
  if (n >= 2u && out[n - 2u] == ';' && out[n - 1u] == '1') {
    n -= 2u;
  }
  out[n] = '\0';
}

static int scan_dir(iso9660_fs_t *fs, UINT32 dir) {
  UINT8  sec[ISO9660_SECTOR];
  UINT32 lba;
  UINT32 total;
  UINT32 s;

  if (dir == 0u || dir >= ISO9660_MAX_NODES || !fs->nodes[dir].used ||
      !fs->nodes[dir].is_dir) {
    return -1;
  }
  if (fs->nodes[dir].scanned != 0u) {
    return 0;
  }
  fs->nodes[dir].scanned = 1u;
  lba = fs->nodes[dir].lba;
  total = (fs->nodes[dir].size + ISO9660_SECTOR - 1u) / ISO9660_SECTOR;
  if (total == 0u) {
    total = 1u;
  }
  for (s = 0; s < total; s++) {
    UINT32 off = 0;

    if (read_sector(fs, lba + s, sec) != 0) {
      return -1;
    }
    while (off + 33u <= ISO9660_SECTOR) {
      UINT32       rec = sec[off];
      UINT32       nlen;
      UINT32       extent;
      UINT32       size;
      UINT8        flags;
      char         name[VFS_NAME_MAX + 1u];
      UINT32       ino;
      iso9660_node_t *nd;

      if (rec == 0u) {
        break; /* rest of this sector is padding */
      }
      if (rec < 33u || off + rec > ISO9660_SECTOR) {
        break;
      }
      nlen = sec[off + 32u];
      if (off + 33u + nlen > ISO9660_SECTOR) {
        break;
      }
      if (nlen == 1u && (sec[off + 33u] == 0x00u ||
                         sec[off + 33u] == 0x01u)) {
        off += rec;
        continue; /* "." and ".." */
      }
      extent = le32(sec + off + 2u);
      size = le32(sec + off + 10u);
      flags = sec[off + 25u];
      decode_name(fs, sec + off + 33u, nlen, name);
      if (name[0] != '\0' && fs->used_nodes < ISO9660_MAX_NODES) {
        ino = fs->used_nodes++;
        nd = &fs->nodes[ino];
        nd->used = 1u;
        nd->parent = dir;
        nd->lba = extent;
        nd->size = size;
        nd->is_dir = (UINT8)((flags & 0x02u) != 0u);
        nd->scanned = 0u;
        {
          UINTN k;
          for (k = 0; k + 1u < sizeof(nd->name) && name[k] != '\0'; k++) {
            nd->name[k] = name[k];
          }
          nd->name[k] = '\0';
        }
      }
      off += rec;
    }
  }
  return 0;
}

UINT32 iso9660_lookup(iso9660_fs_t *fs, UINT32 dir, const char *name) {
  UINT32 i;

  if (fs == NULL || !fs->mounted || name == NULL ||
      scan_dir(fs, dir) != 0) {
    return 0;
  }
  for (i = 1; i < fs->used_nodes; i++) {
    if (fs->nodes[i].used && fs->nodes[i].parent == dir &&
        str_eq(fs->nodes[i].name, name)) {
      return i;
    }
  }
  return 0;
}

int iso9660_readdir(iso9660_fs_t *fs, UINT32 dir, vfs_readdir_cb cb,
                    void *arg) {
  UINT32 i;

  if (fs == NULL || cb == NULL || scan_dir(fs, dir) != 0) {
    return -1;
  }
  for (i = 1; i < fs->used_nodes; i++) {
    if (fs->nodes[i].used && fs->nodes[i].parent == dir) {
      cb(arg, i, fs->nodes[i].name);
    }
  }
  return 0;
}

int iso9660_stat(iso9660_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode) {
  if (fs == NULL || ino == 0u || ino >= fs->used_nodes ||
      !fs->nodes[ino].used) {
    return -1;
  }
  if (size != NULL) {
    *size = fs->nodes[ino].is_dir ? 0u : (UINT64)fs->nodes[ino].size;
  }
  if (mode != NULL) {
    *mode = fs->nodes[ino].is_dir ? VFS_MODE_DIR : VFS_MODE_FILE;
  }
  return 0;
}

INT64 iso9660_read(iso9660_fs_t *fs, UINT32 ino, UINT64 off, void *buf,
                   UINT32 len) {
  if (fs == NULL || ino == 0u || ino >= fs->used_nodes ||
      !fs->nodes[ino].used || fs->nodes[ino].is_dir || buf == NULL) {
    return -1;
  }
  if (off >= (UINT64)fs->nodes[ino].size) {
    return 0;
  }
  if ((UINT64)len > (UINT64)fs->nodes[ino].size - off) {
    len = (UINT32)((UINT64)fs->nodes[ino].size - off);
  }
  if (dev_read_bytes(fs, (UINT64)fs->nodes[ino].lba * ISO9660_SECTOR + off,
                     buf, len) != 0) {
    return -1;
  }
  return (INT64)len;
}
