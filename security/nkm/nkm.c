/**
 * Novus OS - NKM loader, M3 (README part 5, NKM spec V1.0).
 *
 * Load follows the frozen order in spec section 8: bounds, digest,
 * signature, dependencies, map, relocate, init. The order is not
 * cosmetic. Bounds come first so no pointer arithmetic ever leaves the
 * image; the digest comes before the signature so an unsigned module is
 * refused on its own merits rather than being reported as a signature
 * failure; the signature comes last of the checks because it is the only
 * one that costs a double scalar multiplication.
 *
 * Two deviations from the spec, both forced by what the kernel has
 * today and both recorded where they apply:
 *
 *   - Segments are placed by the loader in one contiguous region rather
 *     than at their link-time vaddr. There is no address-space
 *     allocator yet, so the kernel assigns the address and records it;
 *     modules are position independent, so nothing inside one notices.
 *   - A dependency that is not already loaded is reported as
 *     NOVUS_ENOTFOUND instead of being fetched: the kernel stage has no
 *     module directory to read from. Recursion happens as soon as there
 *     is one.
 *
 * Nothing here halts the machine. Every refusal frees what it allocated,
 * leaves the module table untouched and returns a NOVUS_* code the
 * caller logs (README 17, README 3.4).
 */

#include "nkm.h"
#include "heap.h"
#include "vmm.h"
#include "mm.h"
#include "nksym.h"
#include "../../lib/novuslib_rs.h"

/* The generated modules, produced by tools/nkmgen from the same crypto
 * the kernel verifies with. They are what let the self test cover the
 * accept path; the refusal paths are built at run time and need no
 * signing key. */
#include "nkm_test_module.h"

/* ------------------------------------------------------------------ */
/* Layout assertions                                                   */
/* ------------------------------------------------------------------ */
/* The host signing tool sizes its output from these same numbers, so a
 * drift is a build failure here rather than a module that verifies on
 * the host and is refused by the kernel. */

_Static_assert(sizeof(nkm_header_t) == NKM_HEADER_SIZE,
               "nkm_header_t drifted from NKM_HEADER_SIZE");
_Static_assert(sizeof(nkm_dep_t) == NKM_DEP_SIZE, "nkm_dep_t size");
_Static_assert(sizeof(nkm_segment_t) == NKM_SEGMENT_SIZE, "nkm_segment_t size");
_Static_assert(sizeof(nkm_sym_t) == NKM_SYM_SIZE, "nkm_sym_t size");
_Static_assert(sizeof(nkm_rel_t) == NKM_REL_SIZE, "nkm_rel_t size");

/* The kernel is freestanding, so there is no <stddef.h> and no
 * offsetof(); this is the same cast the macro would use. */
#define NKM_OFFOF(type, member) ((UINTN)(UINT64)&(((type *)0)->member))

_Static_assert(NKM_OFFOF(nkm_header_t, hash) == NKM_HASH_OFF,
               "module_hash must sit at 0x50");
_Static_assert(NKM_OFFOF(nkm_header_t, sig) == NKM_SIG_OFF,
               "module_sig must sit at 0x70");
_Static_assert(NKM_HASH_OFF + NKM_HASH_LEN == NKM_SIG_OFF,
               "hash and sig must be adjacent: the digest zeroes one range");
_Static_assert(NKM_SIG_OFF + NKM_SIG_LEN == 0xB0u,
               "hash+sig must end where reserved begins");

/* ------------------------------------------------------------------ */
/* Trusted root keys (spec 6.2)                                        */
/* ------------------------------------------------------------------ */
/* At most four, fixed at compile time, never importable at runtime.
 * Verification polls every filled slot: a key is retired by shipping a
 * kernel that no longer carries it. */

static const UINT8 nkm_root_key_dev[NKM_PUBKEY_LEN] = {
    0x7a, 0x33, 0x7a, 0x9e, 0x91, 0xbd, 0xdf, 0xf2,
    0x4f, 0xd4, 0x88, 0xad, 0x6e, 0x9f, 0x57, 0xcd,
    0x12, 0xbc, 0x6d, 0x5c, 0x01, 0xbe, 0x64, 0x9c,
    0x39, 0x37, 0xee, 0xc3, 0xa3, 0x44, 0x20, 0x93,
};

static const UINT8 *const nkm_trusted_keys[NKM_MAX_ROOT_KEYS] = {
    nkm_root_key_dev,
};

static const UINTN nkm_trusted_key_count = 1u;

/* ------------------------------------------------------------------ */
/* Module table                                                        */
/* ------------------------------------------------------------------ */

static nkm_module_t nkm_modules[NKM_MAX_MODULES];

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static UINT64 nkm_align_up(UINT64 v, UINT64 a) {
  return (v + a - 1u) & ~(a - 1u);
}

