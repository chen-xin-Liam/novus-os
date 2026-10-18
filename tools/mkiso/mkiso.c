/*
 * mkiso - build a bootable ISO 9660 image with an El Torito entry.
 *
 * The ISO carries the FAT32 ESP image produced by mkfat twice over, for
 * two different readers:
 *
 *   - embedded, as an El Torito boot image. The catalog points at it, the
 *     firmware loads it as a virtual disk, and from there the boot path is
 *     identical to booting off a real ESP. This is what makes the disc
 *     bootable, and it is not a file anyone can see.
 *
 *   - published, as a directory tree in the ISO 9660 filesystem: the ESP's
 *     own contents - EFI/BOOT/BOOTX64.EFI, SYSTEM.IMG, ESP.IMG - so that
 *     the image is also a folder you can open. A firmware finds its boot
 *     file at \EFI\BOOT\BOOTX64.EFI, and so does anyone who mounts the
 *     image or unpacks it with an archive tool: the guide path is there in
 *     the open, not sealed inside an image file.
 *
 * So this writes a small ISO: one primary volume descriptor, a directory
 * tree of a handful of entries, and the structures that boot it. Novus
 * does not read files out of the ISO, so there is no Joliet and no Rock
 * Ridge - but the tree is written properly enough for any other reader,
 * because an install image that only a firmware can understand is an
 * install image nobody can check.
 *
 * Usage:
 *   mkiso <out.iso> <esp.img> [volume-id] [--file <iso-path> <host-path>]...
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

#define MAX_NODES       64u
#define MAX_NAME        40u

#define ISO_EPOCH_YEAR  1970   /* date fields are written as zeros */

/* Where the embedded ESP begins. 2048-byte ISO sectors, so aligning to
 * eight of them puts the image on a 16 KiB boundary - which is also a
 * 4096-byte one, so the isohybrid partition describing it starts on a
 * block boundary even on a 4Kn disk. */
#define ESP_ALIGN_SECTORS 8u

static uint8_t *image;
static uint64_t total_sectors;
static uint32_t next_lba;
static FILE    *esp_file;
static uint64_t esp_bytes;
static uint64_t esp_sectors;

/* ------------------------------------------------------------------ */
/* The directory tree                                                  */
/* ------------------------------------------------------------------ */
/* Built from the --file arguments, which give a path in the ISO and the
 * host file to take it from. Directories appear only as the parents of
 * files: there is no --dir, because a directory with nothing in it is not
 * something an ISO needs. */
typedef struct node {
  char         name[MAX_NAME]; /* exactly as written into the ISO */
  int          is_dir;
  uint32_t     lba;
  uint32_t     size;           /* bytes; ISO_SECTOR for a directory */
  const char  *host;           /* host file, NULL for directories */
  uint16_t     dir_num;        /* path-table number, directories only */
  struct node *parent;
  struct node *child;          /* first child */
  struct node *next;           /* next sibling */
} node_t;

static node_t  nodes[MAX_NODES];
static uint32_t nnodes;

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

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint8_t *sector(uint64_t lba) {
  if (lba >= total_sectors) {
    die("internal: LBA out of range");
  }
  return image + lba * ISO_SECTOR;
}

static void upper_iso_name(char *dst, size_t cap, const char *src) {
  size_t i;

  for (i = 0; src[i] != '\0' && i + 1u < cap; i++) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    dst[i] = c;
  }
  dst[i] = '\0';
}

static node_t *new_node(const char *name, int is_dir, node_t *parent) {
  node_t *n;

  if (nnodes >= MAX_NODES) {
    die("too many files");
  }
  n = &nodes[nnodes++];
  memset(n, 0, sizeof(*n));
  upper_iso_name(n->name, sizeof(n->name), name);
  n->is_dir = is_dir;
  n->parent = parent;
  n->size = is_dir ? ISO_SECTOR : 0u;
  return n;
}

/* Sorted insertion: ISO 9660 wants a directory's records ordered, and
 * keeping the children sorted also makes the record order agree with the
 * path table order, which is what a reader checking the two against each
 * other expects. */
static node_t *add_child(node_t *dir, const char *name, int is_dir) {
  node_t **link = &dir->child;
  node_t  *n;

  while (*link != NULL) {
    if (strcmp((*link)->name, name) == 0) {
      return *link;
    }
    if (strcmp((*link)->name, name) > 0) {
      break;
    }
    link = &(*link)->next;
  }
  n = new_node(name, is_dir, dir);
  n->next = *link;
  *link = n;
  return n;
}

