/**
 * Novus OS - virtual file system dispatch (M12).
 *
 * See fs/vfs.h for the contract. This file holds the NovusFS adapter (the
 * reference back end), the shared path walk, and the root registry; the
 * ext4 and NTFS back ends register themselves the same way.
 */

#include "kernel.h"
#include "blkdev.h"
#include "vfs.h"
#include "novusfs.h"

/* ------------------------------------------------------------------ */
/* NovusFS adapter                                                      */
/* ------------------------------------------------------------------ */

static UINT32 nv_lookup(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return novusfs_dir_lookup((novusfs_fs_t *)fs->ctx, dir, name);
}

static UINT32 nv_creat(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return novusfs_creat((novusfs_fs_t *)fs->ctx, dir, name);
}

static UINT32 nv_mkdir(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return novusfs_mkdir((novusfs_fs_t *)fs->ctx, dir, name);
}

static int nv_unlink(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return novusfs_unlink((novusfs_fs_t *)fs->ctx, dir, name);
}

static INT64 nv_read(vfs_fs_t *fs, UINT32 ino, UINT64 off, void *buf,
                     UINT32 len) {
  return novusfs_read((novusfs_fs_t *)fs->ctx, ino, off, buf, len);
}

static INT64 nv_write(vfs_fs_t *fs, UINT32 ino, UINT64 off, const void *buf,
                      UINT32 len) {
  return novusfs_write((novusfs_fs_t *)fs->ctx, ino, off, buf, len);
}

static int nv_stat(vfs_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode) {
  return novusfs_stat((novusfs_fs_t *)fs->ctx, ino, size, mode);
}

static int nv_readdir(vfs_fs_t *fs, UINT32 dir, vfs_readdir_cb cb, void *arg) {
  return novusfs_dir_readdir((novusfs_fs_t *)fs->ctx, dir, cb, arg);
}

static int nv_sync(vfs_fs_t *fs) {
  return novusfs_super_flush((novusfs_fs_t *)fs->ctx);
}

static void nv_fill(vfs_fs_t *v, novusfs_fs_t *n) {
  memset(v, 0, sizeof(*v));
  v->kind = VFS_KIND_NOVUSFS;
  v->ctx = n;
  v->root = n->sb.root_inode;
  v->lookup = nv_lookup;
  v->creat = nv_creat;
  v->mkdir = nv_mkdir;
  v->unlink = nv_unlink;
  v->read = nv_read;
  v->write = nv_write;
  v->stat = nv_stat;
  v->readdir = nv_readdir;
  v->sync = nv_sync;
  v->persistent = (UINT8)(novusfs_root_persistent() ? 1u : 0u);
}

/* ------------------------------------------------------------------ */
/* Root registry                                                        */
/* ------------------------------------------------------------------ */

/* Two views, because there are two kinds of root: the NovusFS one, which
 * novusfs_root() owns and which can change under us, and an ext4/NTFS
 * mount the boot stage or the installer installed explicitly. */
static vfs_fs_t g_novusfs_view;
static vfs_fs_t g_mounted;
static int      g_mounted_set;

vfs_fs_t *vfs_root(void) {
  novusfs_fs_t *n;

  if (g_mounted_set) {
    return &g_mounted;
  }
  n = novusfs_root();
  if (n == NULL) {
    return NULL;
  }
  nv_fill(&g_novusfs_view, n);
  return &g_novusfs_view;
}

void vfs_set_root(vfs_fs_t *fs) {
  if (fs == NULL) {
    g_mounted_set = 0;
    memset(&g_mounted, 0, sizeof(g_mounted));
    return;
  }
  g_mounted = *fs;
  g_mounted_set = 1;
}

vfs_fs_t *vfs_novusfs_wrap(novusfs_fs_t *fs) {
  static vfs_fs_t view;

  if (fs == NULL) {
    return NULL;
  }
  nv_fill(&view, fs);
  return &view;
}

int vfs_root_persistent(void) {
  vfs_fs_t *fs = vfs_root();

  return (fs != NULL && fs->persistent != 0u) ? 1 : 0;
}