/* A NUL-terminated string inside the image, or NULL when the offset is
 * out of bounds or the name runs off the end of the image. */
static const char *nkm_str(const UINT8 *image, UINT64 len, UINT32 off) {
  UINT64 i;

  if ((UINT64)off >= len) {
    return NULL;
  }
  for (i = (UINT64)off; i < len; i++) {
    if (image[i] == 0u) {
      return (const char *)(image + off);
    }
  }
  return NULL;
}

/* One contiguous run of pages. A module's segments have to be one
 * region: a function may not straddle a hole, and relocation offsets are
 * expressed from the region base. The PMM hands out ascending pages, so
 * asking for the next page and checking that it is the next address is
 * enough - and when memory is fragmented the answer is "no" rather than
 * a region that quietly is not contiguous. */
static UINT8 *nkm_alloc_region(UINTN pages) {
  UINT8 *base;
  UINTN  i;

  if (pages == 0u || pages > 64u) {
    return NULL;
  }
  base = (UINT8 *)vmm_alloc_page();
  if (base == NULL) {
    return NULL;
  }
  for (i = 1u; i < pages; i++) {
    UINT8 *p = (UINT8 *)vmm_alloc_page();

    if (p != base + i * MM_PAGE_SIZE) {
      UINTN j;

      if (p != NULL) {
        vmm_free_page(p);
      }
      for (j = 0u; j < i; j++) {
        vmm_free_page(base + j * MM_PAGE_SIZE);
      }
      return NULL;
    }
  }
  return base;
}

static void nkm_free_region(UINT8 *base, UINTN pages) {
  UINTN i;

  if (base == NULL) {
    return;
  }
  for (i = 0u; i < pages; i++) {
    vmm_free_page(base + i * MM_PAGE_SIZE);
  }
}

/* ------------------------------------------------------------------ */
/* Structural checks                                                   */
/* ------------------------------------------------------------------ */

/* Every table and every segment payload must lie inside the image, and
 * the tables must not overlap each other or the header. Fills in the
 * derived offsets the rest of the load needs. */
/* This runs before any image table is followed, while the image is still
 * untrusted input. Its returned offsets are therefore the bounds contract
 * used by later dependency, symbol, relocation and segment reads; a valid
 * signature does not substitute for these structural checks. */
static int nkm_check_layout(const nkm_header_t *h, UINT64 len, UINT32 *dep_off,
                            UINT32 *seg_off, UINT32 *sym_off,
                            UINT64 *data_min) {
  UINT64 dep_end;
  UINT64 seg_end;
  UINT64 sym_end;
  UINT64 rel_end;

  if (h->seg_count == 0u || h->seg_count > NKM_MAX_SEGMENTS) {
    return NOVUS_EINVAL;
  }
  if (h->dep_count > NKM_MAX_DEPS || h->symtab_count > NKM_MAX_SYMS ||
      h->rel_count > NKM_MAX_RELOCS) {
    return NOVUS_EINVAL;
  }
  if (h->module_type > NKM_TYPE_OTHER) {
    return NOVUS_EINVAL;
  }

  /* Tables follow the header in a fixed order: dependencies, segments,
   * symbols; the relocation and string tables carry their own offsets
   * because the spec gives them fields of their own. */
  *dep_off = (UINT32)NKM_HEADER_SIZE;
  *seg_off = (UINT32)(*dep_off + (UINT64)h->dep_count * NKM_DEP_SIZE);
  *sym_off = (UINT32)(*seg_off + (UINT64)h->seg_count * NKM_SEGMENT_SIZE);

  dep_end = *dep_off + (UINT64)h->dep_count * NKM_DEP_SIZE;
  seg_end = *seg_off + (UINT64)h->seg_count * NKM_SEGMENT_SIZE;
  sym_end = *sym_off + (UINT64)h->symtab_count * NKM_SYM_SIZE;
  rel_end = (UINT64)h->rel_offset + (UINT64)h->rel_count * NKM_REL_SIZE;

  if (dep_end > len || seg_end > len || sym_end > len) {
    return NOVUS_EINVAL;
  }
  if (h->rel_count != 0u) {
    if (h->rel_offset < sym_end || rel_end > len) {
      return NOVUS_EINVAL;
    }
  }
  if (h->strtab_offset != 0u) {
    if (h->strtab_offset < (h->rel_count != 0u ? rel_end : sym_end) ||
        (UINT64)h->strtab_offset > len) {
      return NOVUS_EINVAL;
    }
  }

  /* Segment payloads come after every table. */
  *data_min = sym_end;
  if (h->rel_count != 0u && rel_end > *data_min) {
    *data_min = rel_end;
  }
  if (h->strtab_offset != 0u && (UINT64)h->strtab_offset > *data_min) {
    *data_min = (UINT64)h->strtab_offset;
  }
  return NOVUS_OK;
}

