/*
 * mkdisk - assemble a bootable Novus disk image on the host.
 *
 * The boot-stage installer writes GPT, an EFI system partition and the
 * NovusFS system partition from inside the live system. This tool writes
 * the same layout from the host, for the case where the target is not
 * attached to a running Novus: imaging an SD card, seeding a VM disk, or
 * producing something to hand to `make install`.
 *
 * It contains no partition-table logic of its own. kernel/gpt.c is
 * compiled in unchanged - the same arrangement mkfsnovus uses for
 * fs/novusfs - so the two installers cannot drift apart and produce
 * disks that disagree about where a partition starts. The system
 * partition image is copied verbatim, Merkle tree and all, so its root
 * hash is already the one the A core will verify against.
 *
 * Usage:
 *   mkdisk <out.img> <size-mb> <esp.img> <novus.img>
 */

/* fseeko/ftruncate on a multi-megabyte image need the 64-bit offsets. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> /* ftruncate */

#include "gpt.h"
#include "kernel.h"
#include "../lib/novuslib_rs.h"

/* ------------------------------------------------------------------ */
/* Host stand-ins for the freestanding primitives                      */
/* ------------------------------------------------------------------ */
/* Renamed by -D in tools.mk; see the file header. Written by hand so
 * the definitions do not call the libc functions whose names were taken
 * away. */

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

void kputs(const char *s) {
  if (s != NULL) {
    fputs(s, stdout);
  }
}

void kputs_hex(UINT64 v) {
  printf("0x%llx", (unsigned long long)v);
}

void *kmalloc(UINT64 size) {
  return calloc(1, (size_t)(size == 0 ? 1u : size));
}

void kfree(void *ptr) {
  free(ptr);
}

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

/* blkdev.c itself is not compiled here: it drags in the UEFI and ramdisk
 * backends, which need firmware protocols and the kernel PMM. Only the
 * two dispatchers are reimplemented, and they add no behaviour of their
 * own. */
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

#define COPY_CHUNK_SECTORS 256u /* 128 KiB per write */

/* Stream `src` into the image at `lba`. Returns the number of sectors
 * written, or ~0 on failure. The last sector is zero-padded: neither
 * payload is a whole number of sectors in general, and an ESP or a
 * filesystem that ends mid-sector is not something any driver expects. */
static UINT64 stream_in(FILE *src, blkdev_t *dev, UINT64 lba) {
  static UINT8 buf[COPY_CHUNK_SECTORS * BLKDEV_SECTOR_SIZE];
  UINT64       sectors = 0;

  for (;;) {
    size_t got = fread(buf, 1, sizeof(buf), src);

    if (got == 0) {
      break;
    }
    if ((got % BLKDEV_SECTOR_SIZE) != 0) {
      size_t pad = BLKDEV_SECTOR_SIZE - (got % BLKDEV_SECTOR_SIZE);

      memset(buf + got, 0, pad);
      got += pad;
    }
    if (blkdev_write(dev, lba + sectors, (UINTN)(got / BLKDEV_SECTOR_SIZE),
                     buf) != 0) {
      return (UINT64)~0ull;
    }
    sectors += (UINT64)(got / BLKDEV_SECTOR_SIZE);
    if (got < sizeof(buf)) {
      break;
    }
  }
  return sectors;
}

static UINT64 file_sectors(const char *path) {
  FILE  *f = fopen(path, "rb");
  off_t  end;
  UINT64 secs;

  if (f == NULL) {
    fprintf(stderr, "mkdisk: cannot open %s\n", path);
    exit(1);
  }
  if (fseeko(f, 0, SEEK_END) != 0 || (end = ftello(f)) < 0) {
    fprintf(stderr, "mkdisk: cannot size %s\n", path);
    exit(1);
  }
  fclose(f);
  secs = ((UINT64)end + BLKDEV_SECTOR_SIZE - 1u) / BLKDEV_SECTOR_SIZE;
  if (secs == 0) {
    fprintf(stderr, "mkdisk: %s is empty\n", path);
    exit(1);
  }
  return secs;
}

