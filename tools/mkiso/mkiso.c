/*
 * mkiso - build a bootable ISO 9660 image with an El Torito entry.
 *
 * The ISO is only a carrier. Its single job is to hand the firmware the
 * FAT32 ESP image produced by mkfat: the El Torito boot catalog points
 * at that image, the firmware loads it as a virtual disk, and from
 * there on the boot path is identical to booting from a real ESP. So
 * this file writes a minimal ISO - one primary volume descriptor, one
 * root directory holding the ESP image, one path-table entry - and
 * nothing else. Novus does not read files out of the ISO, which means
 * no directory tree walk, no Joliet and no Rock Ridge.
 *
 * Usage:
 *   mkiso <out.iso> <esp.img> [volume-id]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ISO_SECTOR      2048u
#define SYSTEM_AREA     16u    /* sectors 0..15, unused */
#define PVD_LBA         16u
#define BRVD_LBA        17u    /* boot record volume descriptor */
#define TERM_LBA        18u
#define PATH_TABLE_LBA  19u    /* little-endian table */
#define PATH_TABLE_M_LBA 20u   /* big-endian table */
#define ROOT_DIR_LBA    21u
#define FILE_LBA        22u    /* first sector after the root record */

#define ISO_EPOCH_YEAR  1970   /* date fields are written as zeros */

static uint8_t *image;
static uint64_t total_sectors;
static FILE    *esp_file;
static uint64_t esp_bytes;
static uint64_t esp_sectors;

static void die(const char *msg) {
  fprintf(stderr, "mkiso: %s\n", msg);
  exit(1);
}

static void put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xffu);
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)((v >> 24) & 0xffu);
  p[1] = (uint8_t)((v >> 16) & 0xffu);
  p[2] = (uint8_t)((v >> 8) & 0xffu);
  p[3] = (uint8_t)(v & 0xffu);
}

static uint8_t *sector(uint64_t lba) {
  if (lba >= total_sectors) {
    die("internal: LBA out of range");
  }
  return image + lba * ISO_SECTOR;
}

/* One directory record. `name` is written verbatim; the caller supplies
 * the 0x00 byte for "." and "..", and ";1" for file version. */
static uint32_t write_dir_record(uint8_t *p, uint32_t extent, uint32_t size,
                                 uint8_t flags, const uint8_t *name,
                                 uint32_t name_len) {
  p[0] = (uint8_t)(33u + name_len); /* record length */
  p[1] = 0;                         /* extended attribute record length */
  put_le32(p + 2, extent);
  put_be32(p + 6, extent);
  put_le32(p + 10, size);
  put_be32(p + 14, size);
  memset(p + 18, 0, 7); /* recording date: zero means "unspecified" */
  p[25] = flags;
  p[26] = 0; /* file unit size */
  p[27] = 0; /* interleave gap */
  put_le16(p + 28, 1);
  put_be16(p + 30, 1);
  p[32] = (uint8_t)name_len;
  memcpy(p + 33, name, name_len);
  /* Records are padded to an even length. */
  if (((33u + name_len) & 1u) != 0u) {
    p[33u + name_len] = 0;
    return 34u + name_len;
  }
  return 33u + name_len;
}

static void write_pvd(const char *volume_id) {
  uint8_t *p = sector(PVD_LBA);
  uint32_t off;

  memset(p, 0, ISO_SECTOR);
  p[0] = 1; /* primary volume descriptor */
  memcpy(p + 1, "CD001", 5);
  p[6] = 1; /* version */

  memset(p + 8, ' ', 32);
  memcpy(p + 8, "NOVUS", 5);  /* system identifier */
  memset(p + 40, ' ', 32);
  {
    size_t n = strlen(volume_id);
    if (n > 32) {
      n = 32;
    }
    memcpy(p + 40, volume_id, n);
  }

  put_le32(p + 80, (uint32_t)total_sectors);
  put_be32(p + 84, (uint32_t)total_sectors);

  memset(p + 88, 0, 32); /* escape sequences: none */

  put_le16(p + 120, 1);
  put_be16(p + 122, 1);
  put_le16(p + 124, 1);
  put_be16(p + 126, 1);
  put_le16(p + 128, ISO_SECTOR);
  put_be16(p + 130, ISO_SECTOR);

  put_le32(p + 132, 10u); /* path table size: one root entry == 10 bytes */
  put_le32(p + 136, PATH_TABLE_LBA);
  put_le32(p + 140, 0);
  put_be32(p + 144, PATH_TABLE_M_LBA);
  put_be32(p + 148, 0);

  /* Root directory record occupying bytes 156..189. */
  off = 156u;
  {
    uint8_t name[1];
    name[0] = 0x00;
    (void)write_dir_record(p + off, ROOT_DIR_LBA, ISO_SECTOR, 0x02, name, 1);
  }

  memset(p + 190, ' ', 128);
  memcpy(p + 190, "NOVUS", 5); /* volume set identifier */
  memset(p + 318, ' ', 128);
  memcpy(p + 318, "NOVUS", 5); /* publisher */
  memset(p + 446, ' ', 128);
  memcpy(p + 446, "MKISO", 5); /* data preparer */
  memset(p + 574, ' ', 128);
  memcpy(p + 574, "MKISO", 5); /* application */
  memset(p + 702, ' ', 37);
  memcpy(p + 702, "NOVUS.ISO", 9); /* abstract file */
  memset(p + 739, ' ', 37);
  memcpy(p + 739, "NOVUS.ISO", 9); /* bibliographic file */

  /* Dates: 17 bytes of digits plus a timezone, all zero is legal. */
  memset(p + 813, '0', 16);
  p[829] = 0;
  memset(p + 830, '0', 16);
  p[846] = 0;
  memset(p + 847, '0', 16);
  p[863] = 0;
  memset(p + 864, '0', 16);
  p[880] = 0;

  p[881] = 1; /* file structure version */
}

