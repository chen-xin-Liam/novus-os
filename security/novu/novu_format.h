/**
 * Novus OS - update carrier format (frozen ABI).
 *
 * Single source of truth for the 128-byte update header, the four carrier
 * magics, the five priority levels, the flag bits and the control ABI shared
 * between the kernel update service, the host packer (tools/novupkg) and the
 * shell/settings UIs. It is intentionally freestanding-friendly: when the
 * kernel's own <uefi.h> typedefs are not visible it falls back to <stdint.h>,
 * which is ABI-compatible (both are unsigned int / unsigned long long here).
 *
 * NOTE: the spec's "7.3" originally summed the field offsets to 168 bytes.
 * This header is the corrected, frozen layout: the header is exactly 128
 * bytes; file_sha256 lives at 0x48, sig_offset/sig_len at 0x68, the 8-byte
 * patch_uuid at 0x70, and reserved_fill[8] at 0x78.
 */
#ifndef NOVUS_NOVU_FORMAT_H
#define NOVUS_NOVU_FORMAT_H

#ifndef UINT8
#include <stdint.h>
typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#endif
#ifndef INT32
typedef int32_t  INT32;
typedef int64_t  INT64;
#endif

/* ---- carrier types (header.file_type) ---- */
#define NOVU_TYPE_FULL      0u   /* .novu  full system image          */
#define NOVU_TYPE_SYS_PATCH 1u   /* .npth  cumulative system patch    */
#define NOVU_TYPE_APP_PATCH 2u   /* .apth  application patch          */
#define NOVU_TYPE_HOT_PATCH 3u   /* .hotpth emergency hotfix          */

/* ---- priority levels (header.update_level) ---- */
#define UPDATE_LVL_OPTIONAL 0u
#define UPDATE_LVL_RECOMMEND 1u
#define UPDATE_LVL_PREVIEW 2u
#define UPDATE_LVL_URGENT 3u
#define UPDATE_LVL_DEV 4u

/* ---- four magics (little-endian on disk) ---- */
#define NOVU_FULL_MAGIC   0x554E5653u  /* "UNVS" */
#define NOVU_NPTH_MAGIC   0x50544853u  /* "PTHS" */
#define NOVU_APTH_MAGIC   0x41505448u  /* "APTH" */
#define NOVU_HOTPTH_MAGIC 0x484F5450u  /* "HOTP" */

#define NOVUS_UPDATE_HDR_VER_MAJOR 1u
#define NOVUS_UPDATE_HDR_VER_MINOR 0u

/* ---- flag bits (header.flag_bits) ---- */
#define NOVU_F_SNAPSHOT     (1u << 0)  /* take pre-install snapshot      */
#define NOVU_F_DELTA        (1u << 1)  /* delta (vs full) transfer       */
#define NOVU_F_RESUMABLE    (1u << 2)  /* support resume                 */
#define NOVU_F_KERNEL_HOT   (1u << 3)  /* kernel hot-update              */
#define NOVU_F_FORCE_RO     (1u << 4)  /* overwrite read-only path       */
#define NOVU_F_REFRESH_DESKTOP (1u << 5) /* refresh desktop service     */
#define NOVU_F_REMOTE_SILENT (1u << 6) /* server-side silent rollout     */
#define NOVU_F_SECURITY     (1u << 7)  /* security hard fallback         */

/* ---- on-disk 128-byte header (packed, little-endian) ---- */
typedef struct __attribute__((packed)) {
    UINT32 magic;
    UINT16 ver_major;
    UINT16 ver_minor;
    UINT8  file_type;
    UINT8  update_level;
    UINT8  remote_support;
    UINT8  reserved_0;
    UINT32 flag_bits;
    UINT64 target_os_ver;
    UINT64 min_compat_ver;
    UINT32 entry_count;          /* manifest entries                */
    UINT32 delta_block_count;    /* delta blocks (if NOVU_F_DELTA)  */
    UINT32 meta_offset;          /* 0x20 */
    UINT32 meta_len;
    UINT32 table_offset;         /* 0x28 */
    UINT32 table_len;
    UINT32 data_offset;          /* 0x30 */
    UINT32 data_len;
    UINT32 snapshot_offset;      /* 0x38 */
    UINT32 snapshot_len;
    UINT8  file_sha256[32];      /* 0x48 */
    UINT32 sig_offset;           /* 0x68 */
    UINT32 sig_len;              /* 0x6C */
    UINT8  patch_uuid_bin[8];    /* 0x70 */
    UINT8  reserved_fill[8];     /* 0x78 */
} novus_update_header_t;

_Static_assert(sizeof(novus_update_header_t) == 128u, "update header drift");

