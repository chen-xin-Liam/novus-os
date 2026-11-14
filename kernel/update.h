/**
 * Novus OS - update package header (frozen by
 * "Novus OS 自研分级增量更新体系完整规范", section 7).
 *
 * Every update carrier (.novu / .npth / .apth / .hotpth) begins with this
 * 144-byte header. The on-disk layout is the source of truth; the Rust
 * packer/parser (tools/novupkg, apps/src/update.rs) and any kernel helper
 * must agree on these offsets exactly.
 *
 * NOTE: the spec text says "128 byte", but the field table sums to 0x90
 * (144) bytes; we follow the field table (the larger, exact layout) and
 * document the discrepancy in the spec revision.
 *
 * The header is `packed` (no padding) and little-endian on both the file
 * and in memory. Magic is the authoritative type discriminant - the
 * filename suffix is only for display.
 */

#ifndef NOVUS_UPDATE_H
#define NOVUS_UPDATE_H

#include "uefi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Header version (frozen). */
#define NOVUS_UPDATE_HDR_VER_MAJOR 1u
#define NOVUS_UPDATE_HDR_VER_MINOR 0u

/* File type (file_type field, 0x08). */
#define NOVU_TYPE_FULL      0u   /* .novu  full install image       */
#define NOVU_TYPE_SYS_PATCH 1u   /* .npth  system cumulative patch   */
#define NOVU_TYPE_APP_PATCH 2u   /* .apth  application patch         */
#define NOVU_TYPE_HOT_PATCH 3u   /* .hotpth emergency hotfix         */

/* Five-tier update channel (update_level field, 0x09). */
#define NOVU_LVL_OPTIONAL  0u    /* level_optional  */
#define NOVU_LVL_RECOMMEND 1u    /* level_recommend */
#define NOVU_LVL_PREVIEW   2u    /* level_preview   */
#define NOVU_LVL_URGENT    3u    /* level_urgent    */
#define NOVU_LVL_DEV       4u    /* level_dev       */

/* Per-type magic numbers (header[0x00]); suffix is display-only. */
#define NOVU_FULL_MAGIC   0x554E5653u  /* "NVUS" -> .novu  */
#define NOVU_NPTH_MAGIC   0x50544853u  /* "PTHS" -> .npth  */
#define NOVU_APTH_MAGIC   0x41505448u  /* "APTH" -> .apth  */
#define NOVU_HOTPTH_MAGIC 0x484F5450u  /* "HOTP" -> .hotpth */

/* Flag bit mask (flag_bits field, 0x0C). */
#define NOVU_F_SNAPSHOT   (1u << 0)  /* bit0  enable snapshot rollback     */
#define NOVU_F_DELTA      (1u << 1)  /* bit1  delta mode (0 = full replace)*/
#define NOVU_F_RESUME     (1u << 2)  /* bit2  resumable download           */
#define NOVU_F_HOTKERNEL  (1u << 3)  /* bit3  kernel hot-update (no reboot)*/
#define NOVU_F_RWROPATH   (1u << 4)  /* bit4  may overwrite ro system path */
#define NOVU_F_REFRESHUI  (1u << 5)  /* bit5  refresh desktop/services     */
#define NOVU_F_SILENT     (1u << 6)  /* bit6  remote silent install       */
#define NOVU_F_FORCE      (1u << 7)  /* bit7  security forced fallback     */

/* On-disk offsets (byte offsets into the header). */
#define NOVU_OFF_MAGIC          0x00
#define NOVU_OFF_VER            0x04
#define NOVU_OFF_FILE_TYPE      0x08
#define NOVU_OFF_UPDATE_LEVEL   0x09
#define NOVU_OFF_REMOTE         0x0A
#define NOVU_OFF_FLAGS          0x0C
#define NOVU_OFF_TARGET_VER     0x10
#define NOVU_OFF_MIN_VER        0x18
#define NOVU_OFF_ENTRY_COUNT    0x20
#define NOVU_OFF_DELTA_COUNT    0x24
#define NOVU_OFF_META           0x28
#define NOVU_OFF_TABLE          0x30
#define NOVU_OFF_DATA           0x38
#define NOVU_OFF_SNAPSHOT       0x40
#define NOVU_OFF_SHA256         0x48
#define NOVU_OFF_SIG            0x68
#define NOVU_OFF_UUID           0x70

#define NOVUS_UPDATE_HEADER_SIZE 144u   /* 0x90, packed */
#define NOVU_SHA256_LEN 32u
#define NOVU_ED25519_SIG_LEN 64u
#define NOVU_UUID_LEN 8u

/* Fixed 144-byte header layout. Parsers must read exactly this many bytes
 * and decode field-by-field; do not rely on sizeof(struct) across ABIs. */
typedef struct __attribute__((packed)) {
    UINT32 magic;           /* 0x00 four-type magic                  */
    UINT16 ver_major;       /* 0x04                                  */
    UINT16 ver_minor;       /* 0x06                                  */
    UINT8  file_type;       /* 0x08 0..3                             */
    UINT8  update_level;    /* 0x09 0..4                             */
    UINT8  remote_support;  /* 0x0A 1=remote 0=local only            */
    UINT8  reserved_0;      /* 0x0B                                  */
    UINT32 flag_bits;       /* 0x0C bit mask                         */
    UINT64 target_os_ver;   /* 0x10 target OS version                */
    UINT64 min_compat_ver;  /* 0x18 minimum compatible version       */
    UINT32 entry_count;     /* 0x20 file-op / resource entry count   */
    UINT32 delta_block_count; /* 0x24 delta block count (0 for full) */
    UINT32 meta_offset;     /* 0x28 manifest offset                  */
    UINT32 meta_len;        /* 0x2C                                  */
    UINT32 table_offset;    /* 0x30 op-index offset                  */
    UINT32 table_len;       /* 0x34                                  */
    UINT32 data_offset;     /* 0x38 payload offset                   */
    UINT32 data_len;        /* 0x3C                                  */
    UINT32 snapshot_offset; /* 0x40 snapshot hash region offset      */
    UINT32 snapshot_len;    /* 0x44                                  */
    UINT8  file_sha256[32]; /* 0x48 whole-file hash (excl. sig)      */
    UINT32 sig_offset;      /* 0x68 signature offset                 */
    UINT32 sig_len;         /* 0x6C                                  */
    UINT8  patch_uuid_bin[8]; /* 0x70 16-hex id, 8 raw bytes        */
    UINT8  reserved_fill[24]; /* 0x78 pad to 144                     */
} novus_update_header_t;

#ifdef __cplusplus
}
#endif

#endif /* NOVUS_UPDATE_H */
