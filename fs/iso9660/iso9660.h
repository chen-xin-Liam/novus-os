/**
 * Novus OS - read-only ISO 9660 / Joliet volume reader.
 *
 * Joliet is preferred when present: it carries the original file names
 * (spaces, dots, case) that the disc builder publishes. Directory entries
 * are materialised into a fixed node table on first lookup.
 */
#ifndef NOVUS_ISO9660_H
#define NOVUS_ISO9660_H

#include "kernel.h"
#include "blkdev.h"
#include "vfs.h"

#define ISO9660_SECTOR    2048u
#define ISO9660_MAX_NODES 128u
#define ISO9660_ROOT_INO  1u

typedef struct {
  char   name[VFS_NAME_MAX + 1u];
  UINT32 parent;
  UINT32 lba;      /* 2048-byte logical sector of the extent */
  UINT32 size;
  UINT8  is_dir;
  UINT8  used;
  UINT8  scanned;
  UINT8  pad;
} iso9660_node_t;

typedef struct iso9660_fs {
  blkdev_t *dev;
  UINT8     bounce[BLKDEV_BLOCK_MAX];
  UINT32    spb;
  UINT32    root_lba;
  UINT32    root_size;
  UINT8     joliet;
  UINT8     mounted;
  UINT32    used_nodes;
  iso9660_node_t nodes[ISO9660_MAX_NODES];
} iso9660_fs_t;

int    iso9660_mount(iso9660_fs_t *fs, blkdev_t *dev);
void   iso9660_unmount(iso9660_fs_t *fs);
UINT32 iso9660_lookup(iso9660_fs_t *fs, UINT32 dir, const char *name);
int    iso9660_readdir(iso9660_fs_t *fs, UINT32 dir, vfs_readdir_cb cb,
                       void *arg);
INT64  iso9660_read(iso9660_fs_t *fs, UINT32 ino, UINT64 off, void *buf,
                    UINT32 len);
int    iso9660_stat(iso9660_fs_t *fs, UINT32 ino, UINT64 *size,
                    UINT16 *mode);

/* Fill a caller-owned vfs_fs_t view (one per mounted volume). */
void   iso9660_vfs_init(vfs_fs_t *view, iso9660_fs_t *fs);

#endif /* NOVUS_ISO9660_H */
