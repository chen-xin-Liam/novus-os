/**
 * Novus OS - Novus Kernel Module (NKM) loader (README part 5,
 * "NKM 内核模块二进制格式规范 V1.0").
 *
 * The on-disk format is the spec, not a matter of taste, and this header
 * is the kernel's half of it: every size and offset below is pinned with
 * a _Static_assert in nkm.c so a drift becomes a build failure rather
 * than a module that silently fails to verify.
 *
 * The signing contract, frozen in favour of neither side being able to
 * reinterpret it:
 *
 *   1. digest = SHA-256 over the whole file with bytes [0x50, 0xB0)
 *               - module_hash[32] and module_sig[64] - set to zero;
 *   2. sig    = Ed25519 over the 32 raw digest bytes, not over the file;
 *   3. verification polls the whole trusted key pool; there is no key_id
 *               field in V1.0, so rotation means shipping a new kernel.
 *
 * Zeroing the two fields rather than skipping them is what decouples the
 * digest from the signature: the digest covers everything except the two
 * self-describing fields, and the signature then covers the digest.
 *
 * Failure is always a returned NOVUS_* code, never a halt. An unsigned,
 * tampered or wrongly-signed module is refused, whatever it allocated is
 * released and the kernel carries on (README 17, README 3.4).
 */

#ifndef NOVUS_NKM_H
#define NOVUS_NKM_H

#include "uefi.h"
#include "kernel.h"

/* The kernel is freestanding: there is no <stddef.h> to bring NULL in,
 * so each subsystem header that needs it defines it, as pmm.h and
 * blkdev.h already do. */
#ifndef NULL
#define NULL ((VOID *)0)
#endif

/* Packed on-disk structures: no compiler-inserted padding. */
#define NOVUS_NKM_PACKED __attribute__((packed))

/* These records are serialized ABI, not native in-memory structs: their
 * packed sizes and selected field offsets are asserted by nkm.c. Keep any
 * layout change coordinated with the host module signer and its format
 * version, or existing signed images will no longer match this parser. */

/* ------------------------------------------------------------------ */
/* Frozen on-disk format (spec V1.0)                                   */
/* ------------------------------------------------------------------ */

#define NKM_MAGIC0      'N'
#define NKM_MAGIC1      'K'
#define NKM_MAGIC2      'M'
#define NKM_MAGIC3      '!'

/* abi_version is major in the high byte, minor in the low byte. */
#define NKM_ABI_MAJOR   1u
#define NKM_ABI_MINOR   0u
#define NKM_ABI_VERSION ((UINT16)((NKM_ABI_MAJOR << 8) | NKM_ABI_MINOR))

/* Header size. The spec's prose said 128, but its own field table puts
 * reserved[32] at 0xB0, which needs 0xD0; the frozen digest rule
 * (zero 0x50..0xB0) only holds for 0xD0. The offset table wins, and the
 * document has been corrected to match. */
#define NKM_HEADER_SIZE 208u /* 0xD0 */

#define NKM_NAME_LEN    32u
#define NKM_HASH_LEN    32u
#define NKM_SIG_LEN     64u
#define NKM_PUBKEY_LEN  32u

/* Byte range the digest clears: module_hash and module_sig, adjacent by
 * construction. Stated as an offset and a length, exactly as the spec
 * states it, so the verifier does not depend on struct arithmetic. */
#define NKM_HASH_OFF    0x50u
#define NKM_SIG_OFF     0x70u
#define NKM_ZERO_OFF    NKM_HASH_OFF
#define NKM_ZERO_LEN    (NKM_HASH_LEN + NKM_SIG_LEN) /* 96 */

/* Table entry sizes, all frozen by the spec. */
#define NKM_DEP_SIZE     16u
#define NKM_SEGMENT_SIZE 32u
#define NKM_SYM_SIZE     24u
#define NKM_REL_SIZE     16u

/* Spec limits: seg_count is 1..16, dep_count is unbounded in principle
 * but a kernel module with more than eight dependencies is a design
 * error, and the table has to be bounded to be checked. */
#define NKM_MAX_SEGMENTS 16u
#define NKM_MAX_DEPS     8u
#define NKM_MAX_SYMS     64u
#define NKM_MAX_RELOCS   256u
#define NKM_MAX_MODULES  16u

