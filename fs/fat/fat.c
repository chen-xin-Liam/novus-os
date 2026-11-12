/**
 * Novus OS - read-only FAT12/16/32 volume reader.
 *
 * Transfers are in 512-byte sectors through blkdev_read(); a bounce buffer
 * makes every access a whole hardware block, which is what 4Kn devices
 * need. No allocation: the node table is fixed and a directory is scanned
 * at most once.
 */
#include "fat.h"

static UINT16 le16(const UINT8 *p) {
  return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

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

/* Read `len` bytes at an arbitrary byte offset, aligning each transfer to
 * the device's real logical block size. */
static int dev_read_bytes(fat_fs_t *fs, UINT64 byte_off, void *dst,
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

static UINT32 fat_next(fat_fs_t *fs, UINT32 clus) {
  UINT8  b[4];
  UINT64 off = (UINT64)fs->fat_start * 512u;

  memset(b, 0, sizeof(b));
  if (fs->fat_type == 12u) {
    if (dev_read_bytes(fs, off + (UINT64)clus * 3u / 2u, b, 2u) != 0) {
      return fs->eoc;
    }
    return ((clus & 1u) != 0u)
               ? (UINT32)((le16(b) >> 4) & 0x0FFFu)
               : (UINT32)(le16(b) & 0x0FFFu);
  }
  if (fs->fat_type == 16u) {
    if (dev_read_bytes(fs, off + (UINT64)clus * 2u, b, 2u) != 0) {
      return fs->eoc;
    }
    return le16(b);
  }
  if (dev_read_bytes(fs, off + (UINT64)clus * 4u, b, 4u) != 0) {
    return fs->eoc;
  }
  return le32(b) & 0x0FFFFFFFu;
}

static UINT64 cluster_offset(fat_fs_t *fs, UINT32 clus) {
  return ((UINT64)fs->data_start +
          (UINT64)(clus - 2u) * fs->sec_per_clus) * 512u;
}

int fat_mount(fat_fs_t *fs, blkdev_t *dev) {
  UINT8  bpb[512];
  UINT32 total;
  UINT32 data_secs;
  UINT32 clusters;

  if (fs == NULL || dev == NULL) {
    return -1;
  }
  memset(fs, 0, sizeof(*fs));
  fs->dev = dev;
  fs->spb = blkdev_sectors_per_block(dev);
  if (fs->spb == 0u || fs->spb > BLKDEV_BLOCK_MAX / 512u) {
    return -1;
  }
  if (dev_read_bytes(fs, 0, bpb, sizeof(bpb)) != 0) {
    return -1;
  }
  fs->bytes_per_sec = le16(bpb + 11);
  if (fs->bytes_per_sec != 512u) {
    return -1; /* the ESP and USB media this reader targets are 512n */
  }
  fs->sec_per_clus = bpb[13];
  fs->reserved = le16(bpb + 14);
  fs->num_fats = bpb[16];
  fs->root_entries = le16(bpb + 17);
  fs->fat_size = le16(bpb + 22);
  if (fs->fat_size == 0u) {
    fs->fat_size = le32(bpb + 36);
  }
  total = le16(bpb + 19);
  if (total == 0u) {
    total = le32(bpb + 32);
  }
  if (total == 0u || (UINT64)total > dev->sector_count) {
    total = (UINT32)dev->sector_count;
  }
  fs->root_cluster = le32(bpb + 44);
  if (fs->sec_per_clus == 0u || fs->reserved == 0u ||
      fs->num_fats == 0u || fs->fat_size == 0u) {
    return -1;
  }
  if ((bpb[510] != 0x55u || bpb[511] != 0xAAu) &&
      memcmp(bpb + 54, "FAT", 3) != 0 &&
      memcmp(bpb + 82, "FAT", 3) != 0) {
    /* A superfloppy without the boot signature is still a FAT volume if
     * its BPB carries a FAT type string. */
    return -1;
  }
  fs->root_dir_sectors =
      (fs->root_entries * 32u + fs->bytes_per_sec - 1u) /
      fs->bytes_per_sec;
  fs->fat_start = fs->reserved;
  fs->root_dir_start = fs->fat_start + fs->num_fats * fs->fat_size;
  fs->data_start = fs->root_dir_start + fs->root_dir_sectors;
  if (fs->data_start >= total) {
    return -1;
  }
  data_secs = total - fs->data_start;
  clusters = data_secs / fs->sec_per_clus;
  if (clusters < 4085u) {
    fs->fat_type = 12u;
    fs->eoc = 0x0FF8u;
  } else if (clusters < 65525u) {
    fs->fat_type = 16u;
    fs->eoc = 0xFFF8u;
  } else {
    fs->fat_type = 32u;
    fs->eoc = 0x0FFFFFF8u;
  }
  if (fs->fat_type == 32u && fs->root_cluster < 2u) {
    return -1;
  }

  /* Node 1 is the volume root; node 0 stays unused so 0 means "absent". */
  fs->used_nodes = 2u;
  fs->nodes[FAT_ROOT_INO].used = 1u;
  fs->nodes[FAT_ROOT_INO].is_dir = 1u;
  fs->nodes[FAT_ROOT_INO].parent = 0u;
  fs->nodes[FAT_ROOT_INO].first_cluster =
      (fs->fat_type == 32u) ? fs->root_cluster : 0u;
  fs->nodes[FAT_ROOT_INO].name[0] = '\0';
  fs->mounted = 1u;
  return 0;
}

void fat_unmount(fat_fs_t *fs) {
  if (fs != NULL) {
    fs->mounted = 0u;
    fs->dev = NULL;
  }
}

static void decode_83(const UINT8 *e, char *out) {
  UINTN i;
  UINTN n = 0;

  for (i = 0; i < 8u && e[i] != ' '; i++) {
    out[n++] = (char)e[i];
  }
  if (e[8] != ' ') {
    out[n++] = '.';
    for (i = 8; i < 11u && e[i] != ' '; i++) {
      out[n++] = (char)e[i];
    }
  }
  out[n] = '\0';
}

/* Returns 1 when the 0x00 end-of-directory marker is reached. */
static int scan_sector(fat_fs_t *fs, UINT32 dir, const UINT8 *sec) {
  UINTN i;

  for (i = 0; i < 16u; i++) {
    const UINT8 *e = sec + i * 32u;
    UINT32 clus;
    UINT32 ino;
    char   name[13];

    if (e[0] == 0x00u) {
      return 1;
    }
    if (e[0] == 0xE5u || e[0] == '.') {
      continue;
    }
    if ((e[11] & 0x08u) != 0u || (e[11] & 0x0Fu) == 0x0Fu) {
      continue; /* volume label or long-name fragment */
    }
    decode_83(e, name);
    if (name[0] == '\0' || fs->used_nodes >= FAT_MAX_NODES) {
      continue;
    }
    clus = (UINT32)le16(e + 26);
    if (fs->fat_type == 32u) {
      clus |= (UINT32)le16(e + 20) << 16;
    }
    ino = fs->used_nodes++;
    fs->nodes[ino].used = 1u;
    fs->nodes[ino].parent = dir;
    fs->nodes[ino].first_cluster = clus;
    fs->nodes[ino].size = le32(e + 28);
    fs->nodes[ino].is_dir = (UINT8)((e[11] & 0x10u) != 0u);
    fs->nodes[ino].scanned = 0u;
    {
      UINTN k;
      for (k = 0; k + 1u < sizeof(fs->nodes[ino].name) && name[k] != '\0';
           k++) {
        fs->nodes[ino].name[k] = name[k];
      }
      fs->nodes[ino].name[k] = '\0';
    }
  }
  return 0;
}

static int fat_scan_dir(fat_fs_t *fs, UINT32 dir) {
  UINT8  sec[512];
  UINT32 clus;
  UINT32 s;

  if (dir == 0u || dir >= FAT_MAX_NODES || !fs->nodes[dir].used ||
      !fs->nodes[dir].is_dir) {
    return -1;
  }
  if (fs->nodes[dir].scanned != 0u) {
    return 0; /* already materialised */
  }
  fs->nodes[dir].scanned = 1u; /* set first: a cycle cannot recurse */
  if (fs->fat_type != 32u && fs->nodes[dir].first_cluster == 0u) {
    for (s = 0; s < fs->root_dir_sectors; s++) {
      if (dev_read_bytes(fs, (UINT64)(fs->root_dir_start + s) * 512u,
                         sec, sizeof(sec)) != 0) {
        return -1;
      }
      if (scan_sector(fs, dir, sec) != 0) {
        break;
      }
    }
    return 0;
  }
  clus = fs->nodes[dir].first_cluster;
  while (clus >= 2u && clus < fs->eoc) {
    int ended = 0;

    for (s = 0; s < fs->sec_per_clus; s++) {
      if (dev_read_bytes(fs, cluster_offset(fs, clus) + (UINT64)s * 512u,
                         sec, sizeof(sec)) != 0) {
        return -1;
      }
      if (scan_sector(fs, dir, sec) != 0) {
        ended = 1;
        break;
      }
    }
    if (ended != 0) {
      break;
    }
    clus = fat_next(fs, clus);
  }
  return 0;
}

UINT32 fat_lookup(fat_fs_t *fs, UINT32 dir, const char *name) {
  UINT32 i;

  if (fs == NULL || !fs->mounted || name == NULL) {
    return 0;
  }
  if (fat_scan_dir(fs, dir) != 0) {
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

int fat_readdir(fat_fs_t *fs, UINT32 dir, vfs_readdir_cb cb, void *arg) {
  UINT32 i;

  if (fs == NULL || cb == NULL || fat_scan_dir(fs, dir) != 0) {
    return -1;
  }
  for (i = 1; i < fs->used_nodes; i++) {
    if (fs->nodes[i].used && fs->nodes[i].parent == dir) {
      cb(arg, i, fs->nodes[i].name);
    }
  }
  return 0;
}

int fat_stat(fat_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode) {
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

INT64 fat_read(fat_fs_t *fs, UINT32 ino, UINT64 off, void *buf, UINT32 len) {
  UINT8  *out = (UINT8 *)buf;
  UINT64  skip = off;
  UINT32  clus;
  UINT32  clus_bytes;
  UINTN   done = 0;

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
  clus = fs->nodes[ino].first_cluster;
  clus_bytes = fs->sec_per_clus * 512u;
  while (done < len && clus >= 2u && clus < fs->eoc) {
    UINTN chunk;

    if (skip >= (UINT64)clus_bytes) {
      skip -= clus_bytes;
      clus = fat_next(fs, clus);
      continue;
    }
    chunk = (UINTN)((UINT64)clus_bytes - skip);
    if (chunk > len - done) {
      chunk = len - done;
    }
    if (dev_read_bytes(fs, cluster_offset(fs, clus) + skip,
                       out + done, chunk) != 0) {
      return -1;
    }
    done += chunk;
    skip = 0;
    clus = fat_next(fs, clus);
  }
  return (INT64)done;
}
