/**
 * Novus OS - ext4 adapter for the virtual file system dispatch (M12).
 *
 * Wraps an ext4_fs_t as a vfs_fs_t so the kernel talks to ext4 through the
 * same five operations it uses for NovusFS. The inode number is the ext4
 * inode number, which fits the UINT32 handle the dispatch layer carries.
 */

#include "kernel.h"
#include "blkdev.h"
#include "ext4.h"
#include "ext4_int.h"
#include "vfs.h"

static vfs_fs_t g_vfs;

static UINT32 v_lookup(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return ext4_lookup((ext4_fs_t *)fs->ctx, dir, name);
}

static UINT32 v_creat(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return ext4_creat((ext4_fs_t *)fs->ctx, dir, name);
}

static UINT32 v_mkdir(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return ext4_mkdir((ext4_fs_t *)fs->ctx, dir, name);
}

static int v_unlink(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return ext4_unlink((ext4_fs_t *)fs->ctx, dir, name);
}

static INT64 v_read(vfs_fs_t *fs, UINT32 ino, UINT64 off, void *buf,
                    UINT32 len) {
  return ext4_read((ext4_fs_t *)fs->ctx, ino, off, buf, len);
}

static INT64 v_write(vfs_fs_t *fs, UINT32 ino, UINT64 off, const void *buf,
                     UINT32 len) {
  return ext4_write((ext4_fs_t *)fs->ctx, ino, off, buf, len);
}

static int v_stat(vfs_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode) {
  ext4_fs_t *e = (ext4_fs_t *)fs->ctx;
  UINT64     s = 0;
  UINT16     m = 0;

  if (ext4_stat(e, ino, &s, &m) != 0) {
    return -1;
  }
  if (size != NULL) {
    *size = s;
  }
  if (mode != NULL) {
    /* Map the POSIX mode's type bits onto the vfs file-type constants. */
    *mode = ((m & 0xF000u) == 0x4000u) ? VFS_MODE_DIR : VFS_MODE_FILE;
  }
  return 0;
}

static int v_readdir(vfs_fs_t *fs, UINT32 dir, vfs_readdir_cb cb, void *arg) {
  return ext4_readdir((ext4_fs_t *)fs->ctx, dir, cb, arg);
}

static int v_sync(vfs_fs_t *fs) {
  return ext4_sync((ext4_fs_t *)fs->ctx);
}

void ext4_vfs_init(struct vfs_fs *view, ext4_fs_t *fs) {
  vfs_fs_t *v = view;

  if (v == NULL || fs == NULL) {
    return;
  }
  memset(v, 0, sizeof(*v));
  v->kind = VFS_KIND_EXT4;
  v->ctx = fs;
  v->root = fs->root_ino;
  v->persistent = 1;
  v->lookup = v_lookup;
  v->creat = v_creat;
  v->mkdir = v_mkdir;
  v->unlink = v_unlink;
  v->read = v_read;
  v->write = v_write;
  v->stat = v_stat;
  v->readdir = v_readdir;
  v->sync = v_sync;
}

vfs_fs_t *ext4_vfs(ext4_fs_t *fs) {
  if (fs == NULL) {
    return NULL;
  }
  ext4_vfs_init(&g_vfs, fs);
  return &g_vfs;
}
