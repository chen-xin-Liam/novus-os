/**
 * Novus OS - mount points for the single-root VFS.
 *
 * The root file system is still the one vfs_root() returns. A mount point
 * redirects a fixed absolute prefix ("/disk0", "/media/usb0", ...) to a
 * second vfs_fs_t. Path resolution walks one component at a time and
 * switches back ends at the longest matching prefix, so ls/cd/cat work
 * across mounts without changing the syscall ABI's inode handle.
 *
 * The table is fixed-size and allocation-free, like the rest of the VFS.
 */
#ifndef NOVUS_VFS_MOUNT_H
#define NOVUS_VFS_MOUNT_H

#include "vfs.h"

#define VFS_MOUNT_MAX 8u

typedef struct vfs_mount {
  char      path[VFS_PATH_MAX];
  char      dev[16];       /* logical device name: disk0, usb0, cdrom0 */
  char      fstype[12];    /* novusfs, ext4, fat, iso9660 */
  vfs_fs_t *fs;
  UINT8     readonly;
  UINT8     active;
} vfs_mount_t;

/* The user-space view returned by SYS_MOUNTS. Rust mirrors this layout in
 * apps/src/fs.rs; the size assertion keeps the two from drifting. */
typedef struct {
  char  path[64];
  char  dev[16];
  char  fstype[12];
  UINT8 active;
  UINT8 readonly;
  UINT8 pad[2];
} user_mount_t;

_Static_assert(sizeof(user_mount_t) == 96u, "mount ABI drift");

/* Register/remove a mount. "/" is reserved for the boot root. */
int vfs_mount_add(const char *path, vfs_fs_t *fs, const char *dev,
                  const char *fstype, int readonly);
int vfs_mount_remove(const char *path);
const vfs_mount_t *vfs_mount_at(UINT32 index);
UINT32 vfs_mount_count(void);
const vfs_mount_t *vfs_mount_find_path(const char *path);

/* Resolve a path to (inode, back end). Returns 0 when absent. */
UINT32 vfs_resolve_path(const char *path, vfs_fs_t **out_fs);

/* Split `path` in place at the last '/', resolve its parent and return the
 * back end that owns the leaf. Refuses a mount point itself as a leaf. */
int vfs_resolve_parent(char *path, vfs_fs_t **out_fs, UINT32 *parent,
                       char **leaf);

/* Path-level operations that must use the back end of the resolved mount,
 * not the boot root. */
int vfs_mount_unlink_path(const char *path);
int vfs_mount_rename_path(const char *old_path, const char *new_path);

#endif /* NOVUS_VFS_MOUNT_H */
