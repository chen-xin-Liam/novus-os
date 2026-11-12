/**
 * Novus OS - seed the standard root directory tree.
 *
 * Linked into both the kernel and the host-side mkfsnovus, so an image
 * built on the host, a freshly formatted target partition and the ramdisk
 * used by the NovusFS self test all start with exactly the same tree.
 *
 * novusfs_dir_add() deliberately does not reject duplicate names, so every
 * create here is preceded by a lookup. That also makes seeding idempotent:
 * a partially seeded image can be finished without producing duplicates.
 */
#include "novusfs.h"

#define NOVUS_LAYOUT_WANT_DIRS
#define NOVUS_LAYOUT_WANT_META
#include "rootfs_layout.h"

/* Seeding creates ~80 WAL transactions. Each transaction is committed and
 * redone immediately, so its redo records can be dropped as soon as it is
 * durable: keeping them all would wrap the 256 KiB ring and make a later
 * append fail while the tree is only half built. Reset the ring pointers
 * (an empty ring needs no replay) and persist that checkpoint. */
static void seed_checkpoint(novusfs_fs_t *fs) {
  fs->sb.wal_write_ptr = 0;
  fs->sb.wal_tail_ptr = 0;
  (void)novusfs_super_flush(fs);
}

static UINTN layout_strlen(const char *s) {
  UINTN n = 0;

  if (s == NULL) {
    return 0;
  }
  while (s[n] != '\0') {
    n++;
  }
  return n;
}

/* Resolve `path` from the root, creating missing directories as it walks.
 * Returns the final directory inode, or 0 on failure. A path that ends on
 * an existing file is rejected by the caller's stat, not here. */
UINT32 novusfs_mkdir_p(novusfs_fs_t *fs, const char *path) {
  char comp[NOVUSFS_NAME_MAX + 1u];
  const char *p;
  UINT32 cur;

  if (fs == NULL || !fs->mounted || path == NULL) {
    return 0;
  }
  cur = fs->sb.root_inode;
  if (cur == 0) {
    return 0;
  }
  p = path;
  while (*p != '\0') {
    UINTN n = 0;
    UINT32 next;

    while (*p == '/') {
      p++;
    }
    while (*p != '\0' && *p != '/') {
      if (n < NOVUSFS_NAME_MAX) {
        comp[n++] = *p;
      }
      p++;
    }
    if (n == 0) {
      continue;
    }
    /* Overlong components would have been silently truncated. */
    if (n >= NOVUSFS_NAME_MAX + 1u) {
      return 0;
    }
    comp[n] = '\0';
    if (n == 1u && comp[0] == '.') {
      continue;
    }
    next = novusfs_dir_lookup(fs, cur, comp);
    if (next == 0) {
      next = novusfs_mkdir(fs, cur, comp);
      if (next != 0) {
        seed_checkpoint(fs);
      }
    }
    if (next == 0) {
      return 0;
    }
    cur = next;
  }
  return cur;
}

/* Create a text file inside an already seeded tree. The write is straight
 * through like the font injection: the Merkle tree is rebuilt by the caller
 * (mkfsnovus) or by the self test, and the WAL ring stays untouched. */
static int seed_text(novusfs_fs_t *fs, const char *path, const char *text) {
  char parent[NOVUSFS_NAME_MAX * 2u];
  const char *slash = NULL;
  const char *p;
  UINTN len;
  UINT32 dir;
  UINT32 ino;
  UINTN text_len;

  for (p = path; *p != '\0'; p++) {
    if (*p == '/') {
      slash = p;
    }
  }
  len = (slash == NULL) ? 0u : (UINTN)(slash - path);
  if (len == 0u || len >= sizeof(parent)) {
    return -1;
  }
  for (UINTN i = 0; i < len; i++) {
    parent[i] = path[i];
  }
  parent[len] = '\0';

  dir = novusfs_mkdir_p(fs, parent);
  if (dir == 0) {
    kputs("[NOVS] ERROR seed_text: parent\n");
    return -1;
  }
  ino = novusfs_dir_lookup(fs, dir, slash + 1);
  if (ino == 0) {
    ino = novusfs_creat(fs, dir, slash + 1);
    if (ino != 0) {
      seed_checkpoint(fs);
    }
  }
  if (ino == 0) {
    kputs("[NOVS] ERROR seed_text: creat\n");
    return -1;
  }
  text_len = layout_strlen(text);
  if (text_len == 0u) {
    return 0;
  }
  if (novusfs_write_direct(fs, ino, 0, text, text_len) != (int)text_len) {
    kputs("[NOVS] ERROR seed_text: write\n");
    return -1;
  }
  return 0;
}

int novusfs_seed_root(novusfs_fs_t *fs) {
  UINTN i;

  if (fs == NULL || !fs->mounted) {
    return -1;
  }
  for (i = 0; i < sizeof(novus_layout_dirs) / sizeof(novus_layout_dirs[0]);
       i++) {
    if (novusfs_mkdir_p(fs, novus_layout_dirs[i].path) == 0) {
      kputs("[NOVS] ERROR root layout mkdir failed\n");
      return -1;
    }
  }
  for (i = 0; i < sizeof(novus_layout_meta) / sizeof(novus_layout_meta[0]);
       i++) {
    if (seed_text(fs, novus_layout_meta[i].path,
                  novus_layout_meta[i].text) != 0) {
      kputs("[NOVS] ERROR root layout metadata failed\n");
      return -1;
    }
  }
  return 0;
}

UINT32 novusfs_root_top_count(void) {
  return NOVUS_LAYOUT_TOP_DIR_COUNT;
}
