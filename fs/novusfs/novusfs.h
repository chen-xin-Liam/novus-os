/**
 * Novus OS - NovusFS, minimal version, milestone M2 (README part 4).
 *
 * On-disk layout follows the frozen ordering of README 4.2:
 *
 *   block 0                    primary superblock
 *   block 1                    backup superblock (dual copy, 4.2.2)
 *   [wal_start ..)             WAL circular log region
 *   [merkle_start ..)          Merkle tree region (RESERVED, M3, not filled)
 *   [free_bmap_start ..)       free block bitmap
 *   [inode_tbl_start ..)       inode table
 *   [data_start ..)            data blocks
 *
 * M2 deliberately implements the filesystem core plus the write-ahead
 * log; the Merkle tree (M3) and the VFS syscall surface (M4) are out of
 * scope. The Merkle region is still carved out of the layout so that
 * landing the tree later never moves a block and never breaks the disk
 * ABI.
 *
 * All disk access goes through kernel/blkdev.h; all memory comes from
 * kernel/heap.h (kmalloc/kfree); the log checksum is the CRC-32 already
 * exported by lib/novuslib-rs (README 16.4 FFI boundary).
 *
 * This header is the frozen M2 disk format and the internal API.
 */

#ifndef NOVUS_NOVUSFS_H
#define NOVUS_NOVUSFS_H

#include "uefi.h"
#include "kernel.h"
#include "blkdev.h"
#include "heap.h"

/* novuslib_rs.h lives outside the fs/ tree; pull it in by a path
 * relative to this file's directory (same trick kernel/pmm.h uses). */
#include "../../lib/novuslib_rs.h"

#ifndef NULL
#define NULL ((VOID *)0)
#endif

/* Packed on-disk structures: no compiler-inserted padding, so the byte
 * image is stable across toolchains and milestones. */
#define NOVUSFS_PACKED __attribute__((packed))

/* ------------------------------------------------------------------ */
/* Frozen on-disk constants                                            */
/* ------------------------------------------------------------------ */

#define NOVUSFS_MAGIC       0x4E4F5653u   /* "NOVS" */
#define NOVUSFS_VERSION     0x00030000u   /* M3: Merkle tree */
#define NOVUSFS_BLOCK_SIZE  4096u
#define NOVUSFS_SECTORS_PER_BLOCK (NOVUSFS_BLOCK_SIZE / BLKDEV_SECTOR_SIZE)

#define NOVUSFS_SB_PRIMARY  0u
#define NOVUSFS_SB_BACKUP   1u

/* Fixed-size circular WAL: 64 blocks == 256 KiB. */
#define NOVUSFS_WAL_BLOCKS  64u

#define NOVUSFS_MAX_INODES       1024u
#define NOVUSFS_INODE_SIZE       128u
#define NOVUSFS_INODES_PER_BLOCK (NOVUSFS_BLOCK_SIZE / NOVUSFS_INODE_SIZE)
#define NOVUSFS_INODE_TBL_BLOCKS (NOVUSFS_MAX_INODES / NOVUSFS_INODES_PER_BLOCK)

/* One bitmap block covers 4096 * 8 == 32768 data blocks, far more than
 * the 16 MiB ramdisk M2 ships, so a single block is enough. */
#define NOVUSFS_BMAP_BLOCKS 1u

/* 11 direct pointers + 1 single indirect (512 entries) per inode. */
#define NOVUSFS_NDIR          11u
#define NOVUSFS_INDIRECT_CNT  (NOVUSFS_BLOCK_SIZE / 8u)

#define NOVUSFS_NAME_MAX     56u
#define NOVUSFS_DIRENT_SIZE  64u

/* inode modes */
#define NOVUSFS_IFREE 0u
#define NOVUSFS_IFILE 1u
#define NOVUSFS_IDIR  2u

#define NOVUSFS_ROOT_INO 1u

/* ------------------------------------------------------------------ */
/* WAL (README 4.2.1)                                                  */
/* ------------------------------------------------------------------ */

#define WAL_ENTRY_MAGIC  0x57414C45u   /* "WALE" */
#define WAL_HDR_SIZE     32u
/* Bytes of the header covered by the checksum: everything before the
 * checksum field itself (magic..data_len == 28 bytes). */
#define WAL_CRC_PREFIX   28u
#define WAL_MAX_PAYLOAD  NOVUSFS_BLOCK_SIZE

