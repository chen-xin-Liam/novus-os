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

vfs_fs_t *ext4_vfs(ext4_fs_t *fs) {
  if (fs == NULL) {
    return NULL;
  }
  memset(&g_vfs, 0, sizeof(g_vfs));
  g_vfs.kind = VFS_KIND_EXT4;
  g_vfs.ctx = fs;
  g_vfs.root = fs->root_ino;
  g_vfs.persistent = 1;
  g_vfs.lookup = v_lookup;
  g_vfs.creat = v_creat;
  g_vfs.mkdir = v_mkdir;
  g_vfs.unlink = v_unlink;
  g_vfs.read = v_read;
  g_vfs.write = v_write;
  g_vfs.stat = v_stat;
  g_vfs.readdir = v_readdir;
  g_vfs.sync = v_sync;
  return &g_vfs;
}
