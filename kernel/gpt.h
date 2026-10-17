/**
 * Novus OS - GUID Partition Table reader (M3).
 *
 * The live image and an installed system are both GPT disks:
 *
 *   - an EFI system partition (FAT32) that the firmware boots from and
 *     that holds the system files and signed kernel modules;
 *   - a Novus system partition carrying the NovusFS image the A core
 *     verifies with the Merkle tree (README 2.2, 4.2.2).
 *
 * Only the primary header and its entry array are read. A disk with no
 * valid GPT is reported as such and the caller decides what to do - a
 * raw, unpartitioned disk is a legitimate install target, so
 * gpt_read() returning -1 is not an error for the installer.
 *
 * All offsets are in 512-byte sectors, matching blkdev's addressing.
 */

#ifndef NOVUS_GPT_H
#define NOVUS_GPT_H

#include "blkdev.h"

/* Header signature "EFI PART" at LBA 1. */
#define GPT_SIGNATURE        "EFI PART"
#define GPT_SIGNATURE_LEN    8u
#define GPT_HEADER_LBA       1u
#define GPT_HEADER_MIN_SIZE  92u
#define GPT_ENTRY_SIZE       128u

/* Upper bound on entries this reader keeps. A standard GPT reserves
 * 128; Novus creates two partitions, so a small fixed table is enough
 * and keeps the parser allocation-free. */
#define GPT_MAX_PARTITIONS   16

/* Partition type GUIDs, stored in the GPT on-disk byte order (Data1
 * little-endian, then Data2, Data3, then Data4 in sequence).
 *
 * ESP:     C12A7328-F81F-11D2-BA4B-00A0C93EC93B (UEFI specification).
 * NovusFS: 4E565346-4653-4E4F-5655-534600000001, allocated for the
 *          Novus system partition. It is part of the frozen on-disk
 *          format: changing it abandons every installed system. */
#define GPT_TYPE_ESP_BYTES                                              \
  { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,                    \
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }

#define GPT_TYPE_NOVUSFS_BYTES                                          \
  { 0x46, 0x53, 0x56, 0x4E, 0x53, 0x46, 0x4F, 0x4E,                    \
    0x56, 0x55, 0x53, 0x46, 0x53, 0x00, 0x00, 0x01 }

typedef struct {
  UINT8  type_guid[16];
  UINT64 first_lba;    /* first sector of the partition */
  UINT64 last_lba;     /* last sector, inclusive */
  UINT64 sector_count; /* last_lba - first_lba + 1 */
  CHAR16 name[36];     /* UTF-16 name, not NUL-terminated if 36 chars */
} gpt_part_t;

typedef struct {
  UINT64     sector_count;    /* whole device */
  UINT64     first_usable_lba;
  UINT64     last_usable_lba;
  UINTN      count;           /* entries stored in part[] */
  gpt_part_t part[GPT_MAX_PARTITIONS];
} gpt_table_t;

/* Read and validate the GPT on `dev`.
 *
 * Checks the header signature, revision, header size and header CRC-32
 * (computed with the CRC field zeroed, per the UEFI specification), then
 * walks the entry array and keeps every used entry up to
 * GPT_MAX_PARTITIONS. Returns 0 on success, -1 when the device is
 * unreadable or carries no valid GPT. */
int gpt_read(blkdev_t *dev, gpt_table_t *out);

/* Copy the first partition of type `type_guid` into `out`.
 * Returns 0 on success, -1 when no such partition is present. */
int gpt_find_type(const gpt_table_t *table, const UINT8 type_guid[16],
                  gpt_part_t *out);

/* Convenience wrappers for the two types Novus cares about. */
int gpt_find_esp(const gpt_table_t *table, gpt_part_t *out);
int gpt_find_novusfs(const gpt_table_t *table, gpt_part_t *out);

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */
/* A partition to create, as the installer describes it. The unique GUID
 * is filled in by gpt_write(), which derives it from the type GUID and
 * the slot index: a disk that has been re-imaged twice must not confuse
 * a consumer that cached the previous GUIDs. */

#define GPT_ENTRY_ARRAY_ENTRIES 128u
#define GPT_ENTRY_ARRAY_SECTORS                                           \
  ((GPT_ENTRY_ARRAY_ENTRIES * GPT_ENTRY_SIZE) / BLKDEV_SECTOR_SIZE)

typedef struct {
  UINT8  type_guid[16];
  UINT64 first_lba;
  UINT64 last_lba;
  CHAR16 name[36];
} gpt_part_spec_t;

/* Write a complete GPT: protective MBR, primary header, primary entry
 * array, backup entry array and backup header. Everything else on the
 * device is left alone, so the caller writes the partition payloads
 * afterwards.
 *
 * Returns 0 on success, -1 if the device is too small, a sector write
 * fails, or a partition range does not fit. */
int gpt_write(blkdev_t *dev, const gpt_part_spec_t *specs, UINTN count);

#endif /* NOVUS_GPT_H */
