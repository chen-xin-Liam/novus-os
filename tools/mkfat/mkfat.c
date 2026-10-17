/*
 * mkfat - build a FAT32 EFI system partition image.
 *
 * Novus ships its boot files (BOOTX64.EFI, the NovusFS system image and
 * the *.nkm modules) on a FAT32 ESP because that is the one filesystem
 * every UEFI firmware can read without a driver of ours. mkfat writes
 * the bytes of that partition directly, so the tree needs no FAT
 * implementation of its own anywhere else: at boot the firmware's own
 * FAT driver does the reading.
 *
 * Scope, deliberately: 512-byte sectors, one sector per cluster, two
 * FATs, no long file names. Every file the ESP carries is addressed by
 * its 8.3 name, which is what the firmware looks for anyway
 * (EFI/BOOT/BOOTX64.EFI), and short names are what every FAT driver has
 * understood since 1981. Names that are not valid 8.3 are rejected here
 * rather than silently mangled.
 *
 * Usage:
 *   mkfat <out.img> <size-mb> <src>:<dest> [<src>:<dest> ...]
 *
 * `dest` is a path inside the image using '/' separators, e.g.
 * EFI/BOOT/BOOTX64.EFI. Intermediate directories are created.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SECTOR_SIZE      512u
#define SECTORS_PER_CLUS 1u
#define CLUSTER_SIZE     (SECTOR_SIZE * SECTORS_PER_CLUS)
#define RESERVED_SECTORS 32u
#define NUM_FATS         2u
#define ROOT_CLUSTER     2u
#define DIRENT_SIZE      32u
#define DIRENTS_PER_CLUS (CLUSTER_SIZE / DIRENT_SIZE)

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

static uint8_t *image;
static uint64_t image_bytes;
static uint64_t total_sectors;
static uint32_t fat_sectors;
static uint32_t fat_start;  /* sector of FAT 0 */
static uint32_t data_start; /* sector of cluster 2 */
static uint32_t cluster_count;
static uint32_t next_free_cluster;

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

/* Entries are 4 bytes and the sector size is a multiple of 4, so a
 * read never straddles a sector boundary. */
