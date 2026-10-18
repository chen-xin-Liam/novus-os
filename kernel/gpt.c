/**
 * Novus OS - GUID Partition Table reader (M3).
 *
 * Reads just enough of the GPT to locate the EFI system partition and
 * the Novus system partition. The on-disk layout is fixed by the UEFI
 * specification; every field is pulled out by explicit byte offset
 * rather than through an overlaid struct, so neither the compiler's
 * struct padding nor the CHAR16 width can shift anything.
 *
 * The header CRC uses novus_rs_crc32 (lib/novuslib-rs), the same
 * reflected CRC-32 the NovusFS superblock and WAL records use.
 */

#include "gpt.h"
#include "kernel.h"
#include "../lib/novuslib_rs.h"

static const UINT8 gpt_type_esp[16]     = GPT_TYPE_ESP_BYTES;
static const UINT8 gpt_type_novusfs[16] = GPT_TYPE_NOVUSFS_BYTES;

/* ------------------------------------------------------------------ */
/* Little-endian field extraction                                      */
/* ------------------------------------------------------------------ */

static UINT32 le32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static UINT64 le64(const UINT8 *p) {
  return (UINT64)le32(p) | ((UINT64)le32(p + 4) << 32);
}

static int guid_is_zero(const UINT8 *g) {
  int i;
  for (i = 0; i < 16; i++) {
    if (g[i] != 0) {
      return 0;
    }
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Header and entry array                                              */
/* ------------------------------------------------------------------ */

/* Interpret one 128-byte entry and append it when it is in use.
 *
 * `stored` is the number of partitions kept so far and is deliberately
 * not the position in the entry array: unused entries are skipped, so
 * the two diverge as soon as the array has a hole. Returns 1 when an
 * entry was stored, 0 when it was skipped. */
static int gpt_take_entry(const UINT8 *e, blkdev_t *dev, gpt_table_t *out,
                          UINTN *stored) {
  gpt_part_t *p;
  UINT64      first;
  UINT64      last;
  UINT64      blocks; /* the device's own blocks, i.e. what LBA counts */

  /* An all-zero type GUID marks an unused entry. */
  if (guid_is_zero(e)) {
    return 0;
  }
  first = le64(e + 32);
  last = le64(e + 40);
  blocks = dev->sector_count / blkdev_sectors_per_block(dev);
  if (first == 0 || last < first || last >= blocks) {
    return 0; /* nonsensical range: skip rather than publish it */
  }
  if (*stored >= GPT_MAX_PARTITIONS) {
    return 0;
  }

  p = &out->part[*stored];
  memcpy(p->type_guid, e, 16);
  p->first_lba = first;
  p->last_lba = last;
  p->first_sector = blkdev_lba_to_sector(dev, first);
  p->sector_count = blkdev_lba_count(dev, last - first + 1u);
  memcpy(p->name, e + 56, 36 * sizeof(CHAR16));
  (*stored)++;
  return 1;
}

/* Scratch big enough for one header block on any device this layer
 * drives. Kept out of the stack: 4 KiB there is a real cost in the boot
 * stage, where gpt_read() runs before the kernel has a heap. */
static UINT8 gpt_block[BLKDEV_BLOCK_MAX];

int gpt_read(blkdev_t *dev, gpt_table_t *out) {
  UINT8  *hdr = gpt_block;
  UINT32  spb = blkdev_sectors_per_block(dev);
  UINT32  header_size;
  UINT32  stored_crc;
  UINT32  crc;
  UINT64  entry_lba;
  UINT32  entry_count;
  UINT32  entry_size;
  UINT64  blocks;
  UINT64  i;
  UINTN   index = 0;        /* position in the entry array */
  UINTN   stored_count = 0; /* partitions kept in out->part[] */
  UINTN   per_block;

  if (dev == NULL || out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->sector_count = dev->sector_count;

  /* Why a disk has no usable table is worth one line: the caller is
   * otherwise left with "no partitions found", which says nothing about
   * whether the disk is blank, corrupt, or read at the wrong offset.
   * gpt_read() is called on every device at boot, so these are
   * diagnostic - a blank install target legitimately reports the first
   * one and is not an error. */
#define GPT_SAY(why)                                                          \
  do {                                                                        \
    kputs("[GPT ] ");                                                         \
    kputs(dev->name);                                                         \
    kputs(": ");                                                              \
    kputs(why);                                                               \
    kputs("\n");                                                              \
  } while (0)

  /* The header lives at LBA 1, which is one *disk block* past the MBR -
   * sector 8 rather than sector 1 when the block is 4096 bytes. */
  if (blkdev_read(dev, blkdev_lba_to_sector(dev, GPT_HEADER_LBA),
                  (UINTN)spb, hdr) != 0) {
    GPT_SAY("cannot read the header block");
    return -1;
  }
  if (memcmp(hdr, GPT_SIGNATURE, GPT_SIGNATURE_LEN) != 0) {
    GPT_SAY("no GPT signature (blank or not partitioned)");
    return -1;
  }
  header_size = le32(hdr + 12);
  if (header_size < GPT_HEADER_MIN_SIZE ||
      header_size > (UINT32)BLKDEV_BLOCK_MAX) {
    GPT_SAY("header size out of range");
    return -1;
  }

  /* Header CRC-32 covers HeaderSize bytes with the CRC field zeroed. */
  stored_crc = le32(hdr + 16);
  memset(hdr + 16, 0, 4);
  crc = novus_rs_crc32(0, hdr, header_size);
  if (crc != stored_crc) {
    GPT_SAY("header CRC mismatch");
    return -1;
  }

  entry_lba = le64(hdr + 72);
  entry_count = le32(hdr + 80);
  entry_size = le32(hdr + 84);
  blocks = dev->sector_count / (UINT64)spb; /* the device in its own blocks */
  if (entry_size != GPT_ENTRY_SIZE || entry_count == 0 ||
      entry_lba == 0 || entry_lba >= blocks) {
    GPT_SAY("entry array does not fit the device");
    return -1;
  }

  out->first_usable_lba = le64(hdr + 40);
  out->last_usable_lba = le64(hdr + 48);

  /* Entries are 128 bytes and packed, so a block holds however many fit:
   * four on a 512-byte disk, thirty-two on a 4Kn one. */
  per_block = (UINTN)(blkdev_block_size(dev) / GPT_ENTRY_SIZE);
  if (per_block == 0) {
    return -1;
  }
  blocks = ((UINT64)entry_count + (UINT64)per_block - 1u) /
           (UINT64)per_block;

  /* `index` walks the entry array, `stored` counts what was kept. */
  for (i = 0; i < blocks && index < (UINTN)entry_count &&
              stored_count < GPT_MAX_PARTITIONS;
       i++) {
    UINT8 *buf = gpt_block;
    UINTN  k;

    if (entry_lba + i >= dev->sector_count / (UINT64)spb) {
      break;
    }
    if (blkdev_read(dev, blkdev_lba_to_sector(dev, entry_lba + i),
                    (UINTN)spb, buf) != 0) {
      break;
    }
    for (k = 0; k < per_block && index < (UINTN)entry_count; k++, index++) {
      (VOID)gpt_take_entry(buf + k * GPT_ENTRY_SIZE, dev, out,
                           &stored_count);
    }
  }

  out->count = stored_count;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

int gpt_find_type(const gpt_table_t *table, const UINT8 type_guid[16],
                  gpt_part_t *out) {
  UINTN i;

  if (table == NULL || type_guid == NULL) {
    return -1;
  }
  for (i = 0; i < table->count; i++) {
    if (memcmp(table->part[i].type_guid, type_guid, 16) == 0) {
      if (out != NULL) {
        *out = table->part[i];
      }
      return 0;
    }
  }
  return -1;
}

int gpt_find_esp(const gpt_table_t *table, gpt_part_t *out) {
  return gpt_find_type(table, gpt_type_esp, out);
}

int gpt_find_novusfs(const gpt_table_t *table, gpt_part_t *out) {
  return gpt_find_type(table, gpt_type_novusfs, out);
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */
/* Layout the installer produces, in sectors:
 *
 *   0                      protective MBR
 *   1                      primary GPT header
 *   2 .. 65                primary entry array (128 * 128 B = 32 KiB)
 *   66 ..                  partitions
 *   total-33 .. total-2    backup entry array
 *   total-1                backup GPT header
 *
 * The standard reserves 16 KiB for the array; 128 entries is what the
 * specification names as the minimum, and it leaves room to grow without
 * moving the first partition later.

 * The protective MBR claims the whole disk as type 0xEE so that a
 * firmware or an OS that does not understand GPT sees a used disk rather
 * than an empty one - the single most common cause of an accidentally
 * reformatted GPT disk. */

static void wr32(UINT8 *p, UINT32 v) {
  p[0] = (UINT8)(v & 0xffu);
  p[1] = (UINT8)((v >> 8) & 0xffu);
  p[2] = (UINT8)((v >> 16) & 0xffu);
  p[3] = (UINT8)((v >> 24) & 0xffu);
}

static void wr64(UINT8 *p, UINT64 v) {
  wr32(p, (UINT32)(v & 0xffffffffu));
  wr32(p + 4, (UINT32)(v >> 32));
}

/* Fixed disk GUID. A real installer randomises it; Novus writes a
 * constant derived from the magic so an image is reproducible, which
 * matters more than uniqueness for a build artefact. */
static const UINT8 gpt_disk_guid[16] = {
    0x4e, 0x4f, 0x56, 0x55, 0x53, 0x2d, 0x44, 0x49,
    0x53, 0x4b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

/* `spb` is 512-byte sectors per device block: the caller lays partitions
 * out in sectors because that is what it writes payloads with, and the
 * entry on disk has to name them in the disk's own blocks. */
static void gpt_fill_entry(UINT8 *e, const gpt_part_spec_t *s, UINTN slot,
                           UINT32 spb) {
  UINTN i;

  memset(e, 0, GPT_ENTRY_SIZE);
  memcpy(e, s->type_guid, 16);
  /* Unique GUID: the type GUID with the slot index in the last byte, so
   * two partitions of the same type never collide. */
  memcpy(e + 16, s->type_guid, 16);
  e[16 + 15] = (UINT8)(0x10u + slot);
  wr64(e + 32, s->first_lba / spb);
  wr64(e + 40, s->last_lba / spb);
  wr64(e + 48, 0); /* attributes: none */
  for (i = 0; i < 36 && s->name[i] != 0; i++) {
    e[56 + i * 2] = (UINT8)(s->name[i] & 0xffu);
    e[56 + i * 2 + 1] = (UINT8)(s->name[i] >> 8);
  }
}

static int gpt_write_mbr(blkdev_t *dev) {
  UINT8 mbr[BLKDEV_SECTOR_SIZE];
  UINT64 claimed = dev->sector_count - 1u; /* MBR counts 512-byte sectors */

  memset(mbr, 0, sizeof(mbr));
  mbr[446 + 1] = 0x00;
  mbr[446 + 2] = 0x02;
  mbr[446 + 3] = 0x00;
  mbr[446 + 4] = 0xee; /* GPT protective */
  mbr[446 + 5] = 0xff;
  mbr[446 + 6] = 0xff;
  mbr[446 + 7] = 0xff;
  wr32(mbr + 446 + 8, 1u);
  wr32(mbr + 446 + 12,
       (claimed > 0xffffffffu) ? 0xffffffffu : (UINT32)claimed);
  mbr[510] = 0x55;
  mbr[511] = 0xaa;
  return blkdev_write(dev, 0, 1, mbr);
}

/* Build a 92-byte header for `my_lba`. `entry_lba` is where the entry
 * array that belongs to this header starts; `alt_lba` is the other
 * header. `array_crc` is the CRC-32 of the entry array. */
static void gpt_build_header(UINT8 *h, UINT64 my_lba, UINT64 alt_lba,
                             UINT64 first_usable, UINT64 last_usable,
                             UINT64 entry_lba, UINT32 array_crc) {
  memset(h, 0, GPT_HEADER_MIN_SIZE);
  memcpy(h, GPT_SIGNATURE, GPT_SIGNATURE_LEN);
  wr32(h + 8, 0x00010000u);  /* revision 1.0 */
  wr32(h + 12, 92u);         /* header size */
  wr32(h + 16, 0u);          /* CRC field, zeroed for the computation */
  wr32(h + 20, 0u);          /* reserved */
  wr64(h + 24, my_lba);
  wr64(h + 32, alt_lba);
  wr64(h + 40, first_usable);
  wr64(h + 48, last_usable);
  memcpy(h + 56, gpt_disk_guid, 16);
  wr64(h + 72, entry_lba);
  wr32(h + 80, GPT_ENTRY_ARRAY_ENTRIES);
  wr32(h + 84, GPT_ENTRY_SIZE);
  wr32(h + 88, array_crc);
  /* Now that the rest of the header is final, fill in its own CRC. */
  wr32(h + 16, novus_rs_crc32(0, h, GPT_HEADER_MIN_SIZE));
}

int gpt_write(blkdev_t *dev, const gpt_part_spec_t *specs, UINTN count) {
  /* 16 KiB: the entry array is the whole point of the reserve, so it is
   * kept out of the stack. */
  static UINT8 array[GPT_ENTRY_ARRAY_SECTORS][BLKDEV_SECTOR_SIZE];
  UINT8       *hdr = gpt_block; /* one whole device block, see gpt_read */
  UINT32       spb = blkdev_sectors_per_block(dev);
  UINT64       total;      /* the device counted in its own blocks */
  UINT64       array_blocks;
  UINT64       first_usable;
  UINT64       last_usable;
  UINT64       backup_array_lba;
  UINT32       array_crc;
  UINTN        i;
  UINTN        slot;

  if (dev == NULL || specs == NULL || spb == 0 ||
      (dev->sector_count % (UINT64)spb) != 0) {
    return -1;
  }
  total = dev->sector_count / (UINT64)spb;
  /* The array is a byte count in the specification; how many blocks it
   * takes depends on the block size - 32 on a 512-byte disk, 4 on 4Kn. */
  array_blocks = ((UINT64)GPT_ENTRY_ARRAY_SECTORS + (UINT64)spb - 1u) /
                 (UINT64)spb;
  /* 1 MBR + 1 header + array, at both ends, plus a little slack. */
  if (total < 2u * (1u + 1u + array_blocks) + 16u) {
    return -1;
  }
  if (count > GPT_ENTRY_ARRAY_ENTRIES) {
    return -1;
  }
  first_usable = 2u + array_blocks;
  last_usable = total - 1u - array_blocks - 1u;
  backup_array_lba = last_usable + 1u;

  /* Range check before anything is written: a partition that does not
   * fit would leave a half-written table behind. A partition that does
   * not start and end on a block boundary is rejected too - on a 4Kn
   * disk such a range is not addressable, so writing it would produce a
   * table no firmware will accept. */
  for (i = 0; i < count; i++) {
    if ((specs[i].first_lba % (UINT64)spb) != 0 ||
        ((specs[i].last_lba + 1u) % (UINT64)spb) != 0) {
      kputs("[GPT ] partition not aligned to the device block size\n");
      return -1;
    }
    if (specs[i].first_lba < blkdev_lba_count(dev, first_usable) ||
        specs[i].last_lba > blkdev_lba_count(dev, last_usable) ||
        specs[i].last_lba < specs[i].first_lba) {
      return -1;
    }
  }

  memset(array, 0, sizeof(array));
  for (slot = 0; slot < count; slot++) {
    UINTN byte_off = slot * GPT_ENTRY_SIZE;
    gpt_fill_entry(&array[byte_off / BLKDEV_SECTOR_SIZE]
                         [byte_off % BLKDEV_SECTOR_SIZE],
                   &specs[slot], slot, spb);
  }
  array_crc = novus_rs_crc32(0, &array[0][0],
                             (UINT64)GPT_ENTRY_ARRAY_ENTRIES *
                                 GPT_ENTRY_SIZE);

  /* Backup copies first: if the write is interrupted the primary header
   * is still the old (or absent) one, and a recovery tool that only
   * looks at the backup sees a consistent table. */
  for (i = 0; i < array_blocks; i++) {
    if (blkdev_write(dev, blkdev_lba_to_sector(dev, backup_array_lba + i),
                     (UINTN)spb, &array[i * spb][0]) != 0) {
      return -1;
    }
  }
  memset(hdr, 0, (UINTN)spb * BLKDEV_SECTOR_SIZE);
  gpt_build_header(hdr, total - 1u, GPT_HEADER_LBA, first_usable,
                   last_usable, backup_array_lba, array_crc);
  if (blkdev_write(dev, blkdev_lba_to_sector(dev, total - 1u), (UINTN)spb,
                   hdr) != 0) {
    return -1;
  }

  for (i = 0; i < array_blocks; i++) {
    if (blkdev_write(dev, blkdev_lba_to_sector(dev, 2u + i), (UINTN)spb,
                     &array[i * spb][0]) != 0) {
      return -1;
    }
  }
  memset(hdr, 0, (UINTN)spb * BLKDEV_SECTOR_SIZE);
  gpt_build_header(hdr, GPT_HEADER_LBA, total - 1u, first_usable,
                   last_usable, 2u, array_crc);
  if (blkdev_write(dev, blkdev_lba_to_sector(dev, GPT_HEADER_LBA),
                   (UINTN)spb, hdr) != 0) {
    return -1;
  }

  return gpt_write_mbr(dev);
}
