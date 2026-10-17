/**
 * Novus OS - Novus Kernel Module (NKM) loader, M3 (README part 5).
 *
 * An NKM is a flat image: a fixed header, a segment table, optional
 * symbol and relocation tables, and the segment payloads. The header
 * carries a SHA-256 digest of the module and an Ed25519 signature over
 * that digest, made by a root key the kernel already trusts.
 *
 * The signing contract, frozen here because the host signing tool and
 * the kernel verifier must never disagree about it:
 *
 *   1. digest  = SHA-256 over every byte of the module with BOTH
 *                header.hash[32] and header.sig[64] set to zero;
 *   2. sig     = Ed25519 signature over the 32 raw digest bytes
 *                (not over the module, not over its hex form);
 *   3. header.key_id selects the root public key, which is what makes
 *                key rotation possible without reflashing the kernel.
 *
 * The digest is taken with both fields zeroed rather than skipped
 * because the module is hashed as one contiguous byte range; zeroing is
 * the same operation for the signer and the verifier and needs no
 * special-casing of the header layout.
 *
 * Failure is always a returned reason code, never a halt. An unsigned,
 * tampered or wrongly-signed module is refused and the kernel carries
 * on with whatever it already loaded (README 17, README 3.4).
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

/* ------------------------------------------------------------------ */
/* Frozen on-disk format                                               */
/* ------------------------------------------------------------------ */

#define NKM_MAGIC          0x004D4B4Eu   /* "NKM\0" */
#define NKM_VERSION        1u
#define NKM_ABI_VERSION    1u

#define NKM_HASH_LEN       32u
#define NKM_SIG_LEN        64u
#define NKM_PUBKEY_LEN     32u

/* sizeof(nkm_header_t): pinned here so a layout change cannot slip in
 * unnoticed - the host signing tool sizes its output by this constant.
 *
 *   0  magic u32      4  version u16     6  abi_version u16
 *   8  seg_count u32  12 seg_table_off   20 symtab_off   28 symtab_len
 *  36  reloc_off      44 reloc_len       52 stack_size   60 compile_flag
 *  64  key_id         68 hash[32]       100 sig[64]    -> 164
 */
#define NKM_HEADER_SIZE    164u
#define NKM_SEGMENT_SIZE   32u

#define NKM_MAX_SEGMENTS   8u
#define NKM_MAX_MODULES    16u
#define NKM_MAX_ROOT_KEYS  4u

/* Segment types. */
#define NKM_SEG_CODE       1u
#define NKM_SEG_RODATA     2u
#define NKM_SEG_DATA       3u

/* Segment flags. */
#define NKM_SEG_EXEC       0x1u
#define NKM_SEG_WRITE      0x2u

/* compile_flag: how the module was built. PIC is required today - the
 * loader does not relocate absolute addresses. */
#define NKM_CF_PIC         0x1u
#define NKM_CF_DEBUG       0x2u

/* Byte offsets of the two fields the digest clears. Stated explicitly
 * so the verifier does not depend on struct member arithmetic. */
#define NKM_HASH_OFF       68u
#define NKM_SIG_OFF        100u

typedef struct NOVUS_NKM_PACKED nkm_segment {
  UINT32 type;
  UINT64 vaddr;
  UINT64 file_off;
  UINT64 len;
  UINT32 flags;
} nkm_segment_t;

typedef struct NOVUS_NKM_PACKED nkm_header {
  UINT32 magic;
  UINT16 version;
  UINT16 abi_version;
  UINT32 seg_count;
  UINT64 seg_table_off;
  UINT64 symtab_off;
  UINT64 symtab_len;
  UINT64 reloc_off;
  UINT64 reloc_len;
  UINT64 stack_size;
  UINT32 compile_flag;
  UINT32 key_id;          /* which root key signed this module */
  UINT8  hash[NKM_HASH_LEN]; /* zeroed before the digest is taken */
  UINT8  sig[NKM_SIG_LEN];   /* zeroed before the digest is taken */
} nkm_header_t;

/* ------------------------------------------------------------------ */
/* Load results                                                        */
/* ------------------------------------------------------------------ */
/* nkm_load() returns the slot index (>= 0) on success or one of these
 * negative reasons. Every one of them is a refusal, not a crash. */

#define NKM_REJECT_MALFORMED (-1) /* too short, bad magic/version/ABI   */
#define NKM_REJECT_LAYOUT    (-2) /* tables or segments out of bounds   */
#define NKM_REJECT_HASH      (-3) /* digest does not match header.hash  */
#define NKM_REJECT_KEY       (-4) /* key_id names no trusted root key   */
#define NKM_REJECT_SIG       (-5) /* Ed25519 verification failed        */
#define NKM_REJECT_NOSLOT    (-6) /* module table full                  */
#define NKM_REJECT_NOMEM     (-7) /* could not copy the image           */

/* A loaded module, as registered in the kernel module table. */
typedef struct {
  UINTN            in_use;
  UINT32           key_id;
  UINT32           compile_flag;
  UINT64           stack_size;
  UINT32           seg_count;
  UINT64           image_len;
  UINT8           *image;                 /* owned copy, digest-clean */
  UINT8            digest[NKM_HASH_LEN];  /* recomputed from the image */
  nkm_segment_t    segs[NKM_MAX_SEGMENTS];
} nkm_module_t;

/* Verify and register `image`. The caller keeps ownership of `image`;
 * the loader takes a private copy because it has to zero hash and sig
 * before hashing, and mutating the caller's buffer would be rude.
 *
 * Returns the slot index on success, or a negative NKM_REJECT_* code.
 */
int nkm_load(const void *image, UINT64 len);

/* Drop a module and release its copy. Returns 0, or -1 for a bad slot. */
int nkm_unload(UINTN slot);

/* Number of registered modules. */
UINTN nkm_loaded_count(void);

/* Read-only view of a slot, or NULL when the slot is empty. */
const nkm_module_t *nkm_get(UINTN slot);

/* Human-readable text for a nkm_load() reason code, for the log. */
const char *nkm_reject_text(int rc);

/* Self test: every refusal path plus one signed module when the
 * generated test module is compiled in. Returns 0 when all pass. */
int nkm_selftest(void);

#endif /* NOVUS_NKM_H */