static uint32_t fat_get(uint32_t clus) {
  uint64_t byte_off = (uint64_t)clus * 4u;
  uint8_t *p = sector_ptr(fat_start + byte_off / SECTOR_SIZE) +
               (byte_off % SECTOR_SIZE);

  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void fat_set(uint32_t clus, uint32_t value) {
  uint32_t byte_off = clus * 4u;
  uint32_t sec_off = byte_off / SECTOR_SIZE;
  uint32_t in_sec = byte_off % SECTOR_SIZE;
  uint32_t f;

  /* Both FATs carry the same content; the second is the mirror. */
  for (f = 0; f < NUM_FATS; f++) {
    uint8_t *p = sector_ptr(fat_start + (uint64_t)f * fat_sectors + sec_off);
    put_u32(p + in_sec, value & 0x0fffffffu);
  }
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

/* Extend a chain by one cluster, linking it from the old tail. */
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
  uint32_t first_cluster; /* 0 for the root, which is fixed at 2 */
  uint32_t tail_cluster;
  uint32_t used_dirents; /* in the whole chain */
} dir_t;

/* Locate dirent `idx` of a directory, allocating a cluster when the
 * chain has to grow. Returns a pointer to the 32-byte slot. */
static uint8_t *dir_slot(dir_t *d, uint32_t idx) {
  uint32_t clus;
  uint32_t skip = idx / DIRENTS_PER_CLUS;
  uint32_t in_clus = idx % DIRENTS_PER_CLUS;
  uint32_t i;

  clus = (d->first_cluster == 0u) ? ROOT_CLUSTER : d->first_cluster;
  for (i = 0; i < skip; i++) {
    uint32_t next = fat_get(clus);
    if (next >= 0x0ffffff8u) {
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

/* Validate and convert "NAME.EXT" into the 11-byte on-disk form. */
static int make_short_name(const char *name, uint8_t out[11]) {
  const char *dot;
  size_t base_len;
  size_t ext_len;
  size_t i;

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
    char c = name[i];
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-' || c == '~')) {
      return -1;
    }
    out[i] = (uint8_t)c;
  }
  for (i = 0; i < ext_len; i++) {
    char c = dot[1 + i];
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
      return -1;
    }
    out[8 + i] = (uint8_t)c;
  }
  return 0;
}

static void write_dirent(uint8_t *slot, const char *name, uint8_t attr,
                         uint32_t first_clus, uint32_t size) {
  uint8_t short_name[11];

  if (make_short_name(name, short_name) != 0) {
    fprintf(stderr, "mkfat: name is not a valid 8.3 name: %s\n", name);
    exit(1);
  }
  memset(slot, 0, DIRENT_SIZE);
  memcpy(slot, short_name, 11);
  slot[11] = attr;
  put_u16(slot + 20, (uint16_t)((first_clus >> 16) & 0xffffu));
  put_u16(slot + 26, (uint16_t)(first_clus & 0xffffu));
  put_u32(slot + 28, size);
}

/* Find or create the directory named `name` inside `parent`. */
static dir_t dir_get_or_create(dir_t *parent, const char *name) {
  dir_t out;
  uint32_t idx;
  uint32_t first = 0;
  uint32_t tail = 0;

  for (idx = 0;; idx++) {
    uint8_t *slot = dir_slot(parent, idx);
    uint8_t attr = slot[11];

    if (slot[0] == 0x00u) {
      break; /* end of directory: not found */
    }
    if (slot[0] == 0xe5u || attr == ATTR_LONG_NAME) {
      continue;
    }
    {
      uint8_t want[11];
      if (make_short_name(name, want) != 0) {
        die("bad directory name");
      }
      if (memcmp(slot, want, 11) == 0 && (attr & ATTR_DIRECTORY) != 0) {
        first = (uint32_t)slot[26] | ((uint32_t)slot[20] << 16);
        tail = first;
        break;
      }
    }
  }

  if (first == 0u) {
    first = cluster_alloc();
    tail = first;
    memset(cluster_ptr(first), 0, CLUSTER_SIZE);
    write_dirent(dir_slot(parent, idx), name,
                 ATTR_DIRECTORY, first, 0);
    /* "." and ".." are required by convention and cheap to emit. */
    {
      dir_t self;
      self.first_cluster = first;
      self.tail_cluster = tail;
      self.used_dirents = 0;
      write_dirent(dir_slot(&self, 0), ".", ATTR_DIRECTORY, first, 0);
      write_dirent(dir_slot(&self, 1), "..", ATTR_DIRECTORY,
                   (parent->first_cluster == 0u) ? 0u : parent->first_cluster,
                   0);
    }
  }

  out.first_cluster = first;
  out.tail_cluster = tail;
  out.used_dirents = 0;
  return out;
}

static void add_file(dir_t *parent, const char *src_path, const char *name) {
  FILE *f;
  uint32_t first = 0;
  uint32_t tail = 0;
  uint32_t idx = 0;
  uint64_t total = 0;
  uint8_t buf[CLUSTER_SIZE];

  f = fopen(src_path, "rb");
  if (f == NULL) {
    fprintf(stderr, "mkfat: cannot open %s\n", src_path);
    exit(1);
  }

  for (;;) {
    size_t got = fread(buf, 1, sizeof(buf), f);
    uint32_t c;

    if (got == 0) {
      break;
    }
    memset(buf + got, 0, sizeof(buf) - got);
    c = cluster_alloc();
    if (first == 0u) {
      first = c;
    } else {
      tail = chain_append(tail, c);
    }
    tail = c;
    memcpy(cluster_ptr(c), buf, CLUSTER_SIZE);
    total += got;
    if (got < sizeof(buf)) {
      break;
    }
  }
  fclose(f);

  /* First free slot in the parent directory. */
  for (idx = 0;; idx++) {
    uint8_t *slot = dir_slot(parent, idx);
    if (slot[0] == 0x00u || slot[0] == 0xe5u) {
      write_dirent(slot, name, ATTR_ARCHIVE, first, (uint32_t)total);
      return;
    }
  }
}

/* Resolve "EFI/BOOT/BOOTX64.EFI" into its directory, creating as needed. */
static dir_t resolve_dir(const char *path) {
  dir_t cur;
  char buf[256];
  char *p;

  cur.first_cluster = 0; /* root */
  cur.tail_cluster = ROOT_CLUSTER;
  cur.used_dirents = 0;

  if (strlen(path) >= sizeof(buf)) {
    die("path too long");
  }
  strcpy(buf, path);
  p = buf;
  for (;;) {
    char *slash = strchr(p, '/');
    if (slash == NULL) {
      break;
    }
    *slash = '\0';
    if (p[0] != '\0') {
      cur = dir_get_or_create(&cur, p);
    }
    p = slash + 1;
  }
  (void)p;
  return cur;
}

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return (slash == NULL) ? path : slash + 1;
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/* FAT size and cluster count depend on each other, so solve by fixed
 * point iteration: start with no FAT, then recompute until stable. */
static void solve_geometry(void) {
  uint32_t prev = 0;
  uint32_t guard = 0;

  fat_sectors = 0;
  while (guard++ < 64u) {
    uint64_t data_secs = total_sectors - RESERVED_SECTORS -
                         (uint64_t)NUM_FATS * fat_sectors;
    uint64_t clusters = data_secs / SECTORS_PER_CLUS;
    uint32_t need =
        (uint32_t)(((clusters + 2u) * 4u + SECTOR_SIZE - 1u) / SECTOR_SIZE);

    if (need == fat_sectors || need == prev) {
      cluster_count = (uint32_t)clusters;
      break;
    }
    prev = fat_sectors;
    fat_sectors = need;
    cluster_count = (uint32_t)clusters;
  }
  if (cluster_count < 16u) {
    die("image too small for FAT32");
  }
  fat_start = RESERVED_SECTORS;
  data_start = RESERVED_SECTORS + NUM_FATS * fat_sectors;
  next_free_cluster = ROOT_CLUSTER;
}

static void write_bpb(void) {
  uint8_t *b = sector_ptr(0);

  memset(b, 0, SECTOR_SIZE);
  b[0] = 0xeb;
  b[1] = 0x58;
  b[2] = 0x90;
  memcpy(b + 3, "MSWIN4.1", 8);
  put_u16(b + 11, SECTOR_SIZE);
  b[13] = SECTORS_PER_CLUS;
  put_u16(b + 14, RESERVED_SECTORS);
  b[16] = NUM_FATS;
  put_u16(b + 17, 0); /* root entry count: 0 on FAT32 */
  put_u16(b + 19, 0); /* total sectors 16: 0, use the 32-bit field */
  b[21] = 0xf8;       /* media descriptor: fixed disk */
  put_u16(b + 22, 0); /* FAT size 16: 0, use the 32-bit field */
  put_u16(b + 24, 32);
  put_u16(b + 26, 8);
  put_u32(b + 28, 0); /* hidden sectors */
  put_u32(b + 32, (uint32_t)total_sectors);
  put_u32(b + 36, fat_sectors);
  put_u16(b + 40, 0); /* ext flags: all FATs active */
  put_u16(b + 42, 0); /* fs version 0 */
  put_u32(b + 44, ROOT_CLUSTER);
  put_u16(b + 48, 1); /* FSInfo sector */
  put_u16(b + 50, 6); /* backup boot sector */
  b[64] = 0x80;       /* drive number */
  b[66] = 0x29;       /* extended boot signature */
  put_u32(b + 67, 0x4e4f5655u); /* volume id "NOVU" */
  memcpy(b + 71, "NOVUS ESP  ", 11);
  memcpy(b + 82, "FAT32   ", 8);
  b[510] = 0x55;
  b[511] = 0xaa;

  /* FSInfo: only the signatures are mandatory, but the free count is
   * cheap and some drivers read it. */
  {
    uint8_t *f = sector_ptr(1);
    memset(f, 0, SECTOR_SIZE);
    memcpy(f, "RRaA", 4);
    memcpy(f + 484, "rrAa", 4);
    put_u32(f + 488, cluster_count);
    put_u32(f + 492, next_free_cluster);
    f[508] = 0x00;
    f[509] = 0x00;
    f[510] = 0x55;
    f[511] = 0xaa;
  }
}

int main(int argc, char **argv) {
  uint64_t size_mb;
  int i;

  if (argc < 4) {
    fprintf(stderr,
            "usage: mkfat <out.img> <size-mb> <src>:<dest> [<src>:<dest>...]\n");
    return 2;
  }

  size_mb = strtoull(argv[2], NULL, 10);
  if (size_mb == 0 || size_mb > 4096u) {
    die("size-mb must be between 1 and 4096");
  }
  image_bytes = size_mb * 1024u * 1024u;
  total_sectors = image_bytes / SECTOR_SIZE;

  image = calloc(1, (size_t)image_bytes);
  if (image == NULL) {
    die("out of memory");
  }

  solve_geometry();
  write_bpb();

  /* Cluster 2 is the root directory and must start zeroed. */
  (void)cluster_alloc();
  memset(cluster_ptr(ROOT_CLUSTER), 0, CLUSTER_SIZE);

  for (i = 3; i < argc; i++) {
    char spec[512];
    char *colon;
    dir_t dir;

    if (strlen(argv[i]) >= sizeof(spec)) {
      die("argument too long");
    }
    strcpy(spec, argv[i]);
    colon = strchr(spec, ':');
    if (colon == NULL) {
      die("argument must be <src>:<dest>");
    }
    *colon = '\0';
    dir = resolve_dir(colon + 1);
    add_file(&dir, spec, base_name(colon + 1));
  }

  {
    FILE *out = fopen(argv[1], "wb");
    if (out == NULL) {
      die("cannot create output image");
    }
    if (fwrite(image, 1, (size_t)image_bytes, out) != image_bytes) {
      die("short write");
    }
    fclose(out);
  }

  printf("mkfat: %s  %llu MiB, %u clusters, %u FAT sectors\n", argv[1],
         (unsigned long long)size_mb, cluster_count, fat_sectors);
  free(image);
  return 0;
}
