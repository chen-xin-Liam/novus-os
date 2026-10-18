/*
 * mkfat - build a FAT16/FAT32 EFI system partition image.
 *
 * Novus ships its boot files (BOOTX64.EFI, the NovusFS system image and
 * the *.nkm modules) on a FAT ESP because that is the one filesystem
 * every UEFI firmware can read without a driver of ours. mkfat writes
 * the bytes of that partition directly.
 *
 * The generated FAT type is chosen automatically:
 *   - FAT16 for images small enough that the cluster count stays below
 *     the FAT32 minimum (65525 clusters). This keeps the install template
 *     under 32 MiB, which is the largest size the El Torito hard-disk
 *     emulation sector-count field can describe.
 *   - FAT32 for larger images.
 *
 * Scope, deliberately: 512-byte sectors, one sector per cluster, two
 * FATs, no long file names. Every file the ESP carries is addressed by
 * its 8.3 name.
 *
 * Usage:
 *   mkfat <out.img> <size-mb> <src>:<dest> [<src>:<dest> ...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define SECTOR_SIZE      512u
#define SECTORS_PER_CLUS 1u
#define CLUSTER_SIZE     (SECTOR_SIZE * SECTORS_PER_CLUS)
#define RESERVED_SECTORS 32u
#define NUM_FATS         2u
#define DIRENT_SIZE      32u
#define DIRENTS_PER_CLUS (CLUSTER_SIZE / DIRENT_SIZE)
#define ROOT_DIRENTS     512u        /* FAT16 fixed root directory */
#define ROOT_DIR_SECTORS (ROOT_DIRENTS / (SECTOR_SIZE / DIRENT_SIZE))
#define SECTORS_PER_TRACK 63u
#define HEADS             255u

#define ATTR_READ_ONLY 0x01u
#define ATTR_HIDDEN    0x02u
#define ATTR_SYSTEM    0x04u
#define ATTR_VOLUME_ID 0x08u
#define ATTR_DIRECTORY 0x10u
#define ATTR_ARCHIVE   0x20u
#define ATTR_LONG_NAME 0x0Fu

/* ------------------------------------------------------------------ */
/* Image state                                                         */
/* ------------------------------------------------------------------ */

static uint8_t  *image;
static uint64_t  image_bytes;
static uint64_t  total_sectors;
static uint32_t  fat_sectors;
static uint32_t  fat_start;        /* sector of FAT 0 */
static uint32_t  root_dir_start;   /* sector of FAT16 root dir, 0 for FAT32 */
static uint32_t  data_start;       /* sector of cluster 2 */
static uint32_t  cluster_count;
static uint32_t  next_free_cluster;
static int       fat_type;         /* 16 or 32 */
static uint32_t  fat_entry_size;   /* 2 or 4 */

static void die(const char *msg) {
  fprintf(stderr, "mkfat: %s\n", msg);
  exit(1);
}

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint8_t *sector_ptr(uint64_t sec) {
  if (sec >= total_sectors) {
    die("internal: sector out of range");
  }
  return image + sec * SECTOR_SIZE;
}

static uint8_t *cluster_ptr(uint32_t clus) {
  if (clus < 2u || clus >= cluster_count + 2u) {
    die("internal: cluster out of range");
  }
  return image + (uint64_t)data_start * SECTOR_SIZE +
         (uint64_t)(clus - 2u) * CLUSTER_SIZE;
}

/* ------------------------------------------------------------------ */
/* FAT                                                                 */
/* ------------------------------------------------------------------ */

static uint32_t fat_get(uint32_t clus) {
  uint64_t byte_off = (uint64_t)clus * fat_entry_size;
  uint8_t *p = sector_ptr(fat_start + byte_off / SECTOR_SIZE) +
               (byte_off % SECTOR_SIZE);

  if (fat_type == 16) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
  }
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void fat_write_entry(uint32_t clus, uint32_t value) {
  uint64_t byte_off = (uint64_t)clus * fat_entry_size;
  uint32_t sec_off = (uint32_t)(byte_off / SECTOR_SIZE);
  uint32_t in_sec = (uint32_t)(byte_off % SECTOR_SIZE);
  uint32_t f;

  (void)in_sec;
  for (f = 0; f < NUM_FATS; f++) {
    uint8_t *p = sector_ptr(fat_start + (uint64_t)f * fat_sectors + sec_off);
    if (fat_type == 16) {
      put_u16(p + in_sec, (uint16_t)value);
    } else {
      put_u32(p + in_sec, value);
    }
  }
}

