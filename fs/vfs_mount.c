/**
 * Novus OS - mount point table and mount-aware path resolution.
 *
 * No allocation, no recursion into the VFS helpers: resolution walks the
 * path once, component by component, and switches back ends whenever the
 * accumulated prefix is a registered mount. The cost is O(depth * 8).
 */
#include "vfs_mount.h"

static vfs_mount_t g_mounts[VFS_MOUNT_MAX];

static UINTN m_len(const char *s, UINTN cap) {
  UINTN n = 0;

  if (s == NULL) {
    return 0;
  }
  while (n < cap && s[n] != '\0') {
    n++;
  }
  return n;
}

static int m_eq(const char *a, const char *b) {
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

static void m_copy(char *dst, UINTN cap, const char *src) {
  UINTN i = 0;

  if (src != NULL) {
    while (i + 1u < cap && src[i] != '\0') {
      dst[i] = src[i];
      i++;
    }
  }
  dst[i] = '\0';
}

const vfs_mount_t *vfs_mount_find_path(const char *path) {
  UINT32 i;

  if (path == NULL || path[0] != '/') {
    return NULL;
  }
  for (i = 0; i < VFS_MOUNT_MAX; i++) {
    if (g_mounts[i].active != 0u && m_eq(g_mounts[i].path, path)) {
      return &g_mounts[i];
    }
  }
  return NULL;
}

const vfs_mount_t *vfs_mount_at(UINT32 index) {
  return (index < VFS_MOUNT_MAX) ? &g_mounts[index] : NULL;
}

UINT32 vfs_mount_count(void) {
  UINT32 i;
  UINT32 n = 0;

  for (i = 0; i < VFS_MOUNT_MAX; i++) {
    if (g_mounts[i].active != 0u) {
      n++;
    }
  }
  return n;
}

int vfs_mount_add(const char *path, vfs_fs_t *fs, const char *dev,
                  const char *fstype, int readonly) {
  UINT32 i;
  UINT32 free_slot = VFS_MOUNT_MAX;

  if (path == NULL || path[0] != '/' || fs == NULL ||
      m_len(path, VFS_PATH_MAX) >= VFS_PATH_MAX) {
    return -1;
  }
  for (i = 0; i < VFS_MOUNT_MAX; i++) {
    if (g_mounts[i].active != 0u && m_eq(g_mounts[i].path, path)) {
      return -1; /* already mounted: umount first */
    }
    if (g_mounts[i].active == 0u && free_slot == VFS_MOUNT_MAX) {
      free_slot = i;
    }
  }
  if (free_slot == VFS_MOUNT_MAX) {
    return -1;
  }
  memset(&g_mounts[free_slot], 0, sizeof(g_mounts[free_slot]));
  m_copy(g_mounts[free_slot].path, sizeof(g_mounts[free_slot].path), path);
  m_copy(g_mounts[free_slot].dev, sizeof(g_mounts[free_slot].dev), dev);
  m_copy(g_mounts[free_slot].fstype, sizeof(g_mounts[free_slot].fstype),
         fstype);
  g_mounts[free_slot].fs = fs;
  g_mounts[free_slot].readonly = (UINT8)(readonly != 0);
  g_mounts[free_slot].active = 1;
  fs->readonly = (UINT8)(readonly != 0);
  return 0;
}

int vfs_mount_remove(const char *path) {
  UINT32 i;

  if (path == NULL || m_eq(path, "/")) {
    return -1; /* the boot root cannot be unmounted */
  }
  for (i = 0; i < VFS_MOUNT_MAX; i++) {
    if (g_mounts[i].active != 0u && m_eq(g_mounts[i].path, path)) {
      memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
      return 0;
    }
  }
  return -1;
}

UINT32 vfs_resolve_path(const char *path, vfs_fs_t **out_fs) {
  char       acc[VFS_PATH_MAX];
  const char *p = path;
  vfs_fs_t   *fs = vfs_root();
  UINT32      cur;
  UINTN       alen = 0;

  if (fs == NULL || path == NULL) {
    return 0;
  }
  cur = fs->root;
  acc[0] = '\0';
  if (out_fs != NULL) {
    *out_fs = fs;
  }
  while (*p != '\0') {
    char          comp[VFS_NAME_MAX + 1u];
    UINTN         n = 0;
    const vfs_mount_t *m;

    while (*p == '/') {
      p++;
    }
    while (*p != '\0' && *p != '/') {
      if (n >= VFS_NAME_MAX) {
        return 0;
      }
      comp[n++] = *p++;
    }
    if (n == 0u) {
      continue;
    }
    comp[n] = '\0';
    if (alen + 1u + n + 1u > sizeof(acc)) {
      return 0;
    }
    acc[alen++] = '/';
    for (UINTN k = 0; k <= n; k++) {
      acc[alen + k] = comp[k];
    }
    alen += n;

    /* A mount point replaces the directory entry: the mounted volume's
     * root is what a path below the prefix resolves against. */
    m = vfs_mount_find_path(acc);
    if (m != NULL && m->fs != NULL && !m_eq(acc, "/")) {
      fs = m->fs;
      cur = fs->root;
      if (out_fs != NULL) {
        *out_fs = fs;
      }
      continue;
    }
    cur = fs->lookup(fs, cur, comp);
    if (cur == 0u) {
      return 0;
    }
  }
  return cur;
}

int vfs_resolve_parent(char *path, vfs_fs_t **out_fs, UINT32 *parent,
                       char **leaf) {
  UINTN    len;
  UINTN    split;
  vfs_fs_t *fs;

  if (path == NULL || out_fs == NULL || parent == NULL || leaf == NULL) {
    return -1;
  }
  len = m_len(path, VFS_PATH_MAX);
  if (len == 0u || len >= VFS_PATH_MAX) {
    return -1;
  }
  while (len > 0u && path[len - 1u] == '/') {
    path[--len] = '\0';
  }
  if (len == 0u || vfs_mount_find_path(path) != NULL) {
    return -1; /* a mount point is not a removable directory entry */
  }
  split = len;
  while (split > 0u && path[split - 1u] != '/') {
    split--;
  }
  *leaf = path + split;
  if ((*leaf)[0] == '\0' ||
      m_len(*leaf, VFS_NAME_MAX + 1u) > VFS_NAME_MAX) {
    return -1;
  }
  if (split == 0u) {
    fs = vfs_root();
    if (fs == NULL) {
      return -1;
    }
    *parent = fs->root;
    *out_fs = fs;
    return 0;
  }
  path[split - 1u] = '\0';
  *parent = vfs_resolve_path(path, &fs);
  if (*parent == 0u || fs == NULL) {
    return -1;
  }
  *out_fs = fs;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Path-level operations on the resolved back end                       */
/* ------------------------------------------------------------------ */

typedef struct {
  UINT32 count;
} mount_emp_ctx_t;

static void mount_emp_count(void *arg, UINT32 ino, const char *name) {
  (void)ino;
  (void)name;
  ((mount_emp_ctx_t *)arg)->count++;
}

static void bounded_copy(char *dst, const char *src, UINTN cap) {
  UINTN i = 0;

  if (src != NULL) {
    while (i + 1u < cap && src[i] != '\0') {
      dst[i] = src[i];
      i++;
    }
  }
  dst[i] = '\0';
}

int vfs_mount_unlink_path(const char *path) {
  char            copy[VFS_PATH_MAX];
  vfs_fs_t       *fs;
  UINT32          parent;
  UINT32          ino;
  char           *leaf;
  UINT64          size = 0;
  UINT16          mode = 0;
  mount_emp_ctx_t emp;

  if (path == NULL || vfs_mount_find_path(path) != NULL) {
    return -1;
  }
  bounded_copy(copy, path, sizeof(copy));
  if (vfs_resolve_parent(copy, &fs, &parent, &leaf) != 0 ||
      fs->readonly != 0u) {
    return -1;
  }
  ino = fs->lookup(fs, parent, leaf);
  if (ino == 0u || fs->stat(fs, ino, &size, &mode) != 0) {
    return -1;
  }
  if (mode == VFS_MODE_DIR) {
    emp.count = 0u;
    if (fs->readdir(fs, ino, mount_emp_count, &emp) != 0 ||
        emp.count != 0u) {
      return -1;
    }
  }
  return fs->unlink(fs, parent, leaf);
}

int vfs_mount_rename_path(const char *old_path, const char *new_path) {
  char     oc[VFS_PATH_MAX];
  char     nc[VFS_PATH_MAX];
  vfs_fs_t *ofs;
  vfs_fs_t *nfs;
  UINT32    oparent;
  UINT32    nparent;
  char     *oleaf;
  char     *nleaf;

  if (old_path == NULL || new_path == NULL ||
      vfs_mount_find_path(old_path) != NULL ||
      vfs_mount_find_path(new_path) != NULL) {
    return -1;
  }
  bounded_copy(oc, old_path, sizeof(oc));
  bounded_copy(nc, new_path, sizeof(nc));
  if (vfs_resolve_parent(oc, &ofs, &oparent, &oleaf) != 0 ||
      vfs_resolve_parent(nc, &nfs, &nparent, &nleaf) != 0) {
    return -1;
  }
  /* Inodes are only meaningful inside their own back end. */
  if (ofs != nfs || ofs->readonly != 0u || ofs->rename == NULL) {
    return -1;
  }
  return ofs->rename(ofs, oparent, oleaf, nparent, nleaf);
}
