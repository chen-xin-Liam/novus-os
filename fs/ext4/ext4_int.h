/**
 * Novus OS - ext4 internals shared by the driver files (M12).
 *
 * The ext4 driver is split by concern - volume, formatting, inodes,
 * directories - and these are the pieces they share. They are not part of
 * the public interface: only ext4.h is.
 */

#ifndef NOVUS_EXT4_INT_H
#define NOVUS_EXT4_INT_H

#include "ext4.h"

/* Read or write one block of the volume. */
int ext4_blk_read(ext4_fs_t *fs, UINT64 blk, void *buf);
int ext4_blk_write(ext4_fs_t *fs, UINT64 blk, const void *buf);

/* Allocation. The bitmaps are the authority; the group descriptors and
 * the superblock keep the free counters in step. */
int ext4_alloc_block(ext4_fs_t *fs, UINT64 *out);
int ext4_free_block(ext4_fs_t *fs, UINT64 blk);
int ext4_alloc_inode(ext4_fs_t *fs, UINT32 *out);
int ext4_free_inode(ext4_fs_t *fs, UINT32 ino);

/* Write the group descriptor of one group back to the volume. */
int ext4_group_update(ext4_fs_t *fs, UINT32 g);

/* The block that holds the inode table entry of `ino`. */
UINT64 ext4_inode_block(const ext4_fs_t *fs, UINT32 ino);

/* Map one logical block of `in` to a physical one. Returns 1 when the block
 * is mapped, 0 when it is a hole and -1 on error. */
int ext4_map_block(ext4_fs_t *fs, const ext4_inode_t *in, UINT32 lblk,
                   UINT64 *phys);

/* Insert one extent into an inode's extent tree, growing the tree when the
 * inode's own node runs out of room. `in` is updated in place; the caller
 * writes the inode back. */
int ext4_add_extent(ext4_fs_t *fs, UINT32 ino, ext4_inode_t *in, UINT32 lblk,
                    UINT32 len, UINT64 phys);

/* Free every data block of an inode and reset its extent tree. */
int ext4_inode_truncate(ext4_fs_t *fs, UINT32 ino);

/* How many extents fit in one extent tree node. */
#define EXT4_EXTENTS_PER_LEAF 4u

#endif /* NOVUS_EXT4_INT_H */
