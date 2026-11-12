/**
 * Novus OS - read-only FAT adapter for the VFS.
 */
#include "fat.h"

static fat_fs_t *ctx(vfs_fs_t *fs) {
  return (fat_fs_t *)fs->ctx;
}

static UINT32 a_lookup(vfs_fs_t *fs, UINT32 dir, const char *name) {
  return fat_lookup(ctx(fs), dir, name);
}

static INT64 a_read(vfs_fs_t *fs, UINT32 ino, UINT64 off, void *buf,
                    UINT32 len) {
  return fat_read(ctx(fs), ino, off, buf, len);
}

static int a_stat(vfs_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode) {
  return fat_stat(ctx(fs), ino, size, mode);
}

static int a_readdir(vfs_fs_t *fs, UINT32 dir, vfs_readdir_cb cb, void *arg) {
  return fat_readdir(ctx(fs), dir, cb, arg);
}

static int a_unlink(vfs_fs_t *fs, UINT32 dir, const char *name) {
  (void)fs;
  (void)dir;
  (void)name;
  return -1;
}

static UINT32 a_none(vfs_fs_t *fs, UINT32 dir, const char *name) {
  (void)fs;
  (void)dir;
  (void)name;
  return 0;
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

void fat_vfs_init(vfs_fs_t *view, fat_fs_t *fs) {
  if (view == NULL || fs == NULL) {
    return;
  }
  memset(view, 0, sizeof(*view));
  view->kind = VFS_KIND_FAT;
  view->ctx = fs;
  view->root = FAT_ROOT_INO;
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