/* Read the dependency names out of the dependency table. */
static int nkm_collect_deps(nkm_module_t *m, const UINT8 *image, UINT64 len,
                            const nkm_header_t *h, UINT32 dep_off) {
  UINT32 i;

  for (i = 0u; i < h->dep_count; i++) {
    const UINT8 *e = image + dep_off + (UINT64)i * NKM_DEP_SIZE;
    UINT32       name_off = (UINT32)e[0] | ((UINT32)e[1] << 8) |
                      ((UINT32)e[2] << 16) | ((UINT32)e[3] << 24);
    UINT64       min_ver = (UINT64)e[4] | ((UINT64)e[5] << 8) |
                     ((UINT64)e[6] << 16) | ((UINT64)e[7] << 24) |
                     ((UINT64)e[8] << 32) | ((UINT64)e[9] << 40) |
                     ((UINT64)e[10] << 48) | ((UINT64)e[11] << 56);
    UINT32       rsv = (UINT32)e[12] | ((UINT32)e[13] << 8) |
                 ((UINT32)e[14] << 16) | ((UINT32)e[15] << 24);
    const char  *name;

    if (rsv != 0u) {
      return NOVUS_EINVAL;
    }
    if (h->strtab_offset == 0u) {
      return NOVUS_EINVAL; /* a dependency needs a name */
    }
    name = nkm_str(image, len, (UINT32)(h->strtab_offset + name_off));
    if (name == NULL) {
      return NOVUS_EINVAL;
    }
    {
      UINTN j;
      for (j = 0u; j < NKM_NAME_LEN - 1u && name[j] != '\0'; j++) {
        m->deps[i][j] = (UINT8)name[j];
      }
      m->deps[i][j] = 0u;
    }
    /* V1.0 carries no module version in the header, so min_dep_ver has
     * nothing to be compared against; it is recorded and reported, not
     * enforced. */
    (VOID)min_ver;
  }
  m->dep_count = h->dep_count;
  return NOVUS_OK;
}

/* Spec 4: a dependency must already be loaded, and the graph must be
 * acyclic. */
static int nkm_resolve_deps(const nkm_module_t *m) {
  UINT32 i;

  for (i = 0u; i < m->dep_count; i++) {
    const char *name = (const char *)m->deps[i];
    int         dep;

    /* Depending on yourself is the shortest cycle there is. */
    {
      UINTN j;
      int   same = 1;

      for (j = 0u; j < NKM_NAME_LEN; j++) {
        if (m->name[j] != m->deps[i][j]) {
          same = 0;
          break;
        }
        if (m->name[j] == 0u) {
          break;
        }
      }
      if (same) {
        return NOVUS_EDEP;
      }
    }

    dep = nkm_find(name);
    if (dep < 0) {
      return NOVUS_ENOTFOUND;
    }
    /* A module that (transitively) depends on the one being loaded is a
     * cycle too, and can only be observed after both are registered. */
    {
      const nkm_module_t *d = nkm_get((UINTN)dep);
      UINT32              k;

      if (d != NULL) {
        for (k = 0u; k < d->dep_count; k++) {
          UINTN j;
          int   same = 1;

          for (j = 0u; j < NKM_NAME_LEN; j++) {
            if (d->deps[k][j] != m->name[j]) {
              same = 0;
              break;
            }
            if (d->deps[k][j] == 0u) {
              break;
            }
          }
          if (same) {
            return NOVUS_EDEP;
          }
        }
      }
    }
  }
  return NOVUS_OK;
}

/* ------------------------------------------------------------------ */
/* Mapping and relocation                                              */
/* ------------------------------------------------------------------ */

/* Copy every segment into a freshly allocated region and record where
 * each one landed. */