static void fat_set(uint32_t clus, uint32_t value) {
  if (fat_type == 16) {
    value &= 0xffffu;
    if (value >= 0xfff8u) {
      value = 0xffffu;
    }
  } else {
    value &= 0x0fffffffu;
  }
  fat_write_entry(clus, value);
}

/* Write an entry verbatim. Only cluster 0 needs this: it carries the media
 * descriptor in its low byte with the remaining bits set high (0xFFF8 on
 * FAT16), which is not an end-of-chain marker - and forcing it to 0xFFFF
 * makes strict implementations (mtools, and EDK2's own FAT driver) reject
 * the volume as having a bad media type before they ever read a file. */
static void fat_set_raw(uint32_t clus, uint32_t value) {
  fat_write_entry(clus, value);
}

static uint32_t cluster_alloc(void) {
  uint32_t c = next_free_cluster;

  if (c >= cluster_count + 2u) {
    die("out of clusters: image too small");
  }
  next_free_cluster++;
  fat_set(c, 0x0fffffffu); /* end of chain */
  return c;
}

static uint32_t chain_append(uint32_t tail, uint32_t add) {
  if (tail != 0u) {
    fat_set(tail, add);
  }
  fat_set(add, 0x0fffffffu);
  return add;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
  uint32_t first_cluster; /* 0 for the root */
  uint32_t tail_cluster;
  uint32_t used_dirents;  /* in the whole chain */
} dir_t;

static uint8_t *root_dir_slot(uint32_t idx) {
  if (idx >= ROOT_DIRENTS) {
    die("internal: FAT16 root directory full");
  }
  return sector_ptr(root_dir_start) + idx * DIRENT_SIZE;
}

static uint8_t *dir_slot(dir_t *d, uint32_t idx) {
  uint32_t clus;
  uint32_t skip = idx / DIRENTS_PER_CLUS;
  uint32_t in_clus = idx % DIRENTS_PER_CLUS;
  uint32_t i;

  if (d->first_cluster == 0u) {
    return root_dir_slot(idx);
  }
  clus = d->first_cluster;
  for (i = 0; i < skip; i++) {
    uint32_t next = fat_get(clus);
    if (next >= (fat_type == 16 ? 0xfff8u : 0x0ffffff8u)) {
      uint32_t add = cluster_alloc();
      chain_append(d->tail_cluster, add);
      d->tail_cluster = add;
      memset(cluster_ptr(add), 0, CLUSTER_SIZE);
      next = add;
    }
    clus = next;
  }
  return cluster_ptr(clus) + in_clus * DIRENT_SIZE;
}

static uint8_t *next_free_dir_slot(dir_t *d) {
  uint8_t *slot = dir_slot(d, d->used_dirents);
  if (slot[0] != 0x00u && slot[0] != 0xe5u) {
    die("internal: directory slot not free");
  }
  d->used_dirents++;
  return slot;
}

static uint32_t dir_find_or_extend(dir_t *d, const char *name83,
                                   uint32_t *new_cluster) {
  uint32_t idx;

  for (idx = 0; idx < d->used_dirents; idx++) {
    uint8_t *slot = dir_slot(d, idx);
    if (slot[0] == 0x00u || slot[0] == 0xe5u) {
      continue;
    }
    if ((slot[11] & ATTR_LONG_NAME) == ATTR_LONG_NAME) {
      continue;
    }
    if (memcmp(slot, name83, 11) == 0) {
      if (new_cluster != NULL) {
        *new_cluster = ((uint32_t)slot[20] << 16) | ((uint32_t)slot[21] << 24) |
                       ((uint32_t)slot[26]) | ((uint32_t)slot[27] << 8);
      }
      return idx;
    }
  }
  return 0xffffffffu;
}

/* Small pool: ESP directory trees are shallow. */
static dir_t dir_pool[16];
static int   dir_pool_used = 0;