/* ---- manifest entry (in meta region, newline separated) ---- */
/* "PUT <off> <len> <dst>"  or  "DEL <dst>"                       */
#define NOVU_MANIFEST_PUT 0u
#define NOVU_MANIFEST_DEL 1u
typedef struct __attribute__((packed)) {
    UINT8  op;
    UINT32 off;
    UINT32 len;
    UINT8  dst_len;
    char   dst[120];
} novus_manifest_entry_t;

/* ---- delta block descriptor (in table region) ---- */
typedef struct __attribute__((packed)) {
    UINT64 src_off;   /* offset in installed file                    */
    UINT32 src_len;   /* literal bytes copied verbatim               */
    UINT32 patch_off; /* offset into data region of new/patched bytes */
    UINT32 patch_len;
} novus_delta_block_t;

/* ---- snapshot entry (in snapshot region) ---- */
typedef struct __attribute__((packed)) {
    UINT8  kind;      /* 0=file 1=dir 2=config 3=service            */
    UINT8  name_len;
    char   name[120];
    UINT64 mtime;
    UINT32 perms;
} novus_snapshot_entry_t;

/* ---- update index registry record (persisted, fixed 64 entries) ---- */
typedef struct __attribute__((packed)) {
    UINT8  uuid[8];
    UINT8  type;
    UINT8  level;
    UINT8  state;     /* 0=detected 1=queued 2=installing 3=installed
                         4=rolled_back 5=withdrawn 6=locked          */
    UINT8  lock;      /* 0=revocable 1=baseline-locked 2=dev-exempt */
    UINT32 installed_at;
    UINT32 expires_at;
    UINT64 before_ver;
    UINT64 after_ver;
    UINT8  revocable;
    UINT8  reserved[3];
    char   name[48];
} novus_index_record_t;

/* ---- runtime control ABI (syscall SYS_UPDATE) ---- */
#define NOVU_OP_STATUS   0u   /* query service state           */
#define NOVU_OP_LIST     1u   /* enumerate index records       */
#define NOVU_OP_CHECK    2u   /* trigger a check (local/remote)*/
#define NOVU_OP_INSTALL  3u   /* install by uuid               */
#define NOVU_OP_DEFER    4u   /* defer an optional/recommend   */
#define NOVU_OP_IGNORE   5u   /* ignore an update              */
#define NOVU_OP_ROLLBACK 6u   /* withdraw/rollback by uuid     */
#define NOVU_OP_RETRY    7u   /* retry a failed update         */
#define NOVU_OP_CONFIG   8u   /* read/write policy             */

typedef struct __attribute__((packed)) {
    UINT32 op;
    UINT8  uuid[8];
    char   path[128];
    char   url[192];
    UINT8  level;
    UINT8  flags;
    UINT8  reserved[2];
} user_update_req_t;
_Static_assert(sizeof(user_update_req_t) == 336u, "update req ABI drift");

typedef struct __attribute__((packed)) {
    UINT8  uuid[8];
    UINT8  type;
    UINT8  level;
    UINT8  state;
    UINT8  lock;
    UINT32 installed_at;
    UINT32 expires_at;
    UINT64 before_ver;
    UINT64 after_ver;
    char   name[48];
} user_update_info_t;
_Static_assert(sizeof(user_update_info_t) == 84u, "update info ABI drift");

/* ---- HTTPS control ABI (syscall SYS_HTTPS) ---- */
#define NOVU_HTTP_GET  0u
#define NOVU_HTTP_HEAD 1u
#define NOVU_HTTP_POST 2u
typedef struct __attribute__((packed)) {
    char   host[128];
    char   path[192];
    UINT16 port;
    UINT8  method;
    UINT8  reserved;
    UINT64 range_start;
    UINT32 range_len;
    UINT32 response_cap;
} user_https_req_t;
_Static_assert(sizeof(user_https_req_t) == 340u, "https req ABI drift");

/* ---- service state (returned by NOVU_OP_STATUS) ---- */
#define NOVU_SVC_IDLE      0u
#define NOVU_SVC_CHECKING  1u
#define NOVU_SVC_DOWNLOAD  2u
#define NOVU_SVC_VERIFY    3u
#define NOVU_SVC_INSTALL   4u
#define NOVU_SVC_ROLLBACK  5u
#define NOVU_SVC_ERROR     6u

/* fixed capacities (no allocator in kernel / apps) */
#define NOVU_QUEUE_MAX   64u
#define NOVU_INDEX_MAX   64u
#define NOVU_JOURNAL_MAX 64u

#endif /* NOVUS_NOVU_FORMAT_H */