static int nkm_map_segments(nkm_module_t *m, const UINT8 *image, UINT64 len,
                            const nkm_header_t *h, UINT32 seg_off,
                            UINT64 data_min) {
  UINT32 i;
  UINT64 cursor = 0u;
  UINT64 pages;

  for (i = 0u; i < h->seg_count; i++) {
    const UINT8 *e = image + seg_off + (UINT64)i * NKM_SEGMENT_SIZE;
    nkm_segment_t s;

    memcpy(&s, e, sizeof(s));
    if (s.type == 0u || s.type > NKM_SEG_BSS) {
      return NOVUS_EINVAL;
    }
    if (s.len == 0u || s.len > (64u * 1024u * 1024u)) {
      return NOVUS_EINVAL;
    }
    if (s.file_off != 0u) {
      if ((UINT64)s.file_off < data_min || s.file_off + s.len > len) {
        return NOVUS_EINVAL;
      }
    }
    m->seg_type[i] = s.type;
    m->seg_flags[i] = s.flags;
    m->seg_len[i] = s.len;
    m->seg_load[i] = cursor;
    cursor = nkm_align_up(cursor + s.len, NKM_SEG_ALIGN);
  }

  pages = cursor / MM_PAGE_SIZE;
  if (pages == 0u) {
    pages = 1u;
  }
  m->region = nkm_alloc_region((UINTN)pages);
  if (m->region == NULL) {
    return NOVUS_ENOMEM;
  }
  m->region_pages = (UINTN)pages;

  for (i = 0u; i < h->seg_count; i++) {
    const UINT8 *e = image + seg_off + (UINT64)i * NKM_SEGMENT_SIZE;
    nkm_segment_t s;

    memcpy(&s, e, sizeof(s));
    if (s.file_off != 0u) {
      memcpy(m->region + m->seg_load[i], image + s.file_off, (UINTN)s.len);
    }
    /* file_off == 0 is BSS: the pages are already zero. */
  }
  m->seg_count = h->seg_count;
  return NOVUS_OK;
}

/* Bind every relocation. An undefined symbol is looked up in the
 * kernel's export table; a defined one resolves to its segment inside
 * the module's own region. */
static int nkm_relocate(nkm_module_t *m, const UINT8 *image, UINT64 len,
                        const nkm_header_t *h, UINT32 sym_off) {
  UINT64 region_bytes = (UINT64)m->region_pages * MM_PAGE_SIZE;
  UINT32 i;

  for (i = 0u; i < h->rel_count; i++) {
    const UINT8 *e = image + (UINT64)h->rel_offset + (UINT64)i * NKM_REL_SIZE;
    nkm_rel_t    r;
    nkm_sym_t    sym;
    UINT64       value = 0u;
    UINT64       where;
    UINTN        j;

    memcpy(&r, e, sizeof(r));
    if (r.type != NKM_REL_ABS64) {
      return NOVUS_EINVAL; /* PC32 has no meaning in a kernel module */
    }
    if (r.sym >= h->symtab_count) {
      return NOVUS_EINVAL;
    }
    if (r.where + 8u > region_bytes) {
      return NOVUS_EINVAL;
    }

    memcpy(&sym, image + sym_off + (UINT64)r.sym * NKM_SYM_SIZE,
           sizeof(sym));
    if (sym.reserved != 0u) {
      return NOVUS_EINVAL;
    }

    if ((sym.info & 0x0Fu) == NKM_SYM_EXTERN) {
      const char *name;

      if (h->strtab_offset == 0u) {
        return NOVUS_EINVAL;
      }
      name = nkm_str(image, len, (UINT32)(h->strtab_offset + sym.name_off));
      if (name == NULL) {
        return NOVUS_EINVAL;
      }
      {
        VOID *addr = nksym_lookup(name);

        if (addr == NULL) {
          return NOVUS_ENOTFOUND;
        }
        value = (UINT64)(UINTN)addr;
      }
    } else {
      UINT32 seg = (UINT32)((sym.info >> 4) & 0x0Fu);

      if (seg >= m->seg_count) {
        return NOVUS_EINVAL;
      }
      if (sym.value > m->seg_len[seg]) {
        return NOVUS_EINVAL;
      }
      value = (UINT64)(UINTN)(m->region + m->seg_load[seg]) + sym.value;
    }

    where = r.where;
    for (j = 0u; j < 8u; j++) {
      m->region[where + j] = (UINT8)((value >> (j * 8u)) & 0xffu);
    }
  }
  return NOVUS_OK;
}

/* ------------------------------------------------------------------ */
/* Calling into module code                                            */
/* ------------------------------------------------------------------ */
/* Module code is foreign: it was not compiled with this kernel and it
 * obeys the SysV ABI, which means it may trash every caller-saved
 * register. The compiler has no way to know that an indirect call
 * through a pointer it cannot see is any different from a call to a
 * function it compiled - so it keeps live values in those registers.
 * This barrier spells the clobbers out: anything the loader still needs
 * after the call has to live in a callee-saved register or in memory.
 *
 * Without it the first module whose init touches %rdi (as any function
 * taking an argument does) returns into a loader that then indexes the
 * module table with an address instead of a slot. */

#define NKM_CALL_CLOBBERS                                                     \
  "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"

static int nkm_call_init(int (*fn)(void)) {
  int rv = fn();

  __asm__ volatile("" : : : NKM_CALL_CLOBBERS);
  return rv;
}

static int nkm_call_exit(int (*fn)(void)) {
  int rv = fn();

  __asm__ volatile("" : : : NKM_CALL_CLOBBERS);
  return rv;
}

