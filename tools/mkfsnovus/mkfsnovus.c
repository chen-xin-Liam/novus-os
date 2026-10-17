/*
 * mkfsnovus - host-side NovusFS image builder.
 *
 * The point of this tool is that it contains no filesystem logic of its
 * own: it compiles the kernel's fs/novusfs sources unchanged and only
 * supplies the handful of primitives that code expects (kputs, kmalloc,
 * blkdev). A host-side reimplementation of the format would be a second
 * source of truth that could drift from the kernel's - and a disk image
 * written by a drifting tool is a disk image the kernel cannot mount.
 * Sharing the source makes that failure impossible by construction.
 *
 * The kernel sources are freestanding, so they are compiled with memcpy,
 * memset and memcmp renamed to novus_* (see tools.mk) and this file
 * provides those three under their renamed forms. That keeps the
 * kernel's declarations from colliding with glibc's.
 *
 * Usage:
 *   mkfsnovus <out.img> <size-mb>
 *
 * The image is formatted and its Merkle tree is built immediately, so
 * the superblock carries the root hash the kernel will verify against.
 */

/* fseeko/ftruncate on a multi-megabyte image need the 64-bit offsets. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> /* ftruncate */

#include "novusfs.h"

/* ------------------------------------------------------------------ */
/* Host stand-ins for the freestanding primitives                      */
/* ------------------------------------------------------------------ */

void kputs(const char *s) {
  if (s != NULL) {
    fputs(s, stdout);
  }
}

void kputs_hex(UINT64 v) {
  printf("0x%llx", (unsigned long long)v);
}

void *kmalloc(UINT64 size) {
  void *p = calloc(1, (size_t)(size == 0 ? 1u : size));
  return p;
}

void kfree(void *ptr) {
  free(ptr);
}

/* Renamed by -D; see the file header. Hand-written so the definitions
 * do not call the libc functions whose names were taken away. */
void *memcpy(void *dst, const void *src, UINT64 n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  UINT64 i;

  for (i = 0; i < n; i++) {
    d[i] = s[i];
  }
  return dst;
}

void *memset(void *dst, int c, UINT64 n) {
  unsigned char *d = (unsigned char *)dst;
  UINT64 i;

  for (i = 0; i < n; i++) {
    d[i] = (unsigned char)c;
  }
  return dst;
}

int memcmp(const void *a, const void *b, UINT64 n) {
  const unsigned char *x = (const unsigned char *)a;
  const unsigned char *y = (const unsigned char *)b;
  UINT64 i;

  for (i = 0; i < n; i++) {
    if (x[i] != y[i]) {
      return (int)x[i] - (int)y[i];
    }
  }
  return 0;
}

/* The ramdisk backend and the direct-map helpers are the only pieces of
 * the kernel that fs/novusfs reaches for but this tool never uses. They
 * are stubbed rather than compiled so the link stays small. */
void *phys_to_virt(UINT64 phys) {
  (void)phys;
  return NULL;
}

UINT64 virt_to_phys(const void *v) {
  (void)v;
  return 0;
}

blkdev_t *ramdisk_init(UINT64 bytes) {
  (void)bytes;
  return NULL;
}

/* Rust panic landing pad: never called from the FS code, but the Rust
 * object references it. */
void novus_rs_panic_hook(const UINT8 *msg, UINT64 len) {
  UINT64 i;

  fputs("novuslib-rs panic: ", stderr);
  for (i = 0; i < len && msg != NULL; i++) {
    fputc((int)msg[i], stderr);
  }
  fputc('\n', stderr);
  exit(1);
}

/* ------------------------------------------------------------------ */
/* File-backed block device                                            */
/* ------------------------------------------------------------------ */

typedef struct {
  FILE *f;
} file_ctx_t;

static int file_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  file_ctx_t *c = (file_ctx_t *)ctx;

  if (fseeko(c->f, (off_t)(sector * (UINT64)BLKDEV_SECTOR_SIZE), SEEK_SET) !=
      0) {
    return -1;
  }
  if (fread(buf, BLKDEV_SECTOR_SIZE, (size_t)count, c->f) != (size_t)count) {
    return -1;
  }
  return 0;
}

