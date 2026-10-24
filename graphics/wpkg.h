/**
 * Novus OS - wallpaper package (WPAPERS.PKG) layout.
 *
 * The photos ship as one file on the ESP rather than compiled into the
 * loader. The boot stage reads it into reserved pages before
 * ExitBootServices and publishes the range through shared memory; this
 * header is the single description of what is inside those pages, shared
 * by the host packer (tools/wpkg/wpkg.c) and graphics/nd_wallpaper.c.
 *
 * Everything is little-endian and offsets are from the start of the
 * package, so the header needs no type of its own: the kernel reads the
 * fields byte-wise and the host writes them with put_le32.
 *
 *   0    magic[4]  "NWPK"
 *   4    UINT32    version (WPKG_VERSION)
 *   8    UINT32    image count (WPKG_MAX_IMAGES ceiling)
 *   12   UINT32    package size in bytes
 *   16   entry[count], WPKG_ENTRY_SIZE bytes each:
 *           +0   char   name[WPKG_NAME_MAX]  NUL-padded picker label
 *           +32  UINT32 offset               absolute, into the package
 *           +36  UINT32 size                 bytes of the JPEG
 *   16+WPKG_ENTRY_SIZE*count   the JPEG blobs, back to back
 */

#ifndef NOVUS_WPKG_H
#define NOVUS_WPKG_H

#define WPKG_MAGIC0 0x4Eu /* 'N' */
#define WPKG_MAGIC1 0x57u /* 'W' */
#define WPKG_MAGIC2 0x50u /* 'P' */
#define WPKG_MAGIC3 0x4Bu /* 'K' */
#define WPKG_VERSION 1u

#define WPKG_NAME_MAX 32u
#define WPKG_ENTRY_SIZE 40u
#define WPKG_HEADER_SIZE 16u
#define WPKG_MAX_IMAGES 16u

/* Field offsets inside the header. */
#define WPKG_OFF_VERSION 4u
#define WPKG_OFF_COUNT 8u
#define WPKG_OFF_TOTAL 12u

/* Field offsets inside one entry. */
#define WPKG_EOFF_NAME 0u
#define WPKG_EOFF_OFFSET 32u
#define WPKG_EOFF_SIZE 36u

#endif /* NOVUS_WPKG_H */