/* ------------------------------------------------------------------ */
/* Load                                                                */
/* ------------------------------------------------------------------ */

int nkm_load(const void *image, UINT64 len, UINT32 flags) {
  const UINT8  *src = (const UINT8 *)image;
  nkm_header_t  hdr;
  UINT8        *copy;
  UINT8         digest[NKM_HASH_LEN];
  UINT8         stored_hash[NKM_HASH_LEN];
  UINT8         stored_sig[NKM_SIG_LEN];
  UINT32        dep_off;
  UINT32        seg_off;
  UINT32        sym_off;
  UINT64        data_min;
  UINTN         slot;
  int           rc;
  int           verified = 0;

  if (image == NULL || len < NKM_HEADER_SIZE) {
    return NOVUS_EINVAL;
  }
  memcpy(&hdr, src, sizeof(hdr));

  /* 4. magic, 5. ABI, header size. */
  if (hdr.magic[0] != (UINT8)NKM_MAGIC0 || hdr.magic[1] != (UINT8)NKM_MAGIC1 ||
      hdr.magic[2] != (UINT8)NKM_MAGIC2 || hdr.magic[3] != (UINT8)NKM_MAGIC3) {
    return NOVUS_EINVAL;
  }
  if ((UINT16)(hdr.abi_version >> 8) != (UINT16)NKM_ABI_MAJOR) {
    return NOVUS_EINVAL;
  }
  if (hdr.header_size != (UINT16)NKM_HEADER_SIZE) {
    return NOVUS_EINVAL;
  }
  {
    UINTN i;
    for (i = 0u; i < sizeof(hdr.reserved); i++) {
      if (hdr.reserved[i] != 0u) {
        return NOVUS_EINVAL;
      }
    }
    for (i = 0u; i < NKM_NAME_LEN; i++) {
      if (hdr.module_name[i] == 0u) {
        break;
      }
    }
    if (i == 0u || i >= NKM_NAME_LEN) {
      return NOVUS_EINVAL; /* empty or unterminated name */
    }
  }

  /* 6. kernel version. */
  if (hdr.min_kernel_ver > (UINT64)NOVUS_KERNEL_VERSION) {
    return NOVUS_EINVAL;
  }

  rc = nkm_check_layout(&hdr, len, &dep_off, &seg_off, &sym_off, &data_min);
  if (rc != NOVUS_OK) {
    return rc;
  }

  for (slot = 0u; slot < NKM_MAX_MODULES; slot++) {
    if (!nkm_modules[slot].in_use) {
      break;
    }
  }
  if (slot >= NKM_MAX_MODULES) {
    return NOVUS_ENOMEM;
  }

  /* 7. Private copy: the digest rule needs hash and sig to read as
   * zeroes, and the caller's buffer is not ours to modify. */
  copy = (UINT8 *)kmalloc(len);
  if (copy == NULL) {
    return NOVUS_ENOMEM;
  }
  memcpy(copy, src, len);
  memcpy(stored_hash, hdr.hash, NKM_HASH_LEN);
  memcpy(stored_sig, hdr.sig, NKM_SIG_LEN);

  /* 8. Digest over the whole file with hash and sig zeroed. */
  memset(copy + NKM_ZERO_OFF, 0, NKM_ZERO_LEN);
  novus_rs_sha256(copy, len, digest);
  if (memcmp(digest, stored_hash, NKM_HASH_LEN) != 0) {
    kfree(copy);
    return NOVUS_ECORRUPT;
  }

  /* 9. Signature over the 32 digest bytes, against every trusted key. */
  /* The compiled-in key pool is the trust boundary for normal loads: the
   * image cannot nominate its own verifier key. The explicit unsigned flag
   * bypasses only this authenticity check; digest and layout validation
   * still run, but callers must treat that mode as untrusted code. */
  if ((flags & NKM_FLAG_ALLOW_UNSIGNED) != 0u) {
    kputs("[NKM] WARNING: signature check skipped by request\n");
    verified = -1;
  } else {
    UINTN k;

    for (k = 0u; k < nkm_trusted_key_count; k++) {
      if (novus_rs_ed25519_verify(stored_sig, digest, NKM_HASH_LEN,
                                  nkm_trusted_keys[k]) != 0u) {
        verified = 1;
        break;
      }
    }
    if (!verified) {
      kfree(copy);
      return NOVUS_EMODULE;
    }
  }

  {
    nkm_module_t *m = &nkm_modules[slot];

    memset(m, 0, sizeof(*m));
    memcpy(m->name, hdr.module_name, NKM_NAME_LEN);
    m->module_type = hdr.module_type;
    m->min_kernel_ver = hdr.min_kernel_ver;
    m->key_index = (verified > 0) ? 0u : 0xFFFFFFFFu;
    m->image = copy;
    m->image_len = len;
    memcpy(m->digest, digest, NKM_HASH_LEN);

    /* 10. Dependencies. */
    rc = nkm_collect_deps(m, copy, len, &hdr, dep_off);
    if (rc != NOVUS_OK) {
      goto fail;
    }
    rc = nkm_resolve_deps(m);
    if (rc != NOVUS_OK) {
      goto fail;
    }

    /* 11. Map the segments. */
    rc = nkm_map_segments(m, copy, len, &hdr, seg_off, data_min);
    if (rc != NOVUS_OK) {
      goto fail;
    }

    /* 12. Relocate. */
    rc = nkm_relocate(m, copy, len, &hdr, sym_off);
    if (rc != NOVUS_OK) {
      goto fail;
    }

    /* 13. Start it. */
    {
      UINT64 region_bytes = (UINT64)m->region_pages * MM_PAGE_SIZE;

      if (hdr.init_offset != 0u &&
          (UINT64)hdr.init_offset >= region_bytes) {
        rc = NOVUS_EINVAL;
        goto fail;
      }
      if (hdr.exit_offset != 0u &&
          (UINT64)hdr.exit_offset >= region_bytes) {
        rc = NOVUS_EINVAL;
        goto fail;
      }
      m->init = (int (*)(void))(VOID *)(m->region + hdr.init_offset);
      m->exit = (int (*)(void))(VOID *)(m->region + hdr.exit_offset);
    }
    if (m->init != NULL) {
      int rv = nkm_call_init(m->init);

      if (rv != 0) {
        nkm_free_region(m->region, m->region_pages);
        m->region = NULL;
        m->region_pages = 0u;
        rc = NOVUS_EINVAL;
        goto fail;
      }
    }
    m->in_use = 1u;
  }
  return (int)slot;

fail:
  {
    nkm_module_t *m = &nkm_modules[slot];

    if (m->region != NULL) {
      nkm_free_region(m->region, m->region_pages);
    }
    kfree(m->image);
    memset(m, 0, sizeof(*m));
  }
  return rc;
}