const char *vfs_kind_name(vfs_kind_t kind) {
  switch (kind) {
  case VFS_KIND_NOVUSFS: return "NovusFS";
  case VFS_KIND_EXT4: return "ext4";
  case VFS_KIND_NTFS: return "NTFS";
  default: return "unknown";
  }
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                         */
/* ------------------------------------------------------------------ */

UINT32 vfs_lookup_path(vfs_fs_t *fs, const char *path) {
  UINT32       cur;
  const char  *p;

  if (fs == NULL || path == NULL) {
    return 0;
  }
  cur = fs->root;
  p = path;
  if (*p == '/') {
    cur = fs->root;
    p++;
  }
  while (*p != '\0') {
    char  comp[VFS_NAME_MAX + 1u];
    UINTN n = 0;

    while (*p != '\0' && *p != '/') {
      if (n < VFS_NAME_MAX) {
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
    cur = fs->lookup(fs, cur, comp);
    if (cur == 0) {
      return 0;
    }
  }
  return cur;
}

int vfs_read_file(vfs_fs_t *fs, const char *path, void *buf, UINT32 len) {
  UINT32 ino;
  INT64  n;

  if (fs == NULL || path == NULL || buf == NULL) {
    return -1;
  }
  ino = vfs_lookup_path(fs, path);
  if (ino == 0u) {
    return -1;
  }
  n = fs->read(fs, ino, 0, buf, len);
  return (n == (INT64)len) ? 0 : -1;
}

int vfs_write_file(vfs_fs_t *fs, const char *path, const void *buf,
                   UINT32 len) {
  UINT32 ino;
  UINT32 parent;
  char  *leaf;
  char   copy[VFS_NAME_MAX + 2u];
  UINTN  n = 0;
  INT64  w;

  if (fs == NULL || path == NULL || buf == NULL) {
    return -1;
  }
  ino = vfs_lookup_path(fs, path);
  if (ino == 0u) {
    /* Create it: the path is copied because vfs_parent_path splits it in
     * place. */
    while (path[n] != '\0' && n < sizeof(copy) - 1u) {
      copy[n] = path[n];
      n++;
    }
    copy[n] = '\0';
    if (vfs_parent_path(fs, copy, &parent, &leaf) != 0) {
      return -1;
    }
    ino = fs->creat(fs, parent, leaf);
    if (ino == 0u) {
      return -1;
    }
  }
  w = fs->write(fs, ino, 0, buf, len);
  if (w != (INT64)len) {
    return -1;
  }
  if (fs->sync != NULL) {
    (void)fs->sync(fs);
  }
  return 0;
}

/* Length of a NUL-terminated string, never reading past `cap` bytes. The
 * caller uses it to make sure a name fits the file system. */
static UINTN vfs_strlen(const char *s, UINTN cap) {
  UINTN n = 0;

  if (s == NULL) {
    return 0;
  }
  while (n < cap && s[n] != '\0') {
    n++;
  }
  return n;
}

int vfs_parent_path(vfs_fs_t *fs, char *path, UINT32 *parent, char **leaf) {
  UINTN len = 0;
  UINTN split = 0;

  if (fs == NULL || path == NULL || parent == NULL || leaf == NULL) {
    return -1;
  }
  while (path[len] != '\0') {
    len++;
  }
  while (len > 0u && path[len - 1u] == '/') {
    path[--len] = '\0';
  }
  if (len == 0u) {
    return -1;
  }
  split = len;
  while (split > 0u && path[split - 1u] != '/') {
    split--;
  }
  *leaf = path + split;
  if ((*leaf)[0] == '\0' ||
      vfs_strlen(*leaf, VFS_NAME_MAX + 1u) > VFS_NAME_MAX) {
    return -1;
  }
  if (split == 0u) {
    *parent = fs->root;
  } else {
    path[split - 1u] = '\0';
    *parent = vfs_lookup_path(fs, path);
  }
  return (*parent == 0u) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Self test                                                           */
/* ------------------------------------------------------------------ */

/* One file, created and removed again, so the test leaves nothing
 * behind on a real system. */
#define VFS_TEST_FILE "/vfs-selftest.tmp"

typedef struct {
  vfs_fs_t   *fs;
  const char *want;
  int         seen;
} vfs_ls_ctx_t;

static void vfs_ls_pick(void *arg, UINT32 ino, const char *name) {
  vfs_ls_ctx_t *ctx = (vfs_ls_ctx_t *)arg;

  (void)ino;
  if (ctx->want != NULL && name != NULL) {
    UINTN i = 0;

    while (ctx->want[i] != '\0' && name[i] != '\0' &&
           ctx->want[i] == name[i]) {
      i++;
    }
    if (ctx->want[i] == '\0' && name[i] == '\0') {
      ctx->seen = 1;
    }
  }
}

/* Exercise the dispatch layer the way the kernel uses it: create a file,
 * write a pattern, read it back, stat it, list the directory, remove it.
 * Without a mounted root there is nothing to test, which is not a
 * failure - a live medium has no writable root. */
int vfs_selftest(void) {
  vfs_fs_t *fs = vfs_root();
  UINT8      pattern[64];
  UINT8      back[64];
  UINT32     i;
  int        ok = 1;

  if (fs == NULL) {
    kputs("[VFS] selftest skipped (no root)\n");
    return 0;
  }
  for (i = 0; i < sizeof(pattern); i++) {
    pattern[i] = (UINT8)(i * 7u + 3u);
  }
  if (vfs_write_file(fs, VFS_TEST_FILE, pattern, sizeof(pattern)) != 0) {
    kputs("[VFS] selftest FAIL (write)\n");
    return -1;
  }
  memset(back, 0, sizeof(back));
  if (vfs_read_file(fs, VFS_TEST_FILE, back, sizeof(back)) != 0 ||
      memcmp(back, pattern, sizeof(pattern)) != 0) {
    kputs("[VFS] selftest FAIL (read)\n");
    ok = 0;
  }
  {
    UINT32 ino = vfs_lookup_path(fs, VFS_TEST_FILE);
    UINT64 size = 0;
    UINT16 mode = 0;

    if (ino == 0u || fs->stat(fs, ino, &size, &mode) != 0 ||
        mode != VFS_MODE_FILE || size != (UINT64)sizeof(pattern)) {
      kputs("[VFS] selftest FAIL (stat)\n");
      ok = 0;
    }
  }
  {
    vfs_ls_ctx_t ctx;

    ctx.fs = fs;
    ctx.want = "vfs-selftest.tmp";
    ctx.seen = 0;
    if (fs->readdir(fs, fs->root, vfs_ls_pick, &ctx) != 0 || ctx.seen == 0) {
      kputs("[VFS] selftest FAIL (readdir)\n");
      ok = 0;
    }
  }
  {
    char   path[VFS_NAME_MAX + 4u];
    UINT32 parent = 0;
    char  *leaf = NULL;

    memcpy(path, VFS_TEST_FILE, sizeof(path));
    if (vfs_parent_path(fs, path, &parent, &leaf) != 0 ||
        fs->unlink(fs, parent, leaf) != 0) {
      kputs("[VFS] selftest FAIL (unlink)\n");
      ok = 0;
    }
  }
  if (ok) {
    kputs("[VFS] selftest: PASS (");
    kputs(vfs_kind_name(fs->kind));
    kputs(")\n");
    return 0;
  }
  return -1;
}
