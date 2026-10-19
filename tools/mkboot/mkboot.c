#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_layout.h"

#define SECTOR_SIZE 512u
#define PATCH_MAGIC "NVB2"
#define PATCH_HEADER_SIZE 28u
/* SeaBIOS (and real BIOSes) refuse disks under ~1 MiB as boot devices;
 * pad the finished image to a full 2 MiB so the ATA geometry is sane. */
#define MIN_IMAGE_SECTORS 4096u

static int read_file(const char *path, uint8_t **data_out, size_t *size_out) {
  FILE *file = fopen(path, "rb");
  long length;
  uint8_t *data;

  if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
      (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    if (file != NULL) {
      fclose(file);
    }
    fprintf(stderr, "mkboot: cannot read %s\n", path);
    return 0;
  }
  data = (uint8_t *)malloc((size_t)length);
  if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    fprintf(stderr, "mkboot: failed to load %s\n", path);
    return 0;
  }
  fclose(file);
  *data_out = data;
  *size_out = (size_t)length;
  return 1;
}

static uint16_t read_le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
  return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static void write_le32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static void write_le64(uint8_t *p, uint64_t value) {
  write_le32(p, (uint32_t)value);
  write_le32(p + 4, (uint32_t)(value >> 32));
}

static size_t find_patch_header(const uint8_t *data, size_t size) {
  size_t i;
  size_t found = SIZE_MAX;
  if (size < PATCH_HEADER_SIZE) {
    return SIZE_MAX;
  }
  for (i = 0; i <= size - PATCH_HEADER_SIZE; i++) {
    if (memcmp(data + i, PATCH_MAGIC, 4) == 0) {
      if (found != SIZE_MAX) {
        return SIZE_MAX;
      }
      found = i;
    }
  }
  return found;
}

static int write_zeros(FILE *file, size_t count) {
  static const uint8_t zeros[SECTOR_SIZE] = {0};
  while (count != 0) {
    size_t chunk = count < sizeof(zeros) ? count : sizeof(zeros);
    if (fwrite(zeros, 1, chunk, file) != chunk) {
      return 0;
    }
    count -= chunk;
  }
  return 1;
}

int main(int argc, char **argv) {
  uint8_t *bootsect = NULL;
  uint8_t *stage2 = NULL;
  uint8_t *kernel = NULL;
  size_t bootsect_size = 0;
  size_t stage2_size = 0;
  size_t kernel_size = 0;
  size_t header;
  uint64_t kernel_sectors;
  uint64_t padded_kernel_size;
  FILE *output;
  int ok = 0;

  if (argc != 5) {
    fprintf(stderr,
            "usage: mkboot BOOTSECT.BIN STAGE2.BIN KERNEL.BIN OUTPUT.IMG\n");
    return 2;
  }
  if (!read_file(argv[1], &bootsect, &bootsect_size) ||
      !read_file(argv[2], &stage2, &stage2_size) ||
      !read_file(argv[3], &kernel, &kernel_size)) {
    goto done;
  }
  if (bootsect_size != SECTOR_SIZE || bootsect[510] != 0x55 ||
      bootsect[511] != 0xAA) {
    fprintf(stderr, "mkboot: boot sector must be 512 bytes and end in 55 AA\n");
    goto done;
  }
  if (stage2_size != (size_t)STAGE2_SECTORS * SECTOR_SIZE) {
    fprintf(stderr, "mkboot: stage2 size does not match STAGE2_SECTORS\n");
    goto done;
  }
  header = find_patch_header(stage2, stage2_size);
  if (header == SIZE_MAX || read_le16(stage2 + header + 4) != 1 ||
      read_le16(stage2 + header + 6) != STAGE2_SECTORS ||
      read_le32(stage2 + header + 8) != KERNEL_LBA ||
      read_le32(stage2 + header + 16) != KERNEL_LOAD ||
      read_le32(stage2 + header + 12) != 0 ||
      read_le64(stage2 + header + 20) != 0) {
    fprintf(stderr, "mkboot: invalid or already-patched NVB2 stage2 header\n");
    goto done;
  }
  kernel_sectors = (kernel_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
  if (kernel_sectors == 0 || kernel_sectors > 0x007FF800ULL) {
    fprintf(stderr, "mkboot: kernel image does not fit below 4 GiB\n");
    goto done;
  }
  padded_kernel_size = kernel_sectors * SECTOR_SIZE;
  write_le32(stage2 + header + 12, (uint32_t)kernel_sectors);
  write_le64(stage2 + header + 20, (uint64_t)kernel_size);

  output = fopen(argv[4], "wb");
  if (output == NULL) {
    fprintf(stderr, "mkboot: cannot create %s\n", argv[4]);
    goto done;
  }
  if (fwrite(bootsect, 1, bootsect_size, output) == bootsect_size &&
      fwrite(stage2, 1, stage2_size, output) == stage2_size &&
      fwrite(kernel, 1, kernel_size, output) == kernel_size &&
      write_zeros(output, (size_t)(padded_kernel_size - kernel_size))) {
    uint64_t total = bootsect_size + stage2_size + padded_kernel_size;
    uint64_t min_total = (uint64_t)MIN_IMAGE_SECTORS * SECTOR_SIZE;
    ok = total >= min_total ||
         write_zeros(output, (size_t)(min_total - total));
    if (!ok) {
      fprintf(stderr, "mkboot: failed while padding %s to %llu bytes\n",
              argv[4], (unsigned long long)min_total);
    }
  }
  if (fclose(output) != 0) {
    ok = 0;
  }
  if (!ok) {
    remove(argv[4]);
    fprintf(stderr, "mkboot: failed while writing %s\n", argv[4]);
    goto done;
  }
  fprintf(stdout, "BIOS image: %s (%llu kernel sectors, %llu bytes)\n",
          argv[4], (unsigned long long)kernel_sectors,
          (unsigned long long)kernel_size);

done:
  free(bootsect);
  free(stage2);
  free(kernel);
  return ok ? 0 : 1;
}