int nkm_unload(UINTN slot) {
  nkm_module_t *m;
  UINTN         i;
  UINT32        k;

  if (slot >= NKM_MAX_MODULES || !nkm_modules[slot].in_use) {
    return NOVUS_EINVAL;
  }

  /* 3. Anything that still depends on this module keeps it alive. */
  for (i = 0u; i < NKM_MAX_MODULES; i++) {
    if (i == slot || !nkm_modules[i].in_use) {
      continue;
    }
    for (k = 0u; k < nkm_modules[i].dep_count; k++) {
      UINTN j;
      int   same = 1;

      for (j = 0u; j < NKM_NAME_LEN; j++) {
        if (nkm_modules[i].deps[k][j] != nkm_modules[slot].name[j]) {
          same = 0;
          break;
        }
        if (nkm_modules[i].deps[k][j] == 0u) {
          break;
        }
      }
      if (same) {
        return NOVUS_EBUSY;
      }
    }
  }

  m = &nkm_modules[slot];

  /* 4. Stop it. */
  if (m->exit != NULL) {
    (VOID)nkm_call_exit(m->exit);
  }
  /* 5-7. Unmap, unregister, free. */
  nkm_free_region(m->region, m->region_pages);
  kfree(m->image);
  memset(m, 0, sizeof(*m));
  return NOVUS_OK;
}

int nkm_find(const char *name) {
  UINTN i;

  if (name == NULL) {
    return -1;
  }
  for (i = 0u; i < NKM_MAX_MODULES; i++) {
    if (!nkm_modules[i].in_use) {
      continue;
    }
    {
      UINTN j;
      int   same = 1;

      for (j = 0u; j < NKM_NAME_LEN; j++) {
        char a = (char)nkm_modules[i].name[j];
        char b = name[j];

        if (a != b) {
          same = 0;
          break;
        }
        if (a == '\0') {
          break;
        }
      }
      if (same) {
        return (int)i;
      }
    }
  }
  return -1;
}

UINTN nkm_loaded_count(void) {
  UINTN n = 0u;
  UINTN i;

  for (i = 0u; i < NKM_MAX_MODULES; i++) {
    if (nkm_modules[i].in_use) {
      n++;
    }
  }
  return n;
}

const nkm_module_t *nkm_get(UINTN slot) {
  if (slot >= NKM_MAX_MODULES || !nkm_modules[slot].in_use) {
    return NULL;
  }
  return &nkm_modules[slot];
}