static int file_write(void *ctx, UINT64 sector, UINTN count,
                      const void *buf) {
  file_ctx_t *c = (file_ctx_t *)ctx;

  if (fseeko(c->f, (off_t)(sector * (UINT64)BLKDEV_SECTOR_SIZE), SEEK_SET) !=
      0) {
    return -1;
  }
  if (fwrite(buf, BLKDEV_SECTOR_SIZE, (size_t)count, c->f) !=
      (size_t)count) {
    return -1;
  }
  return 0;
}

/* blkdev.c itself is not compiled here: it pulls in the UEFI and ramdisk
 * backends, which need firmware protocols and the kernel PMM. Only the
 * two dispatch functions are reimplemented, which is all fs/novusfs
 * actually calls - and they add no behaviour of their own, so there is
 * no filesystem logic to drift. */
int blkdev_read(blkdev_t *dev, UINT64 sector, UINTN count, void *buf) {
  if (dev == NULL || dev->read == NULL) {
    return -1;
  }
  return dev->read(dev->ctx, sector, count, buf);
}

int blkdev_write(blkdev_t *dev, UINT64 sector, UINTN count,
                 const void *buf) {
  if (dev == NULL || dev->write == NULL) {
    return -1;
  }
  return dev->write(dev->ctx, sector, count, buf);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static void print_root(const UINT8 *root) {
  static const char digits[] = "0123456789abcdef";
  UINTN i;

  for (i = 0; i < NOVUSFS_MERKLE_ROOT_LEN; i++) {
    putchar(digits[(root[i] >> 4) & 0x0fu]);
    putchar(digits[root[i] & 0x0fu]);
  }
}

int main(int argc, char **argv) {
  static novusfs_fs_t fs;
  static file_ctx_t   ctx;
  static blkdev_t     dev;
  UINT64 bytes;
  UINT64 size_mb;
  UINT8  root[NOVUSFS_MERKLE_ROOT_LEN];
  FILE  *out;

  if (argc < 3) {
    fprintf(stderr, "usage: mkfsnovus <out.img> <size-mb>\n");
    return 2;
  }
  size_mb = strtoull(argv[2], NULL, 10);
  if (size_mb == 0 || size_mb > 4096u) {
    fprintf(stderr, "mkfsnovus: size-mb must be between 1 and 4096\n");
    return 1;
  }
  bytes = size_mb * 1024u * 1024u;

  out = fopen(argv[1], "wb+");
  if (out == NULL) {
    fprintf(stderr, "mkfsnovus: cannot create %s\n", argv[1]);
    return 1;
  }
  /* Sparse is fine for a raw image: the filesystem writes every block it
   * cares about, and a hole reads back as zeroes. */
  if (ftruncate(fileno(out), (off_t)bytes) != 0) {
    fprintf(stderr, "mkfsnovus: cannot size %s\n", argv[1]);
    fclose(out);
    return 1;
  }

  ctx.f = out;
  dev.name = "file";
  dev.sector_count = bytes / (UINT64)BLKDEV_SECTOR_SIZE;
  dev.flags = 0;
  dev.ctx = &ctx;
  dev.read = file_read;
  dev.write = file_write;

  if (novusfs_format(&fs, &dev) != 0) {
    fprintf(stderr, "mkfsnovus: format failed\n");
    fclose(out);
    return 1;
  }
  novusfs_dump_layout(&fs);

  if (novusfs_merkle_build(&fs, root) != 0) {
    fprintf(stderr, "mkfsnovus: merkle build failed\n");
    fclose(out);
    return 1;
  }

  fputs("mkfsnovus: ", stdout);
  print_root(root);
  printf("\nmkfsnovus: %s  %llu MiB, merkle root written, merkle_valid=%u\n",
         argv[1], (unsigned long long)size_mb, fs.sb.merkle_valid);

  fflush(out);
  fclose(out);
  return 0;
}
