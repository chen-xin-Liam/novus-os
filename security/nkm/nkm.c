/**
 * Novus OS - NKM loader, M3.
 *
 * Load is a straight-line verification with no execution step: M3
 * proves the chain (header -> digest -> signature -> registration) and
 * M4 supplies the syscall surface and the real loader that maps
 * segments and binds symbols.
 *
 * The order of the checks matters. Bounds come first so no pointer
 * arithmetic ever leaves the image; the digest comes before the
 * signature so an unsigned module is refused on its own merits rather
 * than being reported as a signature failure; the signature comes last
 * because it is the only check that costs a double scalar
 * multiplication.
 *
 * Nothing here halts. Every refusal frees what it allocated, leaves
 * the module table untouched and returns a reason the caller logs.
 */

#include "nkm.h"
#include "heap.h"
#include "../../lib/novuslib_rs.h"

/* The generated test module, produced by tools/nkmgen from the same
 * crypto the kernel verifies with. It is what lets the self test cover
 * the accept path; the refusal paths are built at run time and need no
 * signing key. */
#include "nkm_test_module.h"

/* The on-disk layout is frozen: the host signing tool and this file
 * must agree on it byte for byte. */
_Static_assert(sizeof(nkm_header_t) == NKM_HEADER_SIZE,
               "nkm_header_t layout drifted from NKM_HEADER_SIZE");
_Static_assert(sizeof(nkm_segment_t) == NKM_SEGMENT_SIZE,
               "nkm_segment_t layout drifted from NKM_SEGMENT_SIZE");
_Static_assert(NKM_HASH_OFF + NKM_HASH_LEN == NKM_SIG_OFF,
               "hash and sig must be adjacent for the zeroing rule");
_Static_assert(NKM_SIG_OFF + NKM_SIG_LEN == NKM_HEADER_SIZE,
               "sig must end the header");

/* ------------------------------------------------------------------ */
/* Trusted root keys                                                   */
/* ------------------------------------------------------------------ */
/* key_id -> public key. Rotation works by publishing a new key_id and
 * shipping modules signed with it; old modules keep verifying because
 * their entry stays in the table. An id of 0 is never valid, which is
 * what makes an all-zero header fail rather than accidentally match. */

static const UINT8 nkm_root_key_dev[NKM_PUBKEY_LEN] = {
    0x7a, 0x33, 0x7a, 0x9e, 0x91, 0xbd, 0xdf, 0xf2,
    0x4f, 0xd4, 0x88, 0xad, 0x6e, 0x9f, 0x57, 0xcd,
    0x12, 0xbc, 0x6d, 0x5c, 0x01, 0xbe, 0x64, 0x9c,
    0x39, 0x37, 0xee, 0xc3, 0xa3, 0x44, 0x20, 0x93,
};

typedef struct {
  UINT32       key_id;
  const UINT8 *pub;
} nkm_root_key_t;

static const nkm_root_key_t nkm_root_keys[NKM_MAX_ROOT_KEYS] = {
    {1u, nkm_root_key_dev},
};