static dir_t *alloc_dir(void) {
  if (dir_pool_used >= 16) {
    die("too many directories");
  }
  return &dir_pool[dir_pool_used++];
}

static dir_t *dir_get_or_create(dir_t *parent, const char *name) {
  dir_t      *child;
  char        name83[12];
  uint8_t    *slot;
  uint32_t    clus;

  memset(name83, ' ', 11);
  name83[11] = '\0';
  {
    size_t len = strlen(name);
    size_t i;
    if (len > 8) {
      die("directory base name too long");
    }
    for (i = 0; i < len; i++) {
      name83[i] = (char)toupper((unsigned char)name[i]);
    }
  }
  if (dir_find_or_extend(parent, name83, &clus) != 0xffffffffu) {
    child = alloc_dir();
    child->first_cluster = clus;
    child->tail_cluster = clus;
    child->used_dirents = 2; /* "." and ".." are already there */
    return child;
  }
  clus = cluster_alloc();
  memset(cluster_ptr(clus), 0, CLUSTER_SIZE);
  slot = next_free_dir_slot(parent);
  memcpy(slot, name83, 11);
  slot[11] = ATTR_DIRECTORY | ATTR_ARCHIVE;
  slot[20] = (uint8_t)((clus >> 16) & 0xffu);
  slot[21] = (uint8_t)((clus >> 24) & 0xffu);
  slot[26] = (uint8_t)(clus & 0xffu);
  slot[27] = (uint8_t)((clus >> 8) & 0xffu);
  child = alloc_dir();
  child->first_cluster = clus;
  child->tail_cluster = clus;
  child->used_dirents = 2; /* skip "." and ".." entries */
  /* Create . and .. */
  {
    uint8_t *dot = dir_slot(child, 0);
    memset(dot, ' ', 11);
    dot[0] = '.';
    dot[11] = ATTR_DIRECTORY;
    dot[20] = (uint8_t)((clus >> 16) & 0xffu);
    dot[21] = (uint8_t)((clus >> 24) & 0xffu);
    dot[26] = (uint8_t)(clus & 0xffu);
    dot[27] = (uint8_t)((clus >> 8) & 0xffu);
  }
  {
    uint8_t *dotdot = dir_slot(child, 1);
    memset(dotdot, ' ', 11);
    dotdot[0] = '.';
    dotdot[1] = '.';
    dotdot[11] = ATTR_DIRECTORY;
    dotdot[20] = (uint8_t)((parent->first_cluster >> 16) & 0xffu);
    dotdot[21] = (uint8_t)((parent->first_cluster >> 24) & 0xffu);
    dotdot[26] = (uint8_t)(parent->first_cluster & 0xffu);
    dotdot[27] = (uint8_t)((parent->first_cluster >> 8) & 0xffu);
  }
  return child;
}

static int make_short_name(const char *name, uint8_t *out) {
  const char *dot;
  size_t      base_len;
  size_t      ext_len;
  size_t      i;

  memset(out, ' ', 11);
  dot = strchr(name, '.');
  if (dot == NULL) {
    base_len = strlen(name);
    ext_len = 0;
  } else {
    base_len = (size_t)(dot - name);
    ext_len = strlen(dot + 1);
  }
  if (base_len == 0 || base_len > 8 || ext_len > 3) {
    return -1;
  }
  for (i = 0; i < base_len; i++) {
    out[i] = (uint8_t)toupper((unsigned char)name[i]);
  }
  for (i = 0; i < ext_len; i++) {
    out[8 + i] = (uint8_t)toupper((unsigned char)(dot[i + 1]));
  }
  for (i = 0; i < 11; i++) {
    uint8_t c = out[i];
    if (c >= 'a' && c <= 'z') {
      c = (uint8_t)(c - 'a' + 'A');
    }
    out[i] = c;
  }
  return 0;
}

static void dir_add(dir_t *d, const char *name, uint32_t start_cluster,
                    uint64_t size, uint8_t attr) {
  uint8_t name83[11];
  uint8_t *slot;

  if (make_short_name(name, name83) != 0) {
    die("invalid 8.3 name");
  }
  slot = next_free_dir_slot(d);
  memcpy(slot, name83, 11);
  slot[11] = attr | ATTR_ARCHIVE;
  slot[20] = (uint8_t)((start_cluster >> 16) & 0xffu);
  slot[21] = (uint8_t)((start_cluster >> 24) & 0xffu);
  slot[26] = (uint8_t)(start_cluster & 0xffu);
  slot[27] = (uint8_t)((start_cluster >> 8) & 0xffu);
  put_u32(slot + 28, (uint32_t)size);
}