/* Spec section 6.2: at most four root keys, fixed at compile time, no
 * runtime import. trusted_key_count is how many are actually filled. */
#define NKM_MAX_ROOT_KEYS 4u

/* Segments are placed on 4 KiB boundaries inside the module's region,
 * mirroring the 4 KiB alignment the spec requires of segment data. */
#define NKM_SEG_ALIGN 4096u

/* module_type (spec 3.3). */
#define NKM_TYPE_DRIVER 0u
#define NKM_TYPE_FS     1u
#define NKM_TYPE_COMPAT 2u
#define NKM_TYPE_CRYPTO 3u
#define NKM_TYPE_OTHER  4u

/* Segment types and flags. */
#define NKM_SEG_CODE    1u
#define NKM_SEG_RODATA  2u
#define NKM_SEG_DATA    3u
#define NKM_SEG_BSS     4u

#define NKM_SEG_EXEC    0x1u
#define NKM_SEG_WRITE   0x2u
#define NKM_SEG_READ    0x4u

/* Symbol binding, low nibble of nkm_sym_t.info; the segment a defined
 * symbol lives in is the next nibble. */
#define NKM_SYM_LOCAL   0u
#define NKM_SYM_GLOBAL  1u
#define NKM_SYM_EXTERN  2u /* undefined: resolved against the kernel */

/* Relocation types (spec 5: a kernel module only ever needs the 64-bit
 * absolute form; PC32 is not used and is refused). */
#define NKM_REL_ABS64   1u /* R_X86_64_64 */

/* Load flags (spec 10). */
#define NKM_FLAG_NONE           0x0u
#define NKM_FLAG_ALLOW_UNSIGNED 0x1u /* administrator, this session only */

/* ------------------------------------------------------------------ */
/* Error codes (spec 11)                                               */
/* ------------------------------------------------------------------ */
/* nkm_load() and nkm_unload() return NOVUS_OK (0) or one of these; the
 * values are the spec's, so they are usable as syscall returns later. */

#define NOVUS_OK        0
#define NOVUS_EINVAL    (-1)  /* header field illegal                  */
#define NOVUS_ENOMEM    (-2)  /* kernel memory exhausted               */
#define NOVUS_ENOTFOUND (-3)  /* a dependency is not loaded            */
#define NOVUS_EPERM     (-4)  /* missing capability                    */
#define NOVUS_EBUSY     (-5)  /* module in use by another module       */
#define NOVUS_EIO       (-6)  /* image could not be read               */
#define NOVUS_ECORRUPT  (-7)  /* SHA-256 digest mismatch               */
#define NOVUS_EMODULE   (-8)  /* Ed25519 verification failed           */
#define NOVUS_EDEP      (-12) /* dependency cycle                      */

/* ------------------------------------------------------------------ */
/* On-disk structures                                                  */
/* ------------------------------------------------------------------ */

typedef struct NOVUS_NKM_PACKED nkm_header {
  UINT8  magic[4];            /* 0x00 "NKM!"                          */
  UINT16 abi_version;         /* 0x04 major<<8 | minor                */
  UINT16 header_size;         /* 0x06 = 208                           */
  UINT64 min_kernel_ver;      /* 0x08 lowest compatible kernel        */
  UINT32 module_type;         /* 0x10 NKM_TYPE_*                      */
  UINT16 seg_count;           /* 0x14 1..16                           */
  UINT16 dep_count;           /* 0x16                                 */
  UINT32 symtab_count;        /* 0x18                                 */
  UINT32 rel_count;           /* 0x1C                                 */
  UINT32 strtab_offset;       /* 0x20                                 */
  UINT32 rel_offset;          /* 0x24                                 */
  UINT32 init_offset;         /* 0x28 nkm_init within the region      */
  UINT32 exit_offset;         /* 0x2C nkm_exit within the region      */
  UINT8  module_name[32];     /* 0x30 NUL-terminated                  */
  UINT8  hash[NKM_HASH_LEN];  /* 0x50 zeroed before the digest        */
  UINT8  sig[NKM_SIG_LEN];    /* 0x70 zeroed before the digest        */
  UINT8  reserved[32];        /* 0xB0 must be zero                    */
} nkm_header_t;

