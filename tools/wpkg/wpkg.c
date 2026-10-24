/*
 * wpkg - build the Novus wallpaper package.
 *
 * The photos are shipped as one file on the ESP (WPAPERS.PKG), not
 * compiled into the loader: the boot stage reads the file into reserved pages
 * before ExitBootServices, publishes the range through shared memory, and the
 * kernel's wallpaper layer indexes into it. Keeping the photos out of the PE
 * image means the loader stays small and a medium without the file still
 * boots - nd_wallpaper then paints a flat colour instead.
 *
 * Layout, all little-endian, offsets from the start of the file:
 *
 *   0   magic[4]  "NWPK"
 *   4   UINT32    version (1)
 *   8   UINT32    image count
 *   12  UINT32    package size in bytes (the whole file)
 *   16  entry[count], 40 bytes each:
 *          char   name[32]   NUL-padded, for the picker
 *          UINT32 offset     absolute, into this file
 *          UINT32 size       bytes
 *   16+40*count   the JPEG blobs, back to back
 *
 * The kernel parses this in place out of the staged pages, so nothing here
 * is compressed or padded: what mkfat writes is what the decoder reads.
 * The constants and offsets come from wpkg.h, which the kernel includes
 * too, so the two sides cannot drift apart.
 *
 * Usage: wpkg <out.pkg> <photo.jpg>...
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../graphics/wpkg.h"

static void die(const char *msg) {
    fprintf(stderr, "wpkg: %s\n", msg);
    exit(1);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* Read a whole file into memory. */
static uint8_t *slurp(const char *path, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long len;

    if (f == NULL) {
        fprintf(stderr, "wpkg: cannot open %s\n", path);
        exit(1);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        die("seek failed");
    }
    len = ftell(f);
    if (len <= 0) {
        fprintf(stderr, "wpkg: %s is empty\n", path);
        exit(1);
    }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)len);
    if (buf == NULL) {
        die("out of memory");
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        die("short read");
    }
    fclose(f);
    *out_size = (uint32_t)len;
    return buf;
}

/* The picker label: the file's base name, minus any extension, capped at
 * 31 characters so it always fits a NUL-terminated entry field. */
static void entry_name(const char *path, char *dst) {
    const char *base = strrchr(path, '/');
    const char *dot;
    size_t n;

    base = (base != NULL) ? base + 1 : path;
    dot = strrchr(base, '.');
    n = (dot != NULL) ? (size_t)(dot - base) : strlen(base);
    if (n > WPKG_NAME_MAX - 1u) {
        n = WPKG_NAME_MAX - 1u;
    }
    memcpy(dst, base, n);
    memset(dst + n, 0, WPKG_NAME_MAX - n);
}

int main(int argc, char **argv) {
    uint8_t *blob[WPKG_MAX_IMAGES];
    uint32_t blob_size[WPKG_MAX_IMAGES];
    char name[WPKG_MAX_IMAGES][WPKG_NAME_MAX];
    uint32_t count = 0;
    uint32_t offset;
    uint32_t total;
    uint8_t *pkg;
    FILE *out;
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: wpkg <out.pkg> <photo.jpg>...\n");
        return 2;
    }
    if ((uint32_t)(argc - 2) > WPKG_MAX_IMAGES) {
        die("too many images");
    }

    offset = WPKG_HEADER_SIZE + WPKG_ENTRY_SIZE * (uint32_t)(argc - 2);
    total = offset;
    for (i = 2; i < argc; i++) {
        blob[count] = slurp(argv[i], &blob_size[count]);
        entry_name(argv[i], name[count]);
        total += blob_size[count];
        count++;
    }

    pkg = (uint8_t *)malloc(total);
    if (pkg == NULL) {
        die("out of memory");
    }
    memset(pkg, 0, WPKG_HEADER_SIZE);
    pkg[0] = WPKG_MAGIC0;
    pkg[1] = WPKG_MAGIC1;
    pkg[2] = WPKG_MAGIC2;
    pkg[3] = WPKG_MAGIC3;
    put_le32(pkg + 4, WPKG_VERSION);
    put_le32(pkg + 8, count);
    put_le32(pkg + 12, total);

    {
        uint32_t at = offset;
        for (i = 0; (uint32_t)i < count; i++) {
            uint8_t *e = pkg + WPKG_HEADER_SIZE + WPKG_ENTRY_SIZE * (uint32_t)i;
            memcpy(e, name[i], WPKG_NAME_MAX);
            put_le32(e + 32, at);
            put_le32(e + 36, blob_size[i]);
            memcpy(pkg + at, blob[i], blob_size[i]);
            at += blob_size[i];
        }
    }

    out = fopen(argv[1], "wb");
    if (out == NULL) {
        fprintf(stderr, "wpkg: cannot create %s\n", argv[1]);
        return 1;
    }
    if (fwrite(pkg, 1, total, out) != total) {
        die("short write");
    }
    if (fclose(out) != 0) {
        die("close failed");
    }

    printf("wpkg: %s  %u images, %u bytes\n", argv[1], count, total);
    for (i = 0; (uint32_t)i < count; i++) {
        printf("wpkg:   [%d] %-32s %8u bytes\n", i, name[i], blob_size[i]);
        free(blob[i]);
    }
    free(pkg);
    return 0;
}
