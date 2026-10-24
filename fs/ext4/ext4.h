/**
 * Novus OS - ext4 driver (M12).
 *
 * The second file system Novus speaks. ext4 is what every Linux machine
 * has on disk, so a Novus install has to be able to read it, write it and
 * - for the installer - create it.
 *
 * This header describes the parts of the on-disk format the driver needs
 * and the API the rest of the kernel uses. The format itself is not
 * restated here: the comments name the offsets that matter, and the code
 * reads the structures byte by byte rather than through packed structs,
 * because a wrong alignment would be a silent data corruption.
 *
 * Scope of this milestone (read path, format, mount):
 *
 *   - superblock, including the 64-bit and metadata_csum variants
 *   - group descriptors, block and inode bitmaps
 *   - inodes and the extent tree (any depth)
 *   - directories, linear and htree indexed
 *   - mkfs: enough of a file system that Linux can mount it
 *
 * Journal replay, sparse files, xattr and the metadata checksums are the
 * next milestone; see docs/ext4 与 NTFS 驱动.md.
 */

#ifndef NOVUS_EXT4_H
#define NOVUS_EXT4_H

#include "kernel.h"
#include "blkdev.h"

/* ---- on-disk constants ------------------------------------------------ */

/* The superblock starts 1024 bytes into the volume, so it lives in
 * sector 2 of the block device. */
#define EXT4_SUPER_OFFSET 1024u
#define EXT4_SUPER_SIZE   1024u
#define EXT4_SUPER_MAGIC  0xEF53u

#define EXT4_ROOT_INO      2u
#define EXT4_BAD_BLOCKS_INO 1u
#define EXT4_LOST_FOUND_INO 11u

/* The inode number the first user file gets: 1..10 are reserved. */
#define EXT4_FIRST_INO 11u

#define EXT4_INODE_SIZE 256u
#define EXT4_MAX_GROUPS 64u
#define EXT4_NAME_MAX   255u

/* Block sizes this driver accepts: 1 KiB, 2 KiB and 4 KiB. A bigger block
 * would need a bigger bounce buffer than the driver has. */
#define EXT4_BLOCK_MIN 1024u
#define EXT4_BLOCK_MAX 4096u

/* Feature flags, as the superblock reports them. */
#define EXT4_COMPAT_HAS_JOURNAL        0x00000004u
#define EXT4_INCOMPAT_FILETYPE          0x00000002u
#define EXT4_INCOMPAT_RECOVER           0x00000004u
#define EXT4_INCOMPAT_META_BG           0x00000010u
#define EXT4_INCOMPAT_EXTENTS           0x00000040u
#define EXT4_INCOMPAT_64BIT             0x00000080u
#define EXT4_INCOMPAT_FLEX_BG           0x00000200u
#define EXT4_RO_COMPAT_SPARSE_SUPER     0x00000001u
#define EXT4_RO_COMPAT_LARGE_FILE       0x00000002u
#define EXT4_RO_COMPAT_BTREE_DIR        0x00000004u
#define EXT4_RO_COMPAT_HUGE_FILE        0x00000008u
#define EXT4_RO_COMPAT_DIR_NLINK        0x00000020u
#define EXT4_RO_COMPAT_EXTRA_ISIZE      0x00000040u
#define EXT4_RO_COMPAT_METADATA_CSUM    0x00000400u

/* File types in a directory entry. */
#define EXT4_FT_UNKNOWN   0u
#define EXT4_FT_REG_FILE  1u
#define EXT4_FT_DIR       2u
#define EXT4_FT_SYMLINK   7u

/* The inode flag that says "i_block holds an extent tree". */
#define EXT4_INODE_FLAG_EXTENTS 0x00080000u

/* The magic of an extent tree header. */
#define EXT4_EXTENT_MAGIC 0xF30Au

/* ---- in-memory structures --------------------------------------------- */

/* One mapped run of blocks. */
typedef struct {
  UINT64 block; /* first physical block */
  UINT32 len;   /* number of blocks */
  UINT8  uninit;/* 1 = unwritten (a hole that was never written) */
} ext4_extent_t;

