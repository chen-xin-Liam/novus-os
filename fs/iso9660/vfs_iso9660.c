/**
 * Novus OS - read-only ISO 9660 / Joliet adapter for the VFS.
 */
#include "iso9660.h"

static iso9660_fs_t *ctx(vfs_fs_t *fs) {
  return (iso9660_fs_t *)fs->ctx;
}

static UINT32 a_lookup(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return iso9660_lookup(ctx(fs), dir, name);
}

static INT64 a_read(vfs_fs_t *fs, UINT32 ino, UINT64 off, void *buf,
                    UINT32 len) {
  return iso9660_read(ctx(fs), ino, off, buf, len);
}

static int a_stat(vfs_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode) {
  return iso9660_stat(ctx(fs), ino, size, mode);
}

static int a_readdir(vfs_fs_t *fs, UINT32 dir, vfs_readdir_cb cb, void *arg) {
  return iso9660_readdir(ctx(fs), dir, cb, arg);
}

static UINT32 a_none(vfs_fs_t *fs, UINT32 dir, const char *name) {
  (void)fs;
  (void)dir;
  (void)name;
  return 0;
}

static int a_unlink(vfs_fs_t *fs, UINT32 dir, const char *name) {
  (void)fs;
  (void)dir;
  (void)name;
  return -1;
}

static INT64 a_write(vfs_fs_t *fs, UINT32 ino, UINT64 off, const void *buf,
                     UINT32 len) {
  (void)fs;
  (void)ino;
  (void)off;
  (void)buf;
  (void)len;
  return -1;
}

static int a_rename(vfs_fs_t *fs, UINT32 od, const char *on, UINT32 nd,
                    const char *nn) {
  (void)fs;
  (void)od;
  (void)on;
  (void)nd;
  (void)nn;
  return -1;
}

static int a_sync(vfs_fs_t *fs) {
  (void)fs;
  return 0;
}

void iso9660_vfs_init(vfs_fs_t *view, iso9660_fs_t *fs) {
  if (view == NULL || fs == NULL) {
    return;
  }
  memset(view, 0, sizeof(*view));
  view->kind = VFS_KIND_ISO9660;
  view->ctx = fs;
  view->root = ISO9660_ROOT_INO;
  view->persistent = 0;
  view->readonly = 1;
  view->lookup = a_lookup;
  view->creat = a_none;
  view->mkdir = a_none;
  view->unlink = a_unlink;
  view->read = a_read;
  view->write = a_write;
  view->stat = a_stat;
  view->readdir = a_readdir;
  view->sync = a_sync;
  view->rename = a_rename;
}