const char *nkm_err_text(int rc) {
  switch (rc) {
  case NOVUS_OK:
    return "ok";
  case NOVUS_EINVAL:
    return "invalid header or layout";
  case NOVUS_ENOMEM:
    return "out of memory";
  case NOVUS_ENOTFOUND:
    return "dependency or symbol not found";
  case NOVUS_EPERM:
    return "capability required";
  case NOVUS_EBUSY:
    return "module still in use";
  case NOVUS_EIO:
    return "image unreadable";
  case NOVUS_ECORRUPT:
    return "digest mismatch";
  case NOVUS_EMODULE:
    return "signature verification failed";
  case NOVUS_EDEP:
    return "dependency cycle";
  default:
    return "unknown";
  }
}

/* ------------------------------------------------------------------ */
/* Self test                                                           */
/* ------------------------------------------------------------------ */

static int nkm_check(const char *what, int ok) {
  kputs("[NKM]  ");
  kputs(ok ? "PASS " : "FAIL ");
  kputs(what);
  kputs("\n");
  return ok ? 0 : -1;
}

static void kputs_hex_prefix(const UINT8 *bytes, UINTN n) {
  static const char digits[] = "0123456789abcdef";
  UINTN i;

  for (i = 0u; i < n; i++) {
    char pair[3];

    pair[0] = digits[(bytes[i] >> 4) & 0x0Fu];
    pair[1] = digits[bytes[i] & 0x0Fu];
    pair[2] = '\0';
    kputs(pair);
  }
}

/* Make an image digest-correct again after tampering with a field, so a
 * test can aim at one specific check instead of tripping the digest
 * first. */
static void nkm_reseal(UINT8 *img, UINT64 len) {
  UINT8 digest[NKM_HASH_LEN];

  memset(img + NKM_ZERO_OFF, 0, NKM_ZERO_LEN);
  novus_rs_sha256(img, len, digest);
  memcpy(img + NKM_HASH_OFF, digest, NKM_HASH_LEN);
}

