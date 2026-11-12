/**
 * Novus OS - mount configuration and device binding.
 *
 * /.novusfs/mounts.json is a readable, fixed-key table of the mount
 * points the system may use. The kernel applies it at boot and exposes
 * the resulting state through SYS_MOUNTS. Runtime mount/umount never
 * writes the file back: changing a protected block would invalidate the
 * Merkle root the next boot verifies.
 */
#ifndef NOVUS_MOUNTCFG_H
#define NOVUS_MOUNTCFG_H

#include "kernel.h"
#include "vfs_mount.h"

#define MOUNTCFG_MAX_ENTRIES VFS_MOUNT_MAX
#define MOUNTCFG_TEXT_MAX    4096u

/* Read the mount table, classify detected block devices and mount what is
 * present. Safe to call once, after every candidate root is mounted. */
int mountcfg_apply(void);

/* Explicit operations from the shell's mount/umount commands. `dev` is a
 * logical name (diskN / usbN / cdromN); `fstype` is "auto" or an explicit
 * back end name. */
int mountcfg_mount(const char *path, const char *dev, const char *fstype);
int mountcfg_umount(const char *path);

/* Fill up to `cap` user-visible records; returns the active count. */
UINT32 mountcfg_list(user_mount_t *out, UINT32 cap);

/* Logical name for /dev display, NULL when the index is out of range. */
const char *mountcfg_logical_name(UINT32 index);

#endif /* NOVUS_MOUNTCFG_H */