/* ------------------------------------------------------------------ */
/* Geometry and BPB                                                    */
/* ------------------------------------------------------------------ */

static void solve_geometry(void) {
  uint64_t fat_bytes;
  uint64_t cc;

  /* FAT16 is chosen for smaller images so that the install template fits
   * inside the 32 MiB limit of the El Torito hard-disk emulation field. */
  cc = (total_sectors - RESERVED_SECTORS - ROOT_DIR_SECTORS) /
       SECTORS_PER_CLUS;
  fat_type = (cc < 65525u) ? 16 : 32;
  fat_entry_size = (uint32_t)(fat_type == 16 ? 2 : 4);

  /* Iteratively solve FAT sector count: it depends on cluster count, and
   * cluster count depends on FAT size. Stop when two FATs plus the
   * fixed metadata fit inside total_sectors. */
  fat_sectors = 1u;
  for (;;) {
    uint64_t overhead = RESERVED_SECTORS + (uint64_t)NUM_FATS * fat_sectors;
    if (fat_type == 16) {
      overhead += ROOT_DIR_SECTORS;
    }
    if (overhead >= total_sectors) {
      die("image too small for metadata");
    }
    /* Clusters, not sectors: the FAT indexes clusters, and taking the
     * sectors here would both over-size the FAT and let later allocations
     * walk off the end of the image. */
    cc = (total_sectors - overhead) / SECTORS_PER_CLUS;
    fat_bytes = (cc + 2u) * fat_entry_size;
    if ((fat_bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE <= fat_sectors) {
      break;
    }
    fat_sectors++;
  }

  fat_start = RESERVED_SECTORS;
  if (fat_type == 16) {
    root_dir_start = RESERVED_SECTORS + NUM_FATS * fat_sectors;
    data_start = root_dir_start + ROOT_DIR_SECTORS;
  } else {
    root_dir_start = 0;
    data_start = RESERVED_SECTORS + NUM_FATS * fat_sectors;
  }
  cluster_count = (uint32_t)cc;
  if (cluster_count == 0) {
    die("image too small for data area");
  }
}

static void write_bpb(void) {
  uint8_t *bpb = sector_ptr(0);
  uint32_t vol_id = 0x4e6f7675u; /* "Novu" */

  memset(bpb, 0, SECTOR_SIZE);
  bpb[0] = 0xeb;
  bpb[1] = 0x00;
  bpb[2] = 0x90;
  memcpy(bpb + 3, "NOVUS   ", 8); /* OEM name */
  put_u16(bpb + 11, SECTOR_SIZE);
  bpb[13] = (uint8_t)SECTORS_PER_CLUS;
  put_u16(bpb + 14, RESERVED_SECTORS);
  bpb[16] = (uint8_t)NUM_FATS;
  if (fat_type == 16) {
    put_u16(bpb + 17, ROOT_DIRENTS);
    put_u16(bpb + 19, (uint16_t)(total_sectors < 0x10000u ? total_sectors : 0));
    bpb[21] = 0xf8u; /* media descriptor */
    put_u16(bpb + 22, (uint16_t)fat_sectors);
    put_u16(bpb + 24, SECTORS_PER_TRACK); /* geometry filler */
    put_u16(bpb + 26, HEADS);
    put_u32(bpb + 28, 0);      /* hidden sectors */
    put_u32(bpb + 32, 0);      /* total sectors 32: must be 0 when ts16 != 0 */
    bpb[36] = 0x80u; /* drive number */
    bpb[37] = 0x00u; /* reserved */
    bpb[38] = 0x29u; /* boot signature */
    put_u32(bpb + 39, vol_id);
    memcpy(bpb + 43, "NOVUS ESP   ", 11);
    memcpy(bpb + 54, "FAT16   ", 8);
  } else {
    put_u16(bpb + 17, 0);
    put_u16(bpb + 19, 0);
    bpb[21] = 0xf8u;
    put_u16(bpb + 22, 0);
    put_u16(bpb + 24, SECTORS_PER_TRACK);
    put_u16(bpb + 26, HEADS);
    put_u32(bpb + 28, 0);
    put_u32(bpb + 32, (uint32_t)total_sectors);
    put_u32(bpb + 36, fat_sectors);
    put_u16(bpb + 40, 0);    /* drive description */
    put_u16(bpb + 42, 0);    /* version */
    put_u32(bpb + 44, 2);    /* root cluster */
    put_u16(bpb + 48, 1);    /* FSInfo sector */
    put_u16(bpb + 50, 6);    /* backup boot sector */
    bpb[64] = 0x80u;
    bpb[65] = 0x00u;
    bpb[66] = 0x29u;
    put_u32(bpb + 67, vol_id);
    memcpy(bpb + 71, "NOVUS ESP   ", 11);
    memcpy(bpb + 82, "FAT32   ", 8);
  }
  bpb[510] = 0x55u;
  bpb[511] = 0xaau;

  /* Initialise special FAT entries.  The spec fixes the first entry at
   * 0xFFF8 for FAT16 (it carries the media descriptor in its low byte) and
   * 0x0FFFFFF8 for FAT32; the second entry is an end-of-chain marker.  We
   * deliberately leave the rest of sector 0 alone: this is a superfloppy,
   * and a bogus partition table starting at LBA 0 makes strict drivers
   * reject the volume instead of helping them. */
  if (fat_type == 16) {
    fat_set_raw(0, 0xfff8u); /* media descriptor, not an end marker */
    fat_set(1, 0xffffu);     /* EOF marker */
  } else {
    fat_set_raw(0, 0x0ffffff8u);
    fat_set(1, 0x0fffffffu);
  }
}

/* ------------------------------------------------------------------ */
/* File copying                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
  FILE    *fp;
  char    *src;
  char    *dest;
  char     iso_name[12];
  uint64_t len;
  dir_t   *out_dir;
} file_arg_t;

static file_arg_t files[32];
static size_t     file_count = 0;

static dir_t root;

/* Split a destination path into the directory chain and the leaf name.
 *
 * `A/B/C.D' must create A and B and return C.D. The look-ahead token has to
 * be carried across iterations: calling strtok() to peek at the next part
 * already advances the scanner, so re-reading it at the top of the loop
 * would silently swallow every intermediate directory and flatten the tree
 * into A/C.D - which puts the bootloader where the firmware never looks. */
static void resolve_dir(const char *path, dir_t **out_dir, char *name_out,
                        size_t name_out_size) {
  char   buf[256];
  char  *save;
  char  *part;
  char  *next;
  dir_t *cur = &root;

  if (strlen(path) >= sizeof(buf)) {
    die("destination path too long");
  }
  strcpy(buf, path);

  part = strtok_r(buf, "/", &save);
  if (part == NULL) {
    die("empty path");
  }
  next = strtok_r(NULL, "/", &save);
  while (next != NULL) {
    cur = dir_get_or_create(cur, part);
    part = next;
    next = strtok_r(NULL, "/", &save);
  }

  if (strlen(part) >= name_out_size) {
    die("destination file name too long");
  }
  strcpy(name_out, part);
  *out_dir = cur;
}

static void add_file(file_arg_t *f) {
  uint32_t start;
  uint32_t prev;
  uint32_t clus;
  uint64_t wrote;
  size_t   i;
  uint8_t *buf;

  start = cluster_alloc();
  dir_add(f->out_dir, f->iso_name, start, f->len, ATTR_ARCHIVE);

  /* One whole cluster at a time: the chain advances one cluster per write,
   * so the unit has to be the cluster, not the sector, or a cluster larger
   * than a sector would waste most of itself and run out long before the
   * file is written. */
  buf = (uint8_t *)malloc(CLUSTER_SIZE);
  if (buf == NULL) {
    die("cannot allocate write buffer");
  }

  prev = start;
  wrote = 0;
  while (wrote < f->len) {
    size_t chunk = CLUSTER_SIZE;

    if ((uint64_t)chunk > f->len - wrote) {
      chunk = (size_t)(f->len - wrote);
    }
    if (fread(buf, 1, chunk, f->fp) != chunk) {
      die("short read while copying file");
    }
    if (chunk < CLUSTER_SIZE) {
      memset(buf + chunk, 0, CLUSTER_SIZE - chunk);
    }
    memcpy(cluster_ptr(prev), buf, CLUSTER_SIZE);
    wrote += chunk;
    if (wrote < f->len) {
      clus = cluster_alloc();
      fat_set(prev, clus);
      prev = clus;
    }
  }
  free(buf);
  fat_set(prev, 0x0fffffffu);
  for (i = 0; f->iso_name[i] != '\0'; i++) {
    f->iso_name[i] = (char)toupper((unsigned char)f->iso_name[i]);
  }
  printf("mkfat: %s -> %-17s %10lu bytes (cluster %u, FAT%d)\n",
         f->src, f->iso_name, (unsigned long)f->len, start, fat_type);
  (void)fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  int          size_mb;
  FILE        *out;
  size_t       i;

  if (argc < 4) {
    fprintf(stderr, "usage: mkfat <out.img> <size-mb> <src>:<dest> ...\n");
    return 2;
  }

  size_mb = atoi(argv[2]);
  if (size_mb <= 0 || size_mb > 4096) {
    die("size-mb must be between 1 and 4096");
  }
  image_bytes = (uint64_t)size_mb * 1024u * 1024u;
  total_sectors = image_bytes / SECTOR_SIZE;

  image = (uint8_t *)calloc(1, (size_t)image_bytes);
  if (image == NULL) {
    die("cannot allocate image buffer");
  }

  solve_geometry();
  next_free_cluster = 2u;
  write_bpb();

  /* Root directory is fixed on FAT16 and starts at cluster 2 on FAT32. */
  root.first_cluster = 0;
  root.tail_cluster = 0;
  root.used_dirents = 0;
  if (fat_type == 16) {
    memset(sector_ptr(root_dir_start), 0,
           (size_t)ROOT_DIR_SECTORS * SECTOR_SIZE);
  } else {
    memset(cluster_ptr(2), 0, CLUSTER_SIZE);
  }

  for (i = 0; i < (size_t)(argc - 3); i++) {
    char *colon;
    char  dest[256];
    char  dest_name[256];

    if (file_count >= 32) {
      die("too many files");
    }
    colon = strchr(argv[3 + i], ':');
    if (colon == NULL) {
      die("file argument must be <src>:<dest>");
    }
    *colon = '\0';
    files[file_count].src = argv[3 + i];
    files[file_count].dest = colon + 1;
    files[file_count].fp = fopen(files[file_count].src, "rb");
    if (files[file_count].fp == NULL) {
      die("cannot open source file");
    }
    if (fseek(files[file_count].fp, 0, SEEK_END) != 0) {
      die("cannot seek source file");
    }
    files[file_count].len = (uint64_t)ftell(files[file_count].fp);
    if (fseek(files[file_count].fp, 0, SEEK_SET) != 0) {
      die("cannot rewind source file");
    }

    strncpy(dest, files[file_count].dest, sizeof(dest) - 1);
    dest[sizeof(dest) - 1] = '\0';

    resolve_dir(dest, &files[file_count].out_dir, dest_name,
                sizeof(dest_name));
    if (strlen(dest_name) >= sizeof(files[file_count].iso_name)) {
      die("destination file name too long");
    }
    /* Length already checked above, against the same 11-byte + NUL field. */
    strcpy(files[file_count].iso_name, dest_name);

    file_count++;
  }

  for (i = 0; i < file_count; i++) {
    add_file(&files[i]);
  }

  out = fopen(argv[1], "wb");
  if (out == NULL) {
    die("cannot open output file");
  }
  if (fwrite(image, 1, (size_t)image_bytes, out) != (size_t)image_bytes) {
    die("cannot write output file");
  }
  if (fclose(out) != 0) {
    die("cannot close output file");
  }

  printf("mkfat: wrote %s (%lu MiB, %u clusters, FAT%d)\n",
         argv[1], (unsigned long)(image_bytes / (1024u * 1024u)),
         cluster_count, fat_type);
  return 0;
}