/* Merkle tree (README 4.2.2, frozen in M3).
 *
 * Nodes are fixed 64-byte SHA-256 digests stored consecutively, 64 per
 * 4096-byte block, starting at sb.merkle_start. Node idx lives at
 *
 *   block  = merkle_start + idx / MERKLE_NODES_PER_BLOCK
 *   offset = (idx % MERKLE_NODES_PER_BLOCK) * MERKLE_NODE_SIZE
 *
 * Level 0 holds the leaf hashes, one per protected block in ascending
 * block order; every higher level hashes the 128-byte concatenation of
 * two children. A level with an odd node count is padded by duplicating
 * its last node, and the duplicate IS stored, so build and verify walk
 * identical shapes. The number of levels is
 * ceil(log2(leaf_count)) + 1. */
#define NOVUSFS_MERKLE_ROOT_LEN    32u
#define MERKLE_NODE_SIZE           64u
#define MERKLE_NODES_PER_BLOCK    (NOVUSFS_BLOCK_SIZE / MERKLE_NODE_SIZE)

/* Verification outcomes reported to the caller (never fatal here). */
#define NOVUSFS_MERKLE_OK         0u
#define NOVUSFS_MERKLE_MISMATCH   1u
#define NOVUSFS_MERKLE_UNREADABLE 2u
#define NOVUSFS_MERKLE_NOT_BUILT  3u

/* Transaction types. M2 ships the two that the minimal FS needs. */
#define NOVUSFS_TX_DATA   1u   /* block payload to redo on the data area */
#define NOVUSFS_TX_COMMIT 2u   /* transaction commit marker */

/* ------------------------------------------------------------------ */
/* On-disk structures                                                  */
/* ------------------------------------------------------------------ */

/* Superblock, one 4096-byte block. `checksum` must stay the last field:
 * it is the CRC-32 over every preceding byte of the structure. */
typedef struct NOVUSFS_PACKED novusfs_super {
  UINT32 magic;
  UINT32 version;
  UINT32 block_size;
  UINT64 total_blocks;

  UINT32 inode_count;
  UINT64 inode_tbl_start;
  UINT64 inode_tbl_blocks;

  UINT64 free_bmap_start;
  UINT64 free_bmap_blocks;

  UINT64 wal_start;
  UINT64 wal_blocks;
  UINT64 merkle_start;
  UINT64 merkle_blocks;

  UINT64 wal_write_ptr;   /* circular byte offset inside the WAL region */
  UINT64 wal_tail_ptr;    /* circular byte offset, oldest live entry */

  UINT32 root_inode;
  UINT32 sb_version;      /* dual-copy version counter (README 4.2.2) */

  /* ---- M3: Merkle tree over the protected region (README 4.2.2) ----
   * The tree covers [free_bmap_start, total_blocks): the bitmap, the
   * inode table and the data area. It deliberately excludes the WAL
   * (replayed before verification) and the Merkle region itself (the
   * tree cannot cover its own storage). The superblock is covered by
   * its own CRC-32 and dual copy instead.
   */
  UINT8  merkle_root[NOVUSFS_MERKLE_ROOT_LEN];
  UINT32 merkle_height;     /* levels, == ceil(log2(leaf_count)) + 1 */
  UINT32 merkle_leaf_count; /* == total_blocks - free_bmap_start */
  UINT32 merkle_valid;      /* 1 == root matches the protected region */

  UINT32 checksum;
} novusfs_super_t;

/* Inode: 128 bytes, 32 per block. M2 keeps a single indirect level. */
typedef struct NOVUSFS_PACKED novusfs_inode {
  UINT16 mode;
  UINT16 pad0;
  UINT32 ino;
  UINT32 size;            /* file size in bytes */
  UINT32 nlinks;
  UINT32 blocks;          /* data blocks in use */
  UINT64 mtime;
  UINT64 dblock[NOVUSFS_NDIR];
  UINT64 iblk;            /* single indirect block, 0 == none */
} novusfs_inode_t;

/* Directory entry: fixed 64 bytes, so a directory is a flat array of
 * entries. ino == 0 marks a free/deleted slot. */
typedef struct NOVUSFS_PACKED novusfs_dirent {
  UINT32 ino;
  UINT16 rec_len;
  UINT16 name_len;
  UINT8  name[NOVUSFS_NAME_MAX];
} novusfs_dirent_t;

/* WAL entry header, immediately followed by `data_len` payload bytes. */
typedef struct NOVUSFS_PACKED wal_entry {
  UINT32 magic;
  UINT64 txid;
  UINT32 type;
  UINT64 block_no;        /* absolute FS block the payload redoes */
  UINT32 data_len;
  UINT32 checksum;        /* CRC-32 over header[0..28) ++ payload */
} wal_entry_t;

/* ------------------------------------------------------------------ */
/* In-memory filesystem state                                          */
/* ------------------------------------------------------------------ */