typedef struct NOVUS_NKM_PACKED nkm_dep {
  UINT32 dep_name_off;        /* 0x00 into the string table           */
  UINT64 min_dep_ver;         /* 0x04                                 */
  UINT32 reserved;            /* 0x0C must be zero                    */
} nkm_dep_t;

typedef struct NOVUS_NKM_PACKED nkm_segment {
  UINT32 type;                /* 0x00 NKM_SEG_*                       */
  UINT64 vaddr;               /* 0x04 link-time address, kernel space */
  UINT64 file_off;            /* 0x0C 0 = no image bytes (BSS)        */
  UINT64 len;                 /* 0x14                                 */
  UINT32 flags;               /* 0x1C NKM_SEG_*                       */
} nkm_segment_t;

typedef struct NOVUS_NKM_PACKED nkm_sym {
  UINT32 name_off;            /* 0x00 into the string table           */
  UINT64 value;               /* 0x04 offset within its segment       */
  UINT64 size;                /* 0x0C                                 */
  UINT16 info;                /* 0x14 binding | seg_index << 4        */
  UINT16 reserved;            /* 0x16 must be zero                    */
} nkm_sym_t;

typedef struct NOVUS_NKM_PACKED nkm_rel {
  UINT64 where;               /* 0x00 offset within the region        */
  UINT32 sym;                 /* 0x08 symbol table index              */
  UINT32 type;                /* 0x0C NKM_REL_*                       */
} nkm_rel_t;

/* ------------------------------------------------------------------ */
/* Loaded module record                                                */
/* ------------------------------------------------------------------ */
/* `region` is one contiguous run of pages holding every segment at a
 * 4 KiB-aligned offset; seg_load[] are those offsets. Relocations and
 * the two entry points are all expressed relative to the region base,
 * which is what lets a module be position independent: the spec's
 * per-segment vaddr is kept for the record but the kernel has no
 * address-space allocator yet, so it assigns the real address. */

typedef struct {
  UINTN  in_use;
  UINT8  name[NKM_NAME_LEN];
  UINT32 module_type;
  UINT64 min_kernel_ver;
  UINT32 seg_count;
  UINT32 dep_count;
  UINT8  deps[NKM_MAX_DEPS][NKM_NAME_LEN];
  UINT32 key_index;                  /* trusted key that verified it   */
  UINT64 image_len;
  UINT8 *image;                      /* private copy, digest-clean     */
  UINT8  digest[NKM_HASH_LEN];
  UINTN  region_pages;
  UINT8 *region;                     /* one contiguous run             */
  UINT64 seg_load[NKM_MAX_SEGMENTS]; /* offset from region base        */
  UINT64 seg_len[NKM_MAX_SEGMENTS];
  UINT32 seg_type[NKM_MAX_SEGMENTS];
  UINT32 seg_flags[NKM_MAX_SEGMENTS];
  int (*init)(void);
  int (*exit)(void);
} nkm_module_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Verify, map, relocate and start `image`. The caller keeps ownership;
 * the loader takes a private copy because the digest rule requires hash
 * and sig to read as zeroes.
 *
 * Returns the slot index (>= 0) on success, or a negative NOVUS_* code.
 * NKM_FLAG_ALLOW_UNSIGNED skips the signature step only (spec 10); the
 * digest is always checked. */
int nkm_load(const void *image, UINT64 len, UINT32 flags);

/* Stop a module, unmap it and free everything it owns. Returns NOVUS_OK,
 * NOVUS_EINVAL for a bad slot or NOVUS_EBUSY when another loaded module
 * still depends on it. */
int nkm_unload(UINTN slot);

/* Slot of the module named `name`, or -1 when none is loaded. */
int nkm_find(const char *name);

UINTN nkm_loaded_count(void);

/* Read-only view of a slot, or NULL when the slot is empty. */
/* The returned record and its region/image pointers are owned by the
 * loader; they remain usable only while that slot stays loaded. Unload
 * may run the registered exit hook and then releases both allocations,
 * so clients must not retain these pointers across unload. */
const nkm_module_t *nkm_get(UINTN slot);

/* Human-readable text for an error code, for the log. */
const char *nkm_err_text(int rc);

/* Self test: every refusal path, dependency handling, relocation and the
 * generated signed module. Returns 0 when all pass. */
int nkm_selftest(void);

#endif /* NOVUS_NKM_H */
