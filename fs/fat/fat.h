/**
 * Novus OS - read-only FAT12/16/32 volume reader.
 *
 * Used for ESP images and USB sticks. Directory entries are materialised
 * into a fixed node table on first lookup; inode numbers are indices into
 * that table. Writes are refused by the VFS adapter.
 */
#ifndef NOVUS_FAT_H
#define NOVUS_FAT_H

#include "kernel.h"
#include "blkdev.h"
#include "vfs.h"

#define FAT_MAX_NODES 128u
#define FAT_NAME_MAX  VFS_NAME_MAX
#define FAT_ROOT_INO  1u

typedef struct {
  char   name[FAT_NAME_MAX + 1u];
  UINT32 parent;
  UINT32 first_cluster; /* 0 = fixed FAT12/16 root directory */
  UINT32 size;
  UINT8  is_dir;
  UINT8  used;
  UINT8  scanned;
  UINT8  pad;
} fat_node_t;

typedef struct fat_fs {
  blkdev_t *dev;
  UINT8     bounce[BLKDEV_BLOCK_MAX];
  UINT32    spb;             /* 512-byte sectors per hardware block */
  UINT32    bytes_per_sec;
  UINT32    sec_per_clus;
  UINT32    reserved;
  UINT32    fat_start;
  UINT32    fat_size;        /* sectors per FAT */
  UINT32    num_fats;
  UINT32    root_entries;
  UINT32    root_dir_start;
  UINT32    root_dir_sectors;
  UINT32    data_start;      /* 512-byte sector index */
  UINT32    root_cluster;
  UINT32    eoc;
  UINT32    used_nodes;
  UINT8     fat_type;        /* 12, 16 or 32 */
  UINT8     mounted;
  fat_node_t nodes[FAT_MAX_NODES];
} fat_fs_t;

int    fat_mount(fat_fs_t *fs, blkdev_t *dev);
void   fat_unmount(fat_fs_t *fs);
UINT32 fat_lookup(fat_fs_t *fs, UINT32 dir, const char *name);
int    fat_readdir(fat_fs_t *fs, UINT32 dir, vfs_readdir_cb cb, void *arg);
INT64  fat_read(fat_fs_t *fs, UINT32 ino, UINT64 off, void *buf, UINT32 len);
int    fat_stat(fat_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode);

/* Fill a caller-owned vfs_fs_t view (one per mounted volume). */
void   fat_vfs_init(vfs_fs_t *view, fat_fs_t *fs);

#endif /* NOVUS_FAT_H */