/* One inode, as the driver caches it. The raw i_block array is kept so
 * the extent tree can be walked again without another inode read. */
typedef struct {
  UINT16 mode;        /* POSIX mode bits */
  UINT16 links;
  UINT32 flags;
  UINT32 blocks;      /* 512-byte units, as the format counts them */
  UINT64 size;        /* bytes */
  UINT16 extra_isize;
  UINT8  i_block[60]; /* extent tree root */
} ext4_inode_t;

/* One block group. */
typedef struct {
  UINT64 block_bitmap;
  UINT64 inode_bitmap;
  UINT64 inode_table;
  UINT32 free_blocks;
  UINT32 free_inodes;
  UINT32 used_dirs;
} ext4_group_t;

typedef struct {
  blkdev_t   *dev;
  UINT32      block_size;
  UINT32      block_shift;  /* log2(block_size) - 10 */
  UINT32      blocks_per_group;
  UINT32      inodes_per_group;
  UINT32      inodes_count;
  UINT64      blocks_count;
  UINT32      first_data_block;
  UINT32      desc_size;    /* 32 or 64 bytes per group descriptor */
  UINT32      group_count;
  UINT32      feature_compat;
  UINT32      feature_incompat;
  UINT32      feature_ro_compat;
  UINT32      inode_size;
  UINT32      root_ino;
  UINT32      lost_found_ino;
  UINT32      journal_ino;
  UINT32      free_blocks;
  UINT32      free_inodes;
  ext4_group_t groups[EXT4_MAX_GROUPS];
  UINT8       mounted;
} ext4_fs_t;

/* ---- API --------------------------------------------------------------- */

/* Does this device hold an ext4 volume? Reads the superblock magic only. */
int ext4_probe(blkdev_t *dev);

/* Mount a volume: superblock, group descriptors, root inode. */
int ext4_mount(ext4_fs_t *fs, blkdev_t *dev);

/* Release a mounted volume. */
void ext4_unmount(ext4_fs_t *fs);

/* Create a file system of `bytes` on the device. The size must be a
 * multiple of the block size and large enough for the metadata. */
int ext4_format(ext4_fs_t *fs, blkdev_t *dev, UINT64 bytes);

/* Inodes. */
int ext4_inode_read(ext4_fs_t *fs, UINT32 ino, ext4_inode_t *out);
int ext4_inode_write(ext4_fs_t *fs, UINT32 ino, const ext4_inode_t *in);

/* File data. Both return the number of bytes moved or -1. */
INT64 ext4_read(ext4_fs_t *fs, UINT32 ino, UINT64 off, void *buf, UINT32 len);
INT64 ext4_write(ext4_fs_t *fs, UINT32 ino, UINT64 off, const void *buf,
                 UINT32 len);

/* Directories. */
UINT32 ext4_lookup(ext4_fs_t *fs, UINT32 dir_ino, const char *name);
int    ext4_readdir(ext4_fs_t *fs, UINT32 dir_ino,
                    void (*cb)(void *arg, UINT32 ino, const char *name),
                    void *arg);
UINT32 ext4_creat(ext4_fs_t *fs, UINT32 dir_ino, const char *name);
UINT32 ext4_mkdir(ext4_fs_t *fs, UINT32 dir_ino, const char *name);
int    ext4_unlink(ext4_fs_t *fs, UINT32 dir_ino, const char *name);

/* Metadata of one inode: size and the file type in the POSIX mode bits. */
int    ext4_stat(ext4_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode);

/* Write the metadata back to the volume. */
int    ext4_sync(ext4_fs_t *fs);

/* Format a ramdisk, mount it and exercise the read/write/lookup/readdir/
 * unlink paths end to end. Returns 0 on success, -1 on failure. Runs at boot
 * as a built-in regression test. */
int    ext4_selftest(void);

/* The dispatch-layer view of a mounted ext4 volume. */
struct vfs_fs;
struct vfs_fs *ext4_vfs(ext4_fs_t *fs);

#endif /* NOVUS_EXT4_H */