int nkm_selftest(void) {
  static UINT8 img[NKM_TEST_IMAGE_MAX_LEN + 64u];
  int          fails = 0;
  UINTN        before = nkm_loaded_count();
  UINT64       len;

  kputs("[NKM] === NKM loader selftest ===\n");

  /* 1. Truncated: shorter than a header. */
  fails += nkm_check("reject truncated image",
                     nkm_load(img, NKM_HEADER_SIZE - 1u, NKM_FLAG_NONE) ==
                         NOVUS_EINVAL);

  /* 2. Bad magic. */
  len = NKM_TEST_MODULE_LEN;
  memcpy(img, nkm_test_module, (UINTN)len);
  img[0] ^= 0x01u;
  fails += nkm_check("reject bad magic",
                     nkm_load(img, len, NKM_FLAG_NONE) == NOVUS_EINVAL);

  /* 3. ABI major version the loader does not speak. */
  memcpy(img, nkm_test_module, (UINTN)len);
  img[5] = 0x09u; /* abi_version high byte */
  nkm_reseal(img, len);
  fails += nkm_check("reject incompatible ABI version",
                     nkm_load(img, len, NKM_FLAG_NONE) == NOVUS_EINVAL);

  /* 4. Wrong header size. */
  memcpy(img, nkm_test_module, (UINTN)len);
  img[6] = 0x80u; /* header_size = 128, the stale value in the spec */
  img[7] = 0x00u;
  nkm_reseal(img, len);
  fails += nkm_check("reject wrong header size",
                     nkm_load(img, len, NKM_FLAG_NONE) == NOVUS_EINVAL);

  /* 5. Non-zero reserved bytes. */
  memcpy(img, nkm_test_module, (UINTN)len);
  img[0xB0u] = 0x01u;
  nkm_reseal(img, len);
  fails += nkm_check("reject non-zero reserved",
                     nkm_load(img, len, NKM_FLAG_NONE) == NOVUS_EINVAL);

  /* 6. Kernel version newer than this kernel. */
  memcpy(img, nkm_test_module, (UINTN)len);
  memset(img + 0x08u, 0xffu, 8u); /* min_kernel_ver */
  nkm_reseal(img, len);
  fails += nkm_check("reject module requiring a newer kernel",
                     nkm_load(img, len, NKM_FLAG_NONE) == NOVUS_EINVAL);

  /* 7. Segment table past the end of the image. */
  memcpy(img, nkm_test_module, (UINTN)len);
  {
    UINT16 sc = (UINT16)(img[0x14u] | ((UINT16)img[0x15u] << 8));

    img[0x14u] = (UINT8)(sc + 40u);
  }
  nkm_reseal(img, len);
  fails += nkm_check("reject out-of-bounds segment table",
                     nkm_load(img, len, NKM_FLAG_NONE) == NOVUS_EINVAL);

  /* 8. Digest mismatch: one flipped byte anywhere in the payload. */
  memcpy(img, nkm_test_module, (UINTN)len);
  img[len - 1u] ^= 0xffu;
  fails += nkm_check("reject digest mismatch",
                     nkm_load(img, len, NKM_FLAG_NONE) == NOVUS_ECORRUPT);

  /* 9. Digest correct, signature absent. */
  memcpy(img, nkm_test_module, (UINTN)len);
  memset(img + NKM_SIG_OFF, 0, NKM_SIG_LEN);
  nkm_reseal(img, len);
  fails += nkm_check("reject missing signature",
                     nkm_load(img, len, NKM_FLAG_NONE) == NOVUS_EMODULE);
  /* ... and the same image is accepted when an administrator asks for
   * it explicitly (spec 10). The slot is released again below. */
  {
    int slot = nkm_load(img, len, NKM_FLAG_ALLOW_UNSIGNED);

    fails += nkm_check("accept unsigned when explicitly allowed", slot >= 0);
    if (slot >= 0) {
      fails += nkm_check("released again", nkm_unload((UINTN)slot) ==
                                               NOVUS_OK);
    }
  }

  /* 10. Digest correct but signature not: the digest is resealed after
   * flipping a signature byte, so the failure is the signature's and not
   * the digest's. */
  memcpy(img, nkm_test_module, (UINTN)len);
  img[NKM_SIG_OFF] ^= 0x01u;
  nkm_reseal(img, len);
  fails += nkm_check("reject tampered signature",
                     nkm_load(img, len, NKM_FLAG_NONE) == NOVUS_EMODULE);

  /* 11. A dependency that is not loaded. */
  memcpy(img, nkm_test_dep_module, (UINTN)NKM_TEST_DEP_MODULE_LEN);
  fails += nkm_check("reject missing dependency",
                     nkm_load(img, (UINT64)NKM_TEST_DEP_MODULE_LEN,
                              NKM_FLAG_NONE) == NOVUS_ENOTFOUND);

  /* 12. A module that depends on itself. */
  memcpy(img, nkm_test_selfdep_module, (UINTN)NKM_TEST_SELFDEP_MODULE_LEN);
  fails += nkm_check("reject dependency cycle",
                     nkm_load(img, (UINT64)NKM_TEST_SELFDEP_MODULE_LEN,
                              NKM_FLAG_NONE) == NOVUS_EDEP);

  /* 13. A module signed by the host tool: accepted, relocated and
   * started. The module's nkm_init() writes a magic value into its own
   * data segment through a relocated kernel symbol address, so seeing
   * the value proves the whole chain ran. */
  {
    int slot = nkm_load(nkm_test_module, (UINT64)NKM_TEST_MODULE_LEN,
                        NKM_FLAG_NONE);

    fails += nkm_check("accept signed module", slot >= 0);
    if (slot >= 0) {
      const nkm_module_t *m = nkm_get((UINTN)slot);
      UINT64              magic = 0;
      UINTN               j;

      kputs("[NKM]  loaded: name=");
      kputs((const char *)m->name);
      kputs(" type=");
      kputs_hex((UINT64)m->module_type);
      kputs(" segs=");
      kputs_hex((UINT64)m->seg_count);
      kputs(" digest=");
      kputs_hex_prefix(m->digest, 8);
      kputs("\n");

      /* nkm_init() stored the address it resolved for its undefined
       * symbol "kputs" into the first eight bytes of its data segment.
       * Comparing that against the kernel's own export table checks the
       * whole chain at once: the symbol was found, the relocation landed
       * where the code reads it, and the module's own code ran. */
      for (j = 0u; j < 8u; j++) {
        magic |= (UINT64)m->region[m->seg_load[1] + j] << (j * 8u);
      }
      fails += nkm_check("relocation resolved a kernel symbol",
                         magic == (UINT64)(UINTN)nksym_lookup("kputs"));
      fails += nkm_check("module found by name",
                         nkm_find("demo_drv") >= 0);

      /* Unload runs nkm_exit() and frees the slot. */
      fails += nkm_check("unload frees the slot",
                         nkm_unload((UINTN)slot) == NOVUS_OK &&
                             nkm_get((UINTN)slot) == NULL);
    }
  }

  /* 14. Nothing leaked: every refusal, and the module this test loaded,
   * released their slot again. The count is compared against what was
   * loaded before the test rather than against zero, because the kernel
   * may legitimately hold modules of its own (this run stages one). */
  fails += nkm_check("no module left over from the selftest",
                     nkm_loaded_count() == before);

  kputs("[NKM] === selftest ");
  kputs(fails == 0 ? "PASSED" : "FAILED");
  kputs(" ===\n");
  return (fails == 0) ? 0 : -1;
}