typedef struct novusfs_fs {
  blkdev_t *dev;
  UINTN     mounted;             /* 0 == not mounted */
  novusfs_super_t sb;            /* cached superblock */
  UINT64    wal_bytes;           /* wal_blocks * block_size */
  UINT64    data_start;          /* first data block (derived) */
  UINT64    data_blocks;         /* number of data blocks (derived) */

  /* Scratch buffers, all allocated once at mount.
   * Ownership discipline: `scratch` is a shared read-modify-write
   * buffer that any low-level helper (bitmap, indirect block, partial
   * ring write) may clobber; `blkbuf` belongs to the data path (file
   * and directory blocks) and is never touched by those helpers, so a
   * caller can keep a composed block there across a balloc/bmap call. */
  UINT8    *scratch;             /* one block, low-level read-modify-write */
  UINT8    *blkbuf;              /* one block, data path (file/dir) */
  UINT8    *walbuf;              /* WAL_HDR_SIZE + payload assembly */
  UINT8    *crcbuf;              /* WAL_CRC_PREFIX + payload, for CRC-32 */

  UINT64    next_txid;
  UINT64    mtime_counter;
  UINTN     wal_corrupt;         /* 1 == WAL failed checksum verification */
} novusfs_fs_t;

/* The single mounted instance. Returns NULL while nothing is mounted,
 * so no layer can touch disk state before mount succeeds. */
novusfs_fs_t *novusfs_get(void);
novusfs_fs_t *novusfs_root(void);
int novusfs_root_persistent(void);
void novusfs_set_root(novusfs_fs_t *fs);

/* ------------------------------------------------------------------ */
/* super.c - layout, superblock, raw block IO                          */
/* ------------------------------------------------------------------ */

/* Read one filesystem block into `buf` (NOVUSFS_BLOCK_SIZE bytes). */
int novusfs_read_block(novusfs_fs_t *fs, UINT64 blk, void *buf);
/* Write one filesystem block from `buf`. */
int novusfs_write_block(novusfs_fs_t *fs, UINT64 blk, const void *buf);

/* Carve the layout for `dev`, write both superblock copies and zero the
 * WAL, bitmap and inode table. Fills fs->sb and marks the fs mounted;
 * the caller then creates the root directory (novusfs.c). */
int novusfs_format_layout(novusfs_fs_t *fs, blkdev_t *dev);

/* Read and validate the primary superblock, falling back to the backup
 * copy when the primary fails its magic/CRC check. Allocates the
 * scratch buffers and marks the fs mounted. Returns -1 when neither
 * copy is usable (README 4.2.3: report, refuse, never panic). */
int novusfs_super_read(novusfs_fs_t *fs, blkdev_t *dev);

/* Recompute the checksum, bump sb_version and write both copies. */
int novusfs_super_flush(novusfs_fs_t *fs);

/* Print the layout summary (debug/diagnostic, mirrors to VGA + COM1). */
void novusfs_dump_layout(novusfs_fs_t *fs);

/* M3: replace the allocator the FS uses for its scratch buffers.
 *
 * The boot stage (A core, before ExitBootServices) has no kernel heap,
 * so it swaps in EFI AllocatePool/FreePool before mounting. Passing NULL
 * for either argument restores the default kmalloc/kfree. An explicit
 * setter is used rather than weak symbols because the boot stage and the
 * kernel stage are the same linked image: a weak symbol would let one
 * stage's definition silently win in both. */
typedef void *(*novusfs_alloc_fn)(UINT64 bytes);
typedef void (*novusfs_free_fn)(void *ptr);
void novusfs_set_allocator(novusfs_alloc_fn alloc_fn,
                           novusfs_free_fn free_fn);

/* Allocate/free through whichever allocator is in effect. merkle.c
 * needs buffers of its own and must obey the same hook, or the boot
 * stage would kmalloc into a heap that does not exist yet. */
void *novusfs_alloc(UINT64 bytes);
void  novusfs_free(void *ptr);

/* Release the buffers and forget the device; the image stays on disk. */
void novusfs_unmount(novusfs_fs_t *fs);

/* ------------------------------------------------------------------ */
/* balloc.c - free block bitmap                                        */
/* ------------------------------------------------------------------ */

/* Return 1 when `blk` (absolute block number) is marked in use. */
int novusfs_balloc_is_used(novusfs_fs_t *fs, UINT64 blk);

/* Allocate one data block: returns its absolute block number, or 0 when
 * the data area is exhausted. The returned block is NOT zeroed. */
UINT64 novusfs_balloc_alloc(novusfs_fs_t *fs);