static void write_boot_record(uint32_t catalog_lba) {
  uint8_t *p = sector(BRVD_LBA);

  memset(p, 0, ISO_SECTOR);
  p[0] = 0; /* boot record volume descriptor */
  memcpy(p + 1, "CD001", 5);
  p[6] = 1;
  memset(p + 7, ' ', 32);
  memcpy(p + 7, "EL TORITO SPECIFICATION", 23);
  memset(p + 39, 0, 32);
  put_le32(p + 71, catalog_lba);
}

static void write_terminator(void) {
  uint8_t *p = sector(TERM_LBA);

  memset(p, 0, ISO_SECTOR);
  p[0] = 255;
  memcpy(p + 1, "CD001", 5);
  p[6] = 1;
}

/* El Torito boot catalog.
 *
 * Three entries, and the middle one is the whole reason a UEFI firmware
 * will boot this disc:
 *
 *   0  validation entry      header 0x01, platform 0x00
 *  32  section header        header 0x91, platform 0xEF  <- UEFI looks
 *                            for this platform id; without it the
 *                            firmware sees no EFI boot image at all and
 *                            reports "no bootable option"
 *  64  section entry         bootable, no emulation, points at the ESP
 *
 * The section entry's media type is "no emulation" because the ESP image
 * is not a floppy or hard-disk image: the firmware reads it as a
 * filesystem and starts EFI/BOOT/BOOTX64.EFI out of it.
 *
 * The validation checksum is the negated sum of the entry's 16 u16s. */
static void write_catalog(uint32_t catalog_lba, uint32_t boot_lba,
                          uint32_t boot_sectors) {
  uint8_t *p = sector(catalog_lba);
  uint32_t sum = 0;
  uint32_t i;

  memset(p, 0, ISO_SECTOR);

  /* Validation entry. */
  p[0] = 0x01; /* header id */
  p[1] = 0x00; /* platform: 0 (x86/BIOS), the neutral choice */
  put_le16(p + 2, 0);
  memset(p + 4, ' ', 24);
  memcpy(p + 4, "NOVUS", 5);
  p[30] = 0x55;
  p[31] = 0xaa;
  for (i = 0; i < 16; i++) {
    sum += (uint32_t)p[i * 2] | ((uint32_t)p[i * 2 + 1] << 8);
  }
  put_le16(p + 28, (uint16_t)(0x10000u - (sum & 0xffffu)));

  /* Section header entry: last one, platform EFI, one entry follows. */
  p[32] = 0x91; /* section header, final */
  p[33] = 0xef; /* platform: EFI */
  put_le16(p + 34, 1); /* number of section entries */
  memset(p + 36, ' ', 28);
  memcpy(p + 36, "EFI", 3);

  /* Section entry: bootable, no emulation, the ESP image. */
  p[64] = 0x88; /* boot indicator: bootable */
  p[65] = 0x00; /* media type: no emulation */
  put_le16(p + 66, 0); /* load segment: unused without emulation */
  p[68] = 0x00;        /* system type: x86 */
  p[69] = 0x00;
  put_le16(p + 70, (uint16_t)(boot_sectors > 0xffffu ? 0xffffu : boot_sectors));
  put_le32(p + 72, boot_lba);
  memset(p + 76, 0, 20);
}

