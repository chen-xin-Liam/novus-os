/**
 * Novus OS - virtual file system dispatch (M12).
 *
 * The kernel has one file system that can be the root of a running system
 * (NovusFS) and is growing two more that the installer can format with
 * (ext4, NTFS). Rather than teach every syscall three file systems, this
 * layer holds the small set of operations the kernel actually needs and
 * dispatches to whichever back end is mounted.
 *
 * Design rules kept deliberately narrow:
 *
 *   - One inode handle type (UINT32) for every back end. NovusFS inodes and
 *     ext4 inode numbers are already 32-bit; an NTFS MFT record index fits
 *     as well, so the syscall ABI (user_stat_t, user_dirent_t) does not
 *     change when the root file system changes.
 *   - Path walking lives here, once. A back end only has to resolve a
 *     single component, which is where the three file systems actually
 *     differ.
 *   - No allocation, no blocking, no unbounded copies: every operation is
 *     called from a syscall on a task stack.
 */

#ifndef NOVUS_VFS_H
#define NOVUS_VFS_H

#include "kernel.h"
#include "novusfs.h"

/* Which file system a mount is. */
typedef enum {
  VFS_KIND_NOVUSFS = 0,
  VFS_KIND_EXT4 = 1,
  VFS_KIND_NTFS = 2
} vfs_kind_t;

/* File type bits. These are the values the syscall ABI hands to user
 * space (user_stat_t.mode, user_dirent_t.mode), so a back end must map its
 * own file types onto them. */
#define VFS_MODE_FILE 1u
#define VFS_MODE_DIR  2u

/* Longest path component the dispatch layer copies. Matches NovusFS so a
 * name is never silently truncated on the way down. */
#define VFS_NAME_MAX NOVUSFS_NAME_MAX

typedef struct vfs_fs vfs_fs_t;

/* Directory enumeration callback: one call per entry, in directory order. */
typedef void (*vfs_readdir_cb)(void *ctx, UINT32 ino, const char *name);

struct vfs_fs {
  vfs_kind_t kind;
  void      *ctx;   /* novusfs_fs_t *, ext4_fs_t *, ntfs_fs_t * */
  UINT32     root;  /* inode / MFT record number of the root directory */
  /* 1 = the root survives a reboot (a real disk), 0 = a live image. */
  UINT8      persistent;

  /* Resolve one component inside `dir`. Returns 0 when absent. */
  UINT32 (*lookup)(vfs_fs_t *fs, UINT32 dir, const char *name);
  /* Create a file or a directory inside `dir`; returns the new inode or 0. */
  UINT32 (*creat)(vfs_fs_t *fs, UINT32 dir, const char *name);
  UINT32 (*mkdir)(vfs_fs_t *fs, UINT32 dir, const char *name);
  int    (*unlink)(vfs_fs_t *fs, UINT32 dir, const char *name);
  /* Read/write at a byte offset; returns the byte count or -1. */
  INT64  (*read)(vfs_fs_t *fs, UINT32 ino, UINT64 off, void *buf, UINT32 len);
  INT64  (*write)(vfs_fs_t *fs, UINT32 ino, UINT64 off, const void *buf,
                  UINT32 len);
  /* Size and type of one inode; either output may be NULL. */
  int    (*stat)(vfs_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode);
  int    (*readdir)(vfs_fs_t *fs, UINT32 dir, vfs_readdir_cb cb, void *arg);
  /* Flush metadata; 0 on success. */
  int    (*sync)(vfs_fs_t *fs);
};

/* ---- the root the syscalls talk to ------------------------------------ */

/* The mounted root, or NULL when no file system is mounted. The NovusFS
 * view is derived from novusfs_root() on every call, so there is exactly
 * one source of truth for "which file system is the root". */
vfs_fs_t *vfs_root(void);

/* Make an ext4/NTFS mount the root. Passing NULL restores the NovusFS
 * view. */
void vfs_set_root(vfs_fs_t *fs);

/* 1 when the root lives on a disk and survives a reboot, 0 for a live
 * image in reserved pages. */
int vfs_root_persistent(void);

const char *vfs_kind_name(vfs_kind_t kind);

/* ---- path helpers, shared by every back end --------------------------- */

/* Walk an absolute or relative path. A leading '/' restarts at the root,
 * empty components are skipped, and "." / ".." are ordinary entries. */
UINT32 vfs_lookup_path(vfs_fs_t *fs, const char *path);

/* Split a path into the parent directory and the leaf name, in place:
 * the byte at the last '/' becomes NUL and *leaf points after it. */
int vfs_parent_path(vfs_fs_t *fs, char *path, UINT32 *parent, char **leaf);

/* Read or write a whole small file by path. The file is created when it
 * is missing (write only). Both return 0 on success and -1 on failure;
 * the read requires the file to be exactly `len` bytes long. */
int vfs_read_file(vfs_fs_t *fs, const char *path, void *buf, UINT32 len);
int vfs_write_file(vfs_fs_t *fs, const char *path, const void *buf,
                   UINT32 len);

/* ---- NovusFS adapter -------------------------------------------------- */

/* Exercise the dispatch layer on the mounted root: create, write, read,
 * stat, list and remove one file. Returns 0 on success, -1 on failure,
 * and 0 without a root (a live medium has nothing to test). */
int vfs_selftest(void);

/* Wrap a NovusFS instance as a vfs_fs_t. The returned pointer is static
 * storage owned by this layer; the caller keeps the novusfs_fs_t. */
vfs_fs_t *vfs_novusfs_wrap(novusfs_fs_t *fs);

#endif /* NOVUS_VFS_H */