int main(int argc, char **argv) {
  static file_ctx_t      ctx;
  static blkdev_t        dev;
  gpt_part_spec_t        specs[2];
  UINT64                 size_mb;
  UINT64                 bytes;
  UINT64                 esp_lba;
  UINT64                 esp_sectors;
  UINT64                 sys_lba;
  UINT64                 sys_sectors;
  UINT64                 copied;
  FILE                  *out;
  FILE                  *in;
  UINTN                  i;

  if (argc < 5) {
    fprintf(stderr, "usage: mkdisk <out.img> <size-mb> <esp.img> "
                    "<novus.img>\n");
    return 2;
  }
  size_mb = strtoull(argv[2], NULL, 10);
  if (size_mb < 16u || size_mb > 4096u) {
    fprintf(stderr, "mkdisk: size-mb must be between 16 and 4096\n");
    return 1;
  }
  bytes = size_mb * 1024u * 1024u;

  esp_sectors = file_sectors(argv[3]);
  sys_sectors = file_sectors(argv[4]);

  /* Both payloads start on a 1 MiB boundary, the same convention the
   * boot-stage installer uses, and the system partition is rounded up to
   * a whole MiB so the filesystem's block count stays clean. */
  esp_lba = 2048u;
  sys_lba = esp_lba + esp_sectors;
  sys_sectors = (sys_sectors + 2047u) & ~(UINT64)2047u;

  /* 34 sectors of GPT at each end, plus a MiB of alignment to spare. */
  if (sys_lba + sys_sectors + 2048u > bytes / BLKDEV_SECTOR_SIZE) {
    fprintf(stderr, "mkdisk: %llu MiB is too small for the ESP and the "
                    "system partition\n",
            (unsigned long long)size_mb);
    return 1;
  }

  out = fopen(argv[1], "wb+");
  if (out == NULL) {
    fprintf(stderr, "mkdisk: cannot create %s\n", argv[1]);
    return 1;
  }
  if (ftruncate(fileno(out), (off_t)bytes) != 0) {
    fprintf(stderr, "mkdisk: cannot size %s\n", argv[1]);
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

  in = fopen(argv[3], "rb");
  if (in == NULL) {
    fprintf(stderr, "mkdisk: cannot open %s\n", argv[3]);
    return 1;
  }
  copied = stream_in(in, &dev, esp_lba);
  fclose(in);
  if (copied == (UINT64)~0ull) {
    fprintf(stderr, "mkdisk: failed writing the ESP\n");
    return 1;
  }

  in = fopen(argv[4], "rb");
  if (in == NULL) {
    fprintf(stderr, "mkdisk: cannot open %s\n", argv[4]);
    return 1;
  }
  copied = stream_in(in, &dev, sys_lba);
  fclose(in);
  if (copied == (UINT64)~0ull) {
    fprintf(stderr, "mkdisk: failed writing the system partition\n");
    return 1;
  }

  memset(specs, 0, sizeof(specs));
  {
    static const UINT8 esp_guid[16]      = GPT_TYPE_ESP_BYTES;
    static const UINT8 novusfs_guid[16]  = GPT_TYPE_NOVUSFS_BYTES;

    memcpy(specs[0].type_guid, esp_guid, 16);
    specs[0].first_lba = esp_lba;
    specs[0].last_lba = esp_lba + esp_sectors - 1u;
    for (i = 0; i < 4; i++) {
      specs[0].name[i] = (CHAR16)("EFI "[i]);
    }
    memcpy(specs[1].type_guid, novusfs_guid, 16);
    specs[1].first_lba = sys_lba;
    specs[1].last_lba = sys_lba + sys_sectors - 1u;
    for (i = 0; i < 6; i++) {
      specs[1].name[i] = (CHAR16)("NOVUS "[i]);
    }
  }

  if (gpt_write(&dev, specs, 2) != 0) {
    fprintf(stderr, "mkdisk: GPT write failed\n");
    return 1;
  }

  printf("mkdisk: %s %llu MiB: ESP at LBA %llu (%llu sectors), "
         "NovusFS at LBA %llu (%llu sectors)\n",
         argv[1], (unsigned long long)size_mb, (unsigned long long)esp_lba,
         (unsigned long long)esp_sectors, (unsigned long long)sys_lba,
         (unsigned long long)sys_sectors);

  if (fflush(out) != 0 || fclose(out) != 0) {
    fprintf(stderr, "mkdisk: cannot flush %s\n", argv[1]);
    return 1;
  }
  return 0;
}