/* "EFI/BOOT/BOOTX64.EFI" -> two directories and a file. The file gets the
 * ";1" version suffix ISO 9660 puts on every file name. */
static void add_file(const char *iso_path, const char *host) {
  node_t     *dir = &nodes[0];
  const char *p = iso_path;

  if (*p == '/') {
    p++;
  }
  for (;;) {
    const char *slash = strchr(p, '/');
    size_t      n = slash != NULL ? (size_t)(slash - p) : strlen(p);
    char        comp[MAX_NAME];

    if (n == 0u || n >= sizeof(comp)) {
      die("bad ISO path");
    }
    memcpy(comp, p, n);
    comp[n] = '\0';

    if (slash == NULL) {
      char     fname[MAX_NAME + 2u];
      size_t   len = strlen(comp);
      node_t  *f;

      if (len + 2u > sizeof(fname)) {
        die("file name too long");
      }
      memcpy(fname, comp, len);
      fname[len] = ';';
      fname[len + 1u] = '1';
      fname[len + 2u] = '\0';
      f = add_child(dir, fname, 0);
      f->host = host;
      return;
    }
    dir = add_child(dir, comp, 1);
    p = slash + 1;
  }
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */
/* Directories first, then file data, both in depth-first order - which
 * is also the order the path table is written in, so a directory's number
 * is always lower than its children's. */
static void assign_dirs(node_t *d, uint16_t *num) {
  node_t *c;

  d->dir_num = *num;
  (*num)++;
  if (d->parent == NULL) {
    d->lba = ROOT_DIR_LBA;
  } else {
    d->lba = next_lba++;
  }
  for (c = d->child; c != NULL; c = c->next) {
    if (c->is_dir) {
      assign_dirs(c, num);
    }
  }
}

static void assign_files(node_t *d) {
  node_t *c;

  for (c = d->child; c != NULL; c = c->next) {
    if (c->is_dir) {
      assign_files(c);
      continue;
    }
    {
      FILE *fp = fopen(c->host, "rb");
      long  end;

      if (fp == NULL) {
        die("cannot open a file named in the ISO");
      }
      fseek(fp, 0, SEEK_END);
      end = ftell(fp);
      fclose(fp);
      if (end < 0 || (uint64_t)end > 0xffffffffu) {
        die("cannot size a file named in the ISO");
      }
      c->size = (uint32_t)end;
    }
    c->lba = next_lba;
    next_lba += (c->size + ISO_SECTOR - 1u) / ISO_SECTOR;
  }
}

/* The path table: one entry per directory, parents before children. */
static uint32_t path_table_size(const node_t *d) {
  const node_t *c;
  uint32_t      n = 0;

  if (d->parent == NULL) {
    n = 10u; /* root: one byte of name, which is the 0x00 filler */
  } else {
    n = 8u + (uint32_t)strlen(d->name);
    if ((strlen(d->name) & 1u) == 0u) {
      n += 1u; /* pad to even */
    }
  }
  for (c = d->child; c != NULL; c = c->next) {
    if (c->is_dir) {
      n += path_table_size(c);
    }
  }
  return n;
}

/* ------------------------------------------------------------------ */
/* ISO 9660 structures                                                 */
/* ------------------------------------------------------------------ */
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

static void write_pvd(const char *volume_id, uint32_t path_size) {
  uint8_t *p = sector(PVD_LBA);
  uint32_t off;

  memset(p, 0, ISO_SECTOR);
  p[0] = 1; /* primary volume descriptor */
  memcpy(p + 1, "CD001", 5);
  p[6] = 1;
  memset(p + 7, 0, 32); /* system identifier */
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

  put_le32(p + 132, path_size);
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

static void write_path_table_entry(uint8_t *p, uint32_t *off,
                                   const node_t *d, int big_endian) {
  uint32_t name_len = (d->parent == NULL) ? 1u : (uint32_t)strlen(d->name);
  uint32_t o = *off;

  p[o] = (uint8_t)name_len;
  p[o + 1u] = 0; /* extended attribute record length */
  if (big_endian) {
    put_be32(p + o + 2u, d->lba);
    put_be16(p + o + 6u, d->parent != NULL ? d->parent->dir_num : d->dir_num);
  } else {
    put_le32(p + o + 2u, d->lba);
    put_le16(p + o + 6u, d->parent != NULL ? d->parent->dir_num : d->dir_num);
  }
  if (d->parent == NULL) {
    p[o + 8u] = 0x00; /* root's name is a single zero byte */
  } else {
    memcpy(p + o + 8u, d->name, strlen(d->name));
  }
  o += 8u + name_len;
  if ((name_len & 1u) == 0u) {
    p[o] = 0;
    o += 1u;
  }
  *off = o;
}

static void write_path_tables_node(const node_t *d, uint8_t *le, uint8_t *be,
                                   uint32_t *off) {
  const node_t *c;

  write_path_table_entry(le, off, d, 0);
  {
    uint32_t o = *off;
    write_path_table_entry(be, &o, d, 1);
  }
  for (c = d->child; c != NULL; c = c->next) {
    if (c->is_dir) {
      write_path_tables_node(c, le, be, off);
    }
  }
}

static void write_path_tables(void) {
  uint8_t *le = sector(PATH_TABLE_LBA);
  uint8_t *be = sector(PATH_TABLE_M_LBA);
  uint32_t off = 0;

  memset(le, 0, ISO_SECTOR);
  memset(be, 0, ISO_SECTOR);
  write_path_tables_node(&nodes[0], le, be, &off);
}

/* One directory: ".", "..", then its children in order. */
static void write_dir(const node_t *d) {
  uint8_t *p = sector(d->lba);
  uint32_t off = 0;
  uint8_t  dot[1];
  const node_t *c;

  dot[0] = 0x00;
  memset(p, 0, ISO_SECTOR);
  off += write_dir_record(p + off, d->lba, ISO_SECTOR, 0x02, dot, 1);
  off += write_dir_record(p + off,
                          d->parent != NULL ? d->parent->lba : d->lba,
                          ISO_SECTOR, 0x02, dot, 1);
  for (c = d->child; c != NULL; c = c->next) {
    off += write_dir_record(p + off, c->lba, c->size,
                            c->is_dir ? 0x02u : 0x00u,
                            (const uint8_t *)c->name,
                            (uint32_t)strlen(c->name));
  }
}

static void write_dirs(const node_t *d) {
  const node_t *c;

  write_dir(d);
  for (c = d->child; c != NULL; c = c->next) {
    if (c->is_dir) {
      write_dirs(c);
    }
  }
}

/* File data, copied in whole sectors with the tail zero-filled: an ISO
 * file is a run of sectors, so the last one is padded rather than left
 * short. */
static void copy_files(const node_t *d) {
  const node_t *c;

  for (c = d->child; c != NULL; c = c->next) {
    if (c->is_dir) {
      copy_files(c);
      continue;
    }
    {
      FILE   *fp = fopen(c->host, "rb");
      uint64_t remaining = c->size;
      uint32_t lba = c->lba;

      if (fp == NULL) {
        die("cannot open a file named in the ISO");
      }
      while (remaining > 0) {
        size_t chunk =
            (remaining > ISO_SECTOR) ? ISO_SECTOR : (size_t)remaining;
        if (fread(sector(lba), 1, chunk, fp) != chunk) {
          die("short read on a file named in the ISO");
        }
        if (chunk < ISO_SECTOR) {
          memset(sector(lba) + chunk, 0, ISO_SECTOR - chunk);
        }
        remaining -= chunk;
        lba++;
      }
      fclose(fp);
    }
  }
}

/* El Torito boot catalog.
 *
 * Three entries, and the platform id is what makes a UEFI firmware see
 * the disc as bootable at all:
 *
 *   0  validation entry       header 0x01, platform 0x00, 0x55/0xAA
 *  32  section header         header 0x91, platform 0xEF  <- UEFI looks
 *                             for this platform id; without it the
 *                             firmware sees no EFI boot image and
 *                             reports "no bootable option"
 *  64  section entry          bootable, no emulation, the ESP image
 *
 * No emulation is what the UEFI specification describes for an EFI boot
 * image and what every distribution's install media uses: the firmware
 * loads the image as a virtual disk in its own right, rather than
 * pretending it is a floppy or a hard disk.
 *
 * The loader does not rely on being handed a filesystem for it either: it
 * reads the ESP through the firmware's SimpleFileSystem when one is
 * there, and falls back to its own FAT reader over block I/O when one is
 * not - and it was itself loaded out of that image, so at least one of
 * the two is guaranteed to work.
 *
 * The validation checksum is the negated sum of the entry's 16 u16s, so
 * that the whole entry adds up to zero. */
/* One section entry: bootable, `media` emulation, pointing at the image.
 *
 * `sector_count` is in 512-byte units because that is the sector size of
 * the virtual disk the firmware builds out of the image - the ESP is a
 * FAT volume with 512-byte sectors. Passing the ISO's 2048-byte sector
 * count here would tell the firmware the disk is a quarter of its real
 * size, which is a classic "boots, then cannot see its own filesystem"
 * failure. */
static void write_section_entry(uint8_t *p, uint8_t media, uint32_t boot_lba,
                                uint32_t sector_count) {
  p[0] = 0x88;   /* boot indicator: bootable */
  p[1] = media;  /* 0x00 no emulation, 0x04 hard-disk emulation */
  put_le16(p + 2, 0); /* load segment */
  p[4] = 0x00;        /* system type */
  p[5] = 0x00;
  p[6] = (uint8_t)(sector_count & 0xffu);
  p[7] = (uint8_t)((sector_count >> 8) & 0xffu);
  put_le32(p + 8, boot_lba);
  memset(p + 12, 0, 20);
}

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

  /* Section header: platform EFI, one entry follows, and it is the last
   * section in the catalog. */
  p[32] = 0x91; /* section header, final */
  p[33] = 0xef; /* platform: EFI */
  put_le16(p + 34, 1); /* number of section entries */
  memset(p + 36, ' ', 28);
  memcpy(p + 36, "EFI", 3);

  /* Section entry: bootable, no emulation, the ESP image.
   *
   * No emulation and nothing else. The image is a superfloppy FAT
   * volume - its first sector is a FAT boot sector, not a partition
   * table - so it cannot be offered as hard-disk emulation as well:
   * a firmware that takes that path reads LBA 0 looking for a partition
   * table, finds a boot sector instead, and reports no bootable option
   * on a medium that is in fact perfectly good. Offering an entry that
   * is guaranteed to fail on the firmwares that pick it is worse than
   * offering one entry that every UEFI firmware handles. */
  write_section_entry(p + 64, 0x00, boot_lba, boot_sectors);
}

/* A partition table in the ISO's system area, which the first 32 KiB of
 * an ISO 9660 image leaves unused - and which is exactly the first
 * sector of the medium once the image has been written straight onto a
 * USB stick.
 *
 * That is the whole point: an ISO written to a stick is not a CD any
 * more, and a firmware that looks at a stick sees a disk and looks for a
 * partition table. Without this sector such a stick has no ESP on it as
 * far as the firmware can tell, so it offers no boot option at all -
 * which is most of the "this ISO does not boot on my machine" reports,
 * because a USB stick is how these images are actually used.
 *
 * One entry: bootable, type 0xEF (EFI system partition), covering the
 * embedded ESP image at its byte offset in the image. The CHS fields are
 * the conventional "start at 0/0/1, end at max" pair; anything reading
 * this uses the LBA fields. There is no legacy boot code because Novus
 * is UEFI-only: a firmware that starts this sector as 16-bit code finds
 * a message instead of a stub that would hang. */
static void write_isohybrid_mbr(uint32_t image_lba, uint32_t image_sectors) {
  uint8_t *p = sector(0);
  static const char note[] = "Novus: UEFI only. This image has no legacy "
                             "boot code.";

  memset(p, 0, 512);
  memcpy(p + 0x03, note, sizeof(note) - 1u);

  p[446] = 0x80; /* bootable */
  p[447] = 0x00; /* start head */
  p[448] = 0x01; /* start sector/cylinder */
  p[449] = 0x00; /* start cylinder (low) */
  p[450] = 0xef; /* type: EFI system partition */
  p[451] = 0xfe; /* end head */
  p[452] = 0xff; /* end sector/cylinder */
  p[453] = 0xff; /* end cylinder (low) */
  put_le32(p + 454, image_lba);
  put_le32(p + 458, image_sectors);

  p[510] = 0x55;
  p[511] = 0xaa;
}

/* ------------------------------------------------------------------ */
/* Verification                                                        */
/* ------------------------------------------------------------------ */
/* Look a name up in a directory record. Returns the entry's extent and
 * size, or 0 if it is not there. */
static int dir_find(const uint8_t *dir, const char *name, uint32_t *extent,
                    uint32_t *size, uint8_t *flags) {
  uint32_t   off = 0;
  uint32_t   want = (uint32_t)strlen(name);

  while (off + 33u <= ISO_SECTOR && dir[off] != 0u) {
    uint32_t nlen = dir[off + 32];

    if (off + 33u + nlen <= ISO_SECTOR && nlen == want &&
        memcmp(dir + off + 33, name, nlen) == 0) {
      *extent = read_le32(dir + off + 2);
      *size = read_le32(dir + off + 10);
      *flags = dir[off + 25];
      return 1;
    }
    off += (uint32_t)dir[off];
  }
  return 0;
}

/* Check the finished image against what its own structures claim, before
 * the file is written out.
 *
 * Everything above is written blind into a buffer: an offset that is off
 * by a few bytes still produces a file that looks finished, and the
 * machine that discovers it does not boot is the user's. The point of
 * these checks is that a catalog pointer that lands on the wrong sector,
 * a missing 0x55/0xAA, a checksum that does not add up, a boot entry
 * that does not point at the ESP image, or a boot file that is not where
 * a firmware looks for it all fail here, at build time, with the reason
 * on the terminal - instead of on someone's laptop as "no bootable
 * device". */
static void verify_image(uint32_t catalog_lba, uint32_t boot_lba,
                         uint32_t boot_sectors) {
  const uint8_t *brvd = sector(BRVD_LBA);
  const uint8_t *cat = sector(catalog_lba);
  const uint8_t *mbr = sector(0);
  const uint8_t *esp = sector(boot_lba);
  uint32_t       sum = 0;
  uint32_t       i;

  if (brvd[0] != 0u || memcmp(brvd + 1, "CD001", 5) != 0 ||
      memcmp(brvd + 7, "EL TORITO SPECIFICATION", 23) != 0) {
    die("boot record volume descriptor is malformed");
  }
  if (read_le32(brvd + 71) != catalog_lba || catalog_lba == 0u) {
    die("the boot catalog pointer does not reach the catalog");
  }

  if (cat[0] != 0x01u || cat[30] != 0x55u || cat[31] != 0xaau) {
    die("the boot catalog has no validation entry");
  }
  for (i = 0; i < 16u; i++) {
    sum += (uint32_t)cat[i * 2] | ((uint32_t)cat[i * 2 + 1] << 8);
  }
  if ((sum & 0xffffu) != 0u) {
    die("the boot catalog checksum does not add up to zero");
  }
  if (cat[32] != 0x91u || cat[33] != 0xefu) {
    die("the boot catalog has no EFI platform section");
  }
  if (cat[64] != 0x88u || cat[65] != 0x00u) {
    die("the boot entry is not a bootable no-emulation image");
  }
  if (read_le32(cat + 64 + 8) != boot_lba ||
      (uint32_t)(cat[64 + 6] | ((uint32_t)cat[64 + 7] << 8)) !=
          boot_sectors) {
    die("the boot entry does not point at the ESP image");
  }

  /* The sector the catalog names really holds a FAT volume. */
  if (esp[510] != 0x55u || esp[511] != 0xaau) {
    die("the sector the catalog points at is not a bootable FAT volume");
  }

  /* And the boot file is where a firmware looks for it. This is the
   * check for the other half of the image: the copy of the ESP that is
   * published as a directory tree, that anyone can open, and that a
   * firmware finding \EFI\BOOT\BOOTX64.EFI relies on. */
  {
    uint32_t lba = 0;
    uint32_t size = 0;
    uint8_t  flags = 0;

    if (!dir_find(sector(ROOT_DIR_LBA), "EFI", &lba, &size, &flags) ||
        (flags & 0x02u) == 0u) {
      die("the ISO has no /EFI directory");
    }
    if (!dir_find(sector(lba), "BOOT", &lba, &size, &flags) ||
        (flags & 0x02u) == 0u) {
      die("the ISO has no /EFI/BOOT directory");
    }
    if (!dir_find(sector(lba), "BOOTX64.EFI;1", &lba, &size, &flags) ||
        (flags & 0x02u) != 0u) {
      die("the ISO has no /EFI/BOOT/BOOTX64.EFI");
    }
    if (size == 0u || sector(lba)[0] != 'M' || sector(lba)[1] != 'Z') {
      die("/EFI/BOOT/BOOTX64.EFI is not a PE image");
    }
  }

  /* The isohybrid table is what makes the same file bootable off a USB
   * stick, where it is no longer a disc. It has to cover the image at
   * its byte offset in the file, in 512-byte sectors. */
  if (mbr[510] != 0x55u || mbr[511] != 0xaau || mbr[450] != 0xefu ||
      read_le32(mbr + 454) != boot_lba * (ISO_SECTOR / 512u) ||
      read_le32(mbr + 458) != boot_sectors) {
    die("the isohybrid partition table does not cover the ESP image");
  }
}

static void report_tree(const node_t *d, const char *prefix) {
  const node_t *c;

  for (c = d->child; c != NULL; c = c->next) {
    if (c->is_dir) {
      char sub[MAX_NAME + 32u];
      snprintf(sub, sizeof(sub), "%s%s/", prefix, c->name);
      printf("mkiso:   %s%s/ (directory)\n", prefix, c->name);
      report_tree(c, sub);
    } else {
      printf("mkiso:   %s%s  %u bytes\n", prefix, c->name, c->size);
    }
  }
}

int main(int argc, char **argv) {
  const char *volume_id = "NOVUS_LIVE";
  uint32_t catalog_lba;
  uint32_t boot_lba;
  uint32_t units; /* size of the embedded ESP in 512-byte units */
  uint16_t dir_num = 1;
  int      i;

  if (argc < 3) {
    fprintf(stderr, "usage: mkiso <out.iso> <esp.img> [volume-id] "
                    "[--file <iso-path> <host-path>]...\n");
    return 2;
  }
  memset(nodes, 0, sizeof(nodes));
  nnodes = 1; /* nodes[0] is the root */

  if (argc >= 4 && strcmp(argv[3], "--file") != 0) {
    volume_id = argv[3];
    i = 4;
  } else {
    i = 3;
  }
  for (; i < argc; i++) {
    if (strcmp(argv[i], "--file") == 0 && i + 2 < argc) {
      add_file(argv[i + 1], argv[i + 2]);
      i += 2;
    } else {
      fprintf(stderr, "mkiso: unexpected argument '%s'\n", argv[i]);
      return 2;
    }
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

  /* Layout: descriptors and tables, the directory tree, the published
   * files, then the embedded ESP on an aligned boundary, the catalog,
   * and 150 sectors of slack so a slightly larger ESP still fits without
   * recomputing anything. */
  next_lba = ROOT_DIR_LBA + 1u;
  assign_dirs(&nodes[0], &dir_num);
  assign_files(&nodes[0]);
  boot_lba = (next_lba + (ESP_ALIGN_SECTORS - 1u)) &
             ~(uint32_t)(ESP_ALIGN_SECTORS - 1u);
  catalog_lba = boot_lba + (uint32_t)esp_sectors;
  total_sectors = (uint64_t)catalog_lba + 1u + 150u;

  image = calloc(1, (size_t)(total_sectors * ISO_SECTOR));
  if (image == NULL) {
    die("out of memory");
  }

  write_pvd(volume_id, path_table_size(&nodes[0]));
  write_boot_record(catalog_lba);
  write_terminator();
  write_path_tables();
  write_dirs(&nodes[0]);
  copy_files(&nodes[0]);

  {
    uint64_t remaining = esp_bytes;
    uint64_t lba = boot_lba;

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

  /* The catalog's sector count is in 512-byte units, because that is the
   * sector size of the virtual disk the firmware sees - the ESP image is
   * a FAT32 volume with 512-byte sectors. Passing the ISO's 2048-byte
   * sector count here would tell the firmware the disk is a quarter of
   * its real size. */
  units = (uint32_t)((esp_bytes + 511u) / 512u);
  write_catalog(catalog_lba, boot_lba, units);
  /* The MBR counts in 512-byte sectors too, and boot_lba counts in
   * 2048-byte ones. */
  write_isohybrid_mbr(boot_lba * (ISO_SECTOR / 512u), units);

  /* Now that everything is in place: check the boot structures against
   * what they claim, and refuse to write a file that would not boot. */
  verify_image(catalog_lba, boot_lba, units);

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

  /* Report the parts that decide whether the result boots, not just its
   * size: what the image contains, where the catalog is, what the boot
   * entry names, and where the isohybrid table says the ESP lives once
   * the image is on a stick. */
  printf("mkiso: %s  %llu sectors\n", argv[1],
         (unsigned long long)total_sectors);
  report_tree(&nodes[0], "");
  printf("mkiso:   boot catalog LBA %u, boot entry: no emulation, "
         "embedded ESP at LBA %u, %u x 512B\n",
         catalog_lba, boot_lba, units);
  printf("mkiso:   isohybrid MBR: partition 1 type 0xEF at LBA %u, %u "
         "sectors (USB stick boot)\n",
         boot_lba * (ISO_SECTOR / 512u), units);
  free(image);
  return 0;
}