/* Release one data block obtained from novusfs_balloc_alloc(). */
void novusfs_balloc_free(novusfs_fs_t *fs, UINT64 blk);

/* Number of free data blocks (diagnostics). */
UINT64 novusfs_balloc_free_count(novusfs_fs_t *fs);

/* ------------------------------------------------------------------ */
/* inode.c - inode table                                               */
/* ------------------------------------------------------------------ */

/* Allocate a free inode of `mode`, persist it and return its number
 * (>0), or 0 when the table is full. */
UINT32 novusfs_inode_alloc(novusfs_fs_t *fs, UINT16 mode);

/* Read/write/release an inode.
 *
 * novusfs_inode_write_tx() persists `in` inside an open WAL
 * transaction: the patched inode-table block is appended to the log as
 * a redo record and only reaches the table when the transaction
 * commits. txid == 0 writes straight through, which is what formatting
 * uses before the ring exists. */
int novusfs_inode_read(novusfs_fs_t *fs, UINT32 ino, novusfs_inode_t *out);
int novusfs_inode_write(novusfs_fs_t *fs, const novusfs_inode_t *in);
int novusfs_inode_write_tx(novusfs_fs_t *fs, const novusfs_inode_t *in,
                           UINT64 txid);

/* Persist several inodes at once. Required whenever one transaction
 * touches two inodes that share an inode-table block: writing them
 * separately would append two redo records for the same block and the
 * second would undo the first. Inodes sharing a block are patched into
 * a single block image and logged once. */
int novusfs_inode_write_batch(novusfs_fs_t *fs, const novusfs_inode_t *ins,
                              UINTN count, UINT64 txid);

void novusfs_inode_free(novusfs_fs_t *fs, UINT32 ino);

/* Map logical block `lblk` of `in` to an absolute block number,
 * allocating the mapping when `alloc` is set. Returns 0 when absent. */
UINT64 novusfs_inode_bmap(novusfs_fs_t *fs, novusfs_inode_t *in,
                          UINT64 lblk, int alloc);

/* ------------------------------------------------------------------ */
/* dir.c - directories                                                 */
/* ------------------------------------------------------------------ */

/* Look up `name` inside directory `dir_ino`; returns the inode number
 * or 0 when not found. */
UINT32 novusfs_dir_lookup(novusfs_fs_t *fs, UINT32 dir_ino, const char *name);

/* Directory updates take the directory inode by pointer and modify it
 * in place (size, block count, mtime); they only touch the directory's
 * data blocks on disk. The caller owns the copy and persists it with
 * novusfs_inode_write_tx()/novusfs_inode_write_batch() once every
 * change of the transaction is staged - that is what keeps one
 * transaction from emitting two conflicting records for the same
 * inode-table block.
 *
 * `txid` != 0 routes block updates through the WAL; 0 writes through
 * directly, which is what formatting uses before the ring exists. */
int novusfs_dir_add(novusfs_fs_t *fs, novusfs_inode_t *din,
                    const char *name, UINT32 ino, UINT64 txid);

/* Remove `name` (slot is marked free, not compacted). */
int novusfs_dir_remove(novusfs_fs_t *fs, novusfs_inode_t *din,
                       const char *name, UINT64 txid);

/* Initialise a freshly allocated directory inode with "." and "..". */
int novusfs_dir_init(novusfs_fs_t *fs, novusfs_inode_t *din,
                     UINT32 parent_ino, UINT64 txid);

/* Call `cb` for every live entry of `dir_ino`. `ctx` is passed through. */
typedef void (*novusfs_readdir_cb)(void *ctx, UINT32 ino, const char *name);
int novusfs_dir_readdir(novusfs_fs_t *fs, UINT32 dir_ino,
                        novusfs_readdir_cb cb, void *ctx);

/* ------------------------------------------------------------------ */
/* wal.c - write-ahead log (README 4.2.1)                              */
/* ------------------------------------------------------------------ */

/* Size in bytes of one log entry: header + payload, rounded up to 8. */
UINT64 wal_entry_size(UINT32 data_len);

/* Open a transaction and return its TXID (0 == failure). */
UINT64 wal_begin(novusfs_fs_t *fs);

/* Append a redo record: `len` bytes will be written to absolute block
 * `blk`. Must be called before the block itself is touched. */
int wal_log_block(novusfs_fs_t *fs, UINT64 txid, UINT64 blk,
                  const void *data, UINT32 len);

/* Append the commit marker and apply (redo) every record of `txid` to
 * the data area, then persist the ring pointers. */
int wal_commit(novusfs_fs_t *fs, UINT64 txid);