static void write_path_tables(void) {
  uint8_t *le = sector(PATH_TABLE_LBA);
  uint8_t *be = sector(PATH_TABLE_M_LBA);

  memset(le, 0, ISO_SECTOR);
  le[0] = 1;                    /* name length: root's 0x00 byte */
  put_le32(le + 2, ROOT_DIR_LBA);
  put_le16(le + 6, 1);          /* parent directory number */
  le[8] = 0x00;                 /* name */
  le[9] = 0;                    /* padding to even */

  memset(be, 0, ISO_SECTOR);
  be[0] = 1;
  put_be32(be + 2, ROOT_DIR_LBA);
  put_be16(be + 6, 1);
  be[8] = 0x00;
  be[9] = 0;
}

/* Root directory: ".", ".." and the ESP image. */
static void write_root_dir(void) {
  uint8_t *p = sector(ROOT_DIR_LBA);
  uint32_t off = 0;

  memset(p, 0, ISO_SECTOR);
  {
    uint8_t dot[1];
    dot[0] = 0x00;
    off += write_dir_record(p + off, ROOT_DIR_LBA, ISO_SECTOR, 0x02, dot, 1);
    off += write_dir_record(p + off, ROOT_DIR_LBA, ISO_SECTOR, 0x02, dot, 1);
  }
  {
    const char *fname = "ESP.IMG;1";
    uint32_t size = (esp_bytes > 0xffffffffu) ? 0xffffffffu
                                              : (uint32_t)esp_bytes;
    off += write_dir_record(p + off, FILE_LBA, size, 0x00,
                            (const uint8_t *)fname, (uint32_t)strlen(fname));
  }
  (void)off;
}

int main(int argc, char **argv) {
  const char *volume_id = "NOVUS_LIVE";
  uint32_t catalog_lba;
  uint64_t reserved;

  if (argc < 3) {
    fprintf(stderr, "usage: mkiso <out.iso> <esp.img> [volume-id]\n");
    return 2;
  }
  if (argc >= 4) {
    volume_id = argv[3];
  }

  esp_file = fopen(argv[2], "rb");
  if (esp_file == NULL) {
    die("cannot open ESP image");
  }
  {
    long end;
    fseek(esp_file, 0, SEEK_END);
    end = ftell(esp_file);
    fseek(esp_file, 0, SEEK_SET);
    if (end < 0) {
      die("cannot size ESP image");
    }
    esp_bytes = (uint64_t)end;
  }
  esp_sectors = (esp_bytes + ISO_SECTOR - 1u) / ISO_SECTOR;

  /* Layout: descriptors and tables, catalog, root, file, then 150
   * sectors of slack so a slightly larger ESP still fits without
   * recomputing anything. */
  reserved = FILE_LBA + esp_sectors + 150u;
  total_sectors = reserved;
  catalog_lba = FILE_LBA + esp_sectors;

  image = calloc(1, (size_t)(total_sectors * ISO_SECTOR));
  if (image == NULL) {
    die("out of memory");
  }

  write_pvd(volume_id);
  write_boot_record(catalog_lba);
  write_terminator();
  write_path_tables();
  write_root_dir();
  /* The catalog's sector count is in 512-byte units, because that is the
   * sector size of the virtual disk the firmware sees - the ESP image is
   * a FAT32 volume with 512-byte sectors. Passing the ISO's 2048-byte
   * sector count here would tell the firmware the disk is a quarter of
   * its real size. */
  {
    uint32_t units = (uint32_t)((esp_bytes + 511u) / 512u);

    write_catalog(catalog_lba, FILE_LBA, units);
  }

  {
    uint64_t remaining = esp_bytes;
    uint64_t lba = FILE_LBA;

    while (remaining > 0) {
      size_t chunk = (remaining > ISO_SECTOR) ? ISO_SECTOR : (size_t)remaining;
      if (fread(sector(lba), 1, chunk, esp_file) != chunk) {
        die("short read on ESP image");
      }
      if (chunk < ISO_SECTOR) {
        memset(sector(lba) + chunk, 0, ISO_SECTOR - chunk);
      }
      remaining -= chunk;
      lba++;
    }
  }
  fclose(esp_file);

  {
    FILE *out = fopen(argv[1], "wb");
    if (out == NULL) {
      die("cannot create output ISO");
    }
    if (fwrite(image, 1, (size_t)(total_sectors * ISO_SECTOR), out) !=
        total_sectors * ISO_SECTOR) {
      die("short write");
    }
    fclose(out);
  }

  printf("mkiso: %s  %llu sectors, ESP at LBA %u (%llu bytes)\n", argv[1],
         (unsigned long long)total_sectors, FILE_LBA,
         (unsigned long long)esp_bytes);
  free(image);
  return 0;
}