static const UINT8 *nkm_find_key(UINT32 key_id) {
  UINTN i;

  if (key_id == 0u) {
    return NULL;
  }
  for (i = 0; i < NKM_MAX_ROOT_KEYS; i++) {
    if (nkm_root_keys[i].key_id == key_id && nkm_root_keys[i].pub != NULL) {
      return nkm_root_keys[i].pub;
    }
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Module table                                                        */
/* ------------------------------------------------------------------ */

static nkm_module_t nkm_modules[NKM_MAX_MODULES];

/* ------------------------------------------------------------------ */
/* Verification                                                        */
/* ------------------------------------------------------------------ */

/* Every table and segment must lie inside the image. Returns 0 on
 * success, NKM_REJECT_LAYOUT otherwise. */
static int nkm_check_bounds(const nkm_header_t *h, UINT64 len) {
  UINT64 seg_end;

  if (h->seg_count == 0u || h->seg_count > NKM_MAX_SEGMENTS) {
    return NKM_REJECT_LAYOUT;
  }
  if (h->seg_table_off < NKM_HEADER_SIZE) {
    return NKM_REJECT_LAYOUT;
  }
  seg_end = h->seg_table_off + (UINT64)h->seg_count * NKM_SEGMENT_SIZE;
  if (seg_end > len) {
    return NKM_REJECT_LAYOUT;
  }
  /* Optional tables: zero offset with zero length means "absent". */
  if (h->symtab_off != 0u && h->symtab_off + h->symtab_len > len) {
    return NKM_REJECT_LAYOUT;
  }
  if (h->reloc_off != 0u && h->reloc_off + h->reloc_len > len) {
    return NKM_REJECT_LAYOUT;
  }
  /* No table may overlap the header: the header is hashed in place and
   * the digest rule assumes it sits at the front. */
  if ((h->symtab_len != 0u && h->symtab_off < NKM_HEADER_SIZE) ||
      (h->reloc_len != 0u && h->reloc_off < NKM_HEADER_SIZE)) {
    return NKM_REJECT_LAYOUT;
  }
  return 0;
}

/* Copy every segment descriptor into the module record after checking
 * that its payload fits in the image. */
static int nkm_collect_segments(nkm_module_t *m, const UINT8 *image,
                                UINT64 len, const nkm_header_t *h) {
  UINT32 i;

  for (i = 0; i < h->seg_count; i++) {
    const UINT8 *e = image + h->seg_table_off + (UINT64)i * NKM_SEGMENT_SIZE;
    nkm_segment_t *s = &m->segs[i];

    memcpy(s, e, sizeof(*s));
    /* file_off == 0 is a BSS-style segment: nothing to read. */
    if (s->file_off != 0u) {
      if (s->file_off < NKM_HEADER_SIZE || s->file_off + s->len > len) {
        return NKM_REJECT_LAYOUT;
      }
    }
  }
  m->seg_count = h->seg_count;
  return 0;
}

int nkm_load(const void *image, UINT64 len) {
  const UINT8  *src = (const UINT8 *)image;
  nkm_header_t  hdr;
  UINT8        *copy;
  UINT8         digest[NKM_HASH_LEN];
  const UINT8  *pub;
  UINTN         slot;
  int           rc;

  if (image == NULL || len < NKM_HEADER_SIZE) {
    return NKM_REJECT_MALFORMED;
  }
  memcpy(&hdr, src, sizeof(hdr));

  if (hdr.magic != NKM_MAGIC || hdr.version != NKM_VERSION ||
      hdr.abi_version != NKM_ABI_VERSION) {
    return NKM_REJECT_MALFORMED;
  }
  if ((hdr.compile_flag & NKM_CF_PIC) == 0u) {
    return NKM_REJECT_MALFORMED; /* relocatable or nothing: M3 loads PIC */
  }

  rc = nkm_check_bounds(&hdr, len);
  if (rc != 0) {
    return rc;
  }

  /* Private copy: the digest rule requires hash and sig to read as
   * zeroes, and the caller's buffer is not ours to modify. */
  copy = (UINT8 *)kmalloc(len);
  if (copy == NULL) {
    return NKM_REJECT_NOMEM;
  }
  memcpy(copy, src, len);
  memset(copy + NKM_HASH_OFF, 0, NKM_HASH_LEN);
  memset(copy + NKM_SIG_OFF, 0, NKM_SIG_LEN);
  novus_rs_sha256(copy, len, digest);

  if (memcmp(digest, hdr.hash, NKM_HASH_LEN) != 0) {
    kfree(copy);
    return NKM_REJECT_HASH;
  }

  pub = nkm_find_key(hdr.key_id);
  if (pub == NULL) {
    kfree(copy);
    return NKM_REJECT_KEY;
  }

  /* The signature covers the 32 digest bytes, not the module. */
  if (novus_rs_ed25519_verify(hdr.sig, digest, NKM_HASH_LEN, pub) == 0u) {
    kfree(copy);
    return NKM_REJECT_SIG;
  }

  for (slot = 0; slot < NKM_MAX_MODULES; slot++) {
    if (!nkm_modules[slot].in_use) {
      break;
    }
  }
  if (slot >= NKM_MAX_MODULES) {
    kfree(copy);
    return NKM_REJECT_NOSLOT;
  }

  {
    nkm_module_t *m = &nkm_modules[slot];

    memset(m, 0, sizeof(*m));
    rc = nkm_collect_segments(m, copy, len, &hdr);
    if (rc != 0) {
      kfree(copy);
      return rc;
    }
    m->in_use = 1;
    m->key_id = hdr.key_id;
    m->compile_flag = hdr.compile_flag;
    m->stack_size = hdr.stack_size;
    m->image = copy;
    m->image_len = len;
    memcpy(m->digest, digest, NKM_HASH_LEN);
  }
  return (int)slot;
}

int nkm_unload(UINTN slot) {
  nkm_module_t *m;

  if (slot >= NKM_MAX_MODULES || !nkm_modules[slot].in_use) {
    return -1;
  }
  m = &nkm_modules[slot];
  if (m->image != NULL) {
    kfree(m->image);
    m->image = NULL;
  }
  memset(m, 0, sizeof(*m));
  return 0;
}

UINTN nkm_loaded_count(void) {
  UINTN n = 0;
  UINTN i;

  for (i = 0; i < NKM_MAX_MODULES; i++) {
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

const char *nkm_reject_text(int rc) {
  switch (rc) {
  case NKM_REJECT_MALFORMED:
    return "malformed header";
  case NKM_REJECT_LAYOUT:
    return "tables or segments out of bounds";
  case NKM_REJECT_HASH:
    return "digest mismatch";
  case NKM_REJECT_KEY:
    return "unknown root key id";
  case NKM_REJECT_SIG:
    return "signature verification failed";
  case NKM_REJECT_NOSLOT:
    return "module table full";
  case NKM_REJECT_NOMEM:
    return "out of memory";
  default:
    return "unknown";
  }
}

/* First `n` bytes of a digest, hex, for the log. Novus prints only a
 * prefix: a full 32-byte hash per line buys an operator nothing. */
static void kputs_hex_prefix(const UINT8 *bytes, UINTN n) {
  static const char digits[] = "0123456789abcdef";
  UINTN i;

  for (i = 0; i < n; i++) {
    char pair[3];
    pair[0] = digits[(bytes[i] >> 4) & 0x0Fu];
    pair[1] = digits[bytes[i] & 0x0Fu];
    pair[2] = '\0';
    kputs(pair);
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

/* Build a structurally valid module in `buf` and give it the digest
 * that the loader will compute. `key_id`, `bad_hash` and `bad_sig`
 * select which check the result is meant to trip.
 *
 * Layout: header (156) | one segment (32) | 64 bytes of payload. */
#define NKM_TEST_PAYLOAD_OFF (NKM_HEADER_SIZE + NKM_SEGMENT_SIZE)
#define NKM_TEST_PAYLOAD_LEN 64u
#define NKM_TEST_IMAGE_LEN   (NKM_TEST_PAYLOAD_OFF + NKM_TEST_PAYLOAD_LEN)

static UINT64 nkm_build_test(UINT8 *buf, UINT32 key_id, int bad_hash,
                             int bad_sig) {
  nkm_header_t  h;
  nkm_segment_t s;
  UINT64        i;

  memset(buf, 0, NKM_TEST_IMAGE_LEN);
  for (i = 0; i < NKM_TEST_PAYLOAD_LEN; i++) {
    buf[NKM_TEST_PAYLOAD_OFF + i] = (UINT8)(i * 5u + 1u);
  }

  memset(&s, 0, sizeof(s));
  s.type = NKM_SEG_CODE;
  s.vaddr = 0x100000u;
  s.file_off = NKM_TEST_PAYLOAD_OFF;
  s.len = NKM_TEST_PAYLOAD_LEN;
  s.flags = NKM_SEG_EXEC;
  memcpy(buf + NKM_HEADER_SIZE, &s, sizeof(s));

  memset(&h, 0, sizeof(h));
  h.magic = NKM_MAGIC;
  h.version = NKM_VERSION;
  h.abi_version = NKM_ABI_VERSION;
  h.seg_count = 1u;
  h.seg_table_off = NKM_HEADER_SIZE;
  h.symtab_off = 0;
  h.symtab_len = 0;
  h.reloc_off = 0;
  h.reloc_len = 0;
  h.stack_size = 8192u;
  h.compile_flag = NKM_CF_PIC;
  h.key_id = key_id;
  memcpy(buf, &h, sizeof(h));

  /* The digest the loader will compute: hash and sig zeroed. */
  novus_rs_sha256(buf, NKM_TEST_IMAGE_LEN, buf + NKM_HASH_OFF);
  if (bad_hash) {
    buf[NKM_HASH_OFF] ^= 0xffu;
  }
  if (bad_sig) {
    memset(buf + NKM_SIG_OFF, 0, NKM_SIG_LEN);
  }
  return NKM_TEST_IMAGE_LEN;
}

int nkm_selftest(void) {
  static UINT8 img[NKM_TEST_IMAGE_LEN + 64u];
  int fails = 0;
  int rc;
  UINT64 len;

  kputs("[NKM] === NKM loader selftest ===\n");

  /* 1. Truncated: shorter than a header. */
  fails += nkm_check("reject truncated image",
                     nkm_load(img, NKM_HEADER_SIZE - 1u) ==
                         NKM_REJECT_MALFORMED);

  /* 2. Bad magic. */
  len = nkm_build_test(img, 1u, 0, 0);
  img[0] ^= 0x01u;
  fails += nkm_check("reject bad magic",
                     nkm_load(img, len) == NKM_REJECT_MALFORMED);

  /* 3. Non-PIC is not loadable today. */
  len = nkm_build_test(img, 1u, 0, 0);
  memset(img + 60u, 0, 4u); /* compile_flag */
  fails += nkm_check("reject non-PIC module",
                     nkm_load(img, len) == NKM_REJECT_MALFORMED);

  /* 4. Segment table past the end of the image. */
  len = nkm_build_test(img, 1u, 0, 0);
  {
    UINT64 off = len + 4096u;
    memcpy(img + 12u, &off, 8u); /* seg_table_off */
  }
  fails += nkm_check("reject out-of-bounds segment table",
                     nkm_load(img, len) == NKM_REJECT_LAYOUT);

  /* 5. Digest mismatch: the stored hash does not match the content. */
  len = nkm_build_test(img, 1u, 1, 0);
  fails += nkm_check("reject digest mismatch",
                     nkm_load(img, len) == NKM_REJECT_HASH);

  /* 6. Unknown root key id. */
  len = nkm_build_test(img, 0xdeadbeefu, 0, 0);
  fails += nkm_check("reject unknown root key id",
                     nkm_load(img, len) == NKM_REJECT_KEY);

  /* 7. Digest correct, signature absent. */
  len = nkm_build_test(img, 1u, 0, 1);
  fails += nkm_check("reject missing signature",
                     nkm_load(img, len) == NKM_REJECT_SIG);

  /* 8. A module signed by the host tool must be accepted. A writable
   * copy is used because the tampering check below flips a byte. */
  {
    static UINT8 mod[NKM_TEST_MODULE_LEN];

    memcpy(mod, nkm_test_module, NKM_TEST_MODULE_LEN);
    rc = nkm_load(mod, NKM_TEST_MODULE_LEN);
    fails += nkm_check("accept signed module", rc >= 0);
    if (rc >= 0) {
      const nkm_module_t *m = nkm_get((UINTN)rc);

      kputs("[NKM]  loaded: key_id=");
      kputs_hex(m->key_id);
      kputs(" segs=");
      kputs_hex(m->seg_count);
      kputs(" digest=");
      kputs_hex_prefix(m->digest, 8);
      kputs("\n");

      /* One flipped byte in the payload must be caught by the digest,
       * before the signature is even consulted. */
      mod[NKM_TEST_PAYLOAD_OFF] ^= 0xffu;
      fails += nkm_check("reject tampered signed module",
                         nkm_load(mod, NKM_TEST_MODULE_LEN) ==
                             NKM_REJECT_HASH);
      mod[NKM_TEST_PAYLOAD_OFF] ^= 0xffu;

      fails += nkm_check("unload frees the slot",
                         nkm_unload((UINTN)rc) == 0 &&
                         nkm_get((UINTN)rc) == NULL);
    }
  }

  kputs("[NKM] === selftest ");
  kputs(fails == 0 ? "PASSED" : "FAILED");
  kputs(" ===\n");
  return (fails == 0) ? 0 : -1;
}