/* Abandon a transaction: no redo records of `txid` are applied. */
void wal_abort(novusfs_fs_t *fs, UINT64 txid);

/* Replay committed transactions found between tail and write pointer.
 * Called on mount; idempotent, so re-running over an already-applied
 * log is harmless. Returns the number of entries replayed. */
UINT64 wal_recover(novusfs_fs_t *fs);

/* Walk the ring and verify every entry's magic + CRC-32. Returns the
 * number of valid entries; sets fs->wal_corrupt on the first failure. */
UINT64 wal_verify(novusfs_fs_t *fs);

/* Bytes currently in use in the ring (diagnostics). */
UINT64 wal_used_bytes(novusfs_fs_t *fs);

/* ------------------------------------------------------------------ */
/* merkle.c - integrity tree (README 4.2.2)                            */
/* ------------------------------------------------------------------ */

/* Number of tree levels for `leaf_count` leaves: ceil(log2(n)) + 1.
 * Exposed so the layout code can size the region before the tree
 * exists and so the selftest can pin the shape. */
UINT32 novusfs_merkle_height(UINT64 leaf_count);

/* Blocks that a tree over `total_blocks` blocks needs, including the
 * per-level padding. Used by the formatter to reserve the region. */
UINT64 novusfs_merkle_reserve(UINT64 total_blocks);

/* Build the tree over [free_bmap_start, total_blocks) and store it in
 * the Merkle region, then record the root, height and leaf count in the
 * superblock with merkle_valid = 1 and flush both superblock copies.
 *
 * Writes out_root[32] when out_root is non-NULL. Returns 0 on success,
 * -1 when the region is too small, the device fails or the superblock
 * cannot be flushed. */
int novusfs_merkle_build(novusfs_fs_t *fs, UINT8 *out_root);

/* Verify the protected region against the root stored in the
 * superblock.
 *
 * Implemented as a rebuild followed by a comparison, which guarantees
 * build and verify can never disagree about the tree shape. The rebuild
 * only writes the Merkle region - derived data - so the protected
 * region is never modified by a verification.
 *
 * Returns one of the NOVUSFS_MERKLE_* codes; out_root[32] receives the
 * recomputed root when non-NULL. Never fatal: the caller decides. */
UINT32 novusfs_merkle_verify(novusfs_fs_t *fs, UINT8 *out_root);

/* Mark the stored root stale. Called by the WAL once a committed
 * transaction has changed a protected block, so a later mount cannot
 * mistake an outdated root for a valid one. */
void novusfs_merkle_invalidate(novusfs_fs_t *fs);

/* ------------------------------------------------------------------ */
/* novusfs.c - high level API                                          */
/* ------------------------------------------------------------------ */

/* Format `dev` (layout + root directory) and mount it. */
int novusfs_format(novusfs_fs_t *fs, blkdev_t *dev);

/* Mount an existing NovusFS image, then recover the WAL. */
int novusfs_mount(novusfs_fs_t *fs, blkdev_t *dev);

/* Create `path` (relative to `dir_ino`) as a file; returns its inode. */
UINT32 novusfs_creat(novusfs_fs_t *fs, UINT32 dir_ino, const char *name);

/* Create a directory; returns its inode number. */
UINT32 novusfs_mkdir(novusfs_fs_t *fs, UINT32 dir_ino, const char *name);

/* Remove one entry from a directory. */
int novusfs_unlink(novusfs_fs_t *fs, UINT32 dir_ino, const char *name);

/* Write `len` bytes at `off` of `ino`; grows the file as needed.
 * The whole update runs inside one WAL transaction. Returns the number
 * of bytes written, or -1 on error. */
INT64 novusfs_write(novusfs_fs_t *fs, UINT32 ino, UINT64 off,
                    const void *data, UINT64 len);

/* Read up to `len` bytes at `off` of `ino` into `dst`. Returns the
 * number of bytes actually read, or -1 on error. */
INT64 novusfs_read(novusfs_fs_t *fs, UINT32 ino, UINT64 off,
                   void *dst, UINT64 len);

/* stat: file size and mode into `out_size` / `out_mode`. */
int novusfs_stat(novusfs_fs_t *fs, UINT32 ino, UINT64 *out_size,
                 UINT16 *out_mode);

/* Resolve a '/'-separated path relative to `dir_ino`; 0 == not found. */
UINT32 novusfs_lookup_path(novusfs_fs_t *fs, UINT32 dir_ino, const char *path);

/* End-to-end self test; prints PASS/FAIL lines. Returns 0 on success. */
int novusfs_selftest(void);

#endif /* NOVUS_NOVUSFS_H */